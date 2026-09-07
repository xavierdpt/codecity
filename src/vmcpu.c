/* vmcpu.c -- a virtual CPU for `cpuid` and `xgetbv` to answer honestly.
 *
 * §4.2 measured this: `git`, `bash` and our own binary never execute a
 * `cpuid` at all -- for ordinary application code it is a loader
 * phenomenon, done once inside ld.so and written into a struct.  Codec
 * libraries invert that: `libvpx` has fifteen of them, in its own code,
 * with no IFUNC in sight.  A resolver that dereferences ld.so's data
 * cannot be run honestly (§4.2); a `cpuid` can, because it is a pure
 * function of (eax, ecx) and a small table.
 *
 * So this is not a curiosity.  It is the control that decides which kernel
 * a wisp walks into: the same call site, under two profiles, ends up in
 * two different rooms.
 *
 * AVX-512 is absent from every profile on purpose -- it is not modelled
 * (§4.3.1), and a profile that advertised it would send a wisp into code
 * the interpreter would only havoc.
 */
#define _GNU_SOURCE
#include "vm.h"
#include <string.h>

/* ---- CPUID.1:EDX ---------------------------------------------------- */
#define D_FPU   (1u << 0)
#define D_TSC   (1u << 4)
#define D_MSR   (1u << 5)
#define D_PAE   (1u << 6)
#define D_CX8   (1u << 8)
#define D_APIC  (1u << 9)
#define D_SEP   (1u << 11)
#define D_CMOV  (1u << 15)
#define D_CLFSH (1u << 19)
#define D_MMX   (1u << 23)
#define D_FXSR  (1u << 24)
#define D_SSE   (1u << 25)
#define D_SSE2  (1u << 26)

/* ---- CPUID.1:ECX ---------------------------------------------------- */
#define C_SSE3    (1u << 0)
#define C_PCLMUL  (1u << 1)
#define C_SSSE3   (1u << 9)
#define C_FMA     (1u << 12)
#define C_CX16    (1u << 13)
#define C_SSE41   (1u << 19)
#define C_SSE42   (1u << 20)
#define C_MOVBE   (1u << 22)
#define C_POPCNT  (1u << 23)
#define C_AES     (1u << 25)
#define C_XSAVE   (1u << 26)
#define C_OSXSAVE (1u << 27)
#define C_AVX     (1u << 28)
#define C_F16C    (1u << 29)

/* ---- CPUID.7.0:EBX --------------------------------------------------- */
#define B_BMI1  (1u << 3)
#define B_AVX2  (1u << 5)
#define B_BMI2  (1u << 8)

typedef struct {
    const char *name, *what;
    uint32_t maxleaf, sig;
    uint32_t f1edx, f1ecx, f7ebx;
    uint64_t xcr0;
} Profile;

/* The three levels the x86-64 psABI names, which is what a codec's own
   dispatch actually tests for.                                        */
static const Profile PROF[] = {
    { "baseline", "x86-64-v1: SSE2 and nothing after it",
      0x7, 0x000006fb,
      D_FPU|D_TSC|D_MSR|D_PAE|D_CX8|D_APIC|D_SEP|D_CMOV|D_CLFSH|D_MMX|D_FXSR|D_SSE|D_SSE2,
      0,
      0,
      0x3 },
    { "v2", "x86-64-v2: SSE3 through SSE4.2, POPCNT",
      0x7, 0x000106e5,
      D_FPU|D_TSC|D_MSR|D_PAE|D_CX8|D_APIC|D_SEP|D_CMOV|D_CLFSH|D_MMX|D_FXSR|D_SSE|D_SSE2,
      C_SSE3|C_PCLMUL|C_SSSE3|C_CX16|C_SSE41|C_SSE42|C_POPCNT|C_AES,
      0,
      0x3 },
    { "v3", "x86-64-v3: AVX, AVX2, BMI1/2, FMA",
      0xd, 0x000306c3,
      D_FPU|D_TSC|D_MSR|D_PAE|D_CX8|D_APIC|D_SEP|D_CMOV|D_CLFSH|D_MMX|D_FXSR|D_SSE|D_SSE2,
      C_SSE3|C_PCLMUL|C_SSSE3|C_FMA|C_CX16|C_SSE41|C_SSE42|C_MOVBE|C_POPCNT|C_AES|
      C_XSAVE|C_OSXSAVE|C_AVX|C_F16C,
      B_BMI1|B_AVX2|B_BMI2,
      0x7 },
    { "host", "whatever this machine reports", 0, 0, 0, 0, 0, 0 },
};
#define NPROF ((int)(sizeof PROF / sizeof *PROF))

static int g_prof;          /* baseline, deliberately: it maximises the
                               fraction of code the interpreter can model
                               and it is what the compiler assumed */

int vm_cpu_set(const char *name){
    for (int i = 0; i < NPROF; i++)
        if (!strcmp(PROF[i].name, name)){ g_prof = i; return 1; }
    return 0;
}
int         vm_cpu_profile(void){ return g_prof; }
const char *vm_cpu_name(void){ return PROF[g_prof].name; }
const char *vm_cpu_what(void){ return PROF[g_prof].what; }
const char *vm_cpu_nth(int i, const char **what){
    if (i < 0 || i >= NPROF) return NULL;
    if (what) *what = PROF[i].what;
    return PROF[i].name;
}

#if defined(__x86_64__) || defined(__i386__)
static void host_cpuid(uint32_t leaf, uint32_t sub, uint32_t o[4]){
    __asm__ volatile("cpuid" : "=a"(o[0]), "=b"(o[1]), "=c"(o[2]), "=d"(o[3])
                             : "a"(leaf), "c"(sub));
}
#else
static void host_cpuid(uint32_t leaf, uint32_t sub, uint32_t o[4]){
    (void)leaf; (void)sub; o[0] = o[1] = o[2] = o[3] = 0;
}
#endif

void vm_cpuid(uint32_t leaf, uint32_t sub, uint32_t o[4]){
    o[0] = o[1] = o[2] = o[3] = 0;
    if (g_prof == 3){ host_cpuid(leaf, sub, o); return; }
    const Profile *p = &PROF[g_prof];
    switch (leaf){
    case 0:
        o[0] = p->maxleaf;
        o[1] = 0x756e6547; o[3] = 0x49656e69; o[2] = 0x6c65746e;   /* GenuineIntel */
        break;
    case 1:
        o[0] = p->sig;
        o[1] = 0x00100800;                  /* one logical CPU, 64-byte lines */
        o[2] = p->f1ecx;
        o[3] = p->f1edx;
        break;
    case 7:
        if (sub == 0){ o[0] = 0; o[1] = p->f7ebx; }
        break;
    case 0xd:
        if (sub == 0){ o[0] = (uint32_t)p->xcr0; o[2] = 0x340; }
        break;
    case 0x80000000: o[0] = 0x80000008; break;
    case 0x80000001:
        o[2] = 1;                            /* LAHF in long mode */
        o[3] = (1u << 11) | (1u << 20) | (1u << 29);   /* SYSCALL, NX, LM */
        break;
    default: break;                          /* cache and topology: zero */
    }
}

uint64_t vm_xcr0(void){
    if (g_prof == 3){
        uint32_t o[4];
        host_cpuid(0xd, 0, o);
        return ((uint64_t)o[3] << 32) | o[0];
    }
    return PROF[g_prof].xcr0;
}
