/* vm.h -- the CPU state a wisp carries through a code room
 *
 * A snapshot of registers, flags and memory that steps from instruction to
 * instruction along the path the code actually takes.  Nothing here is
 * execution: there is no kernel, no loader, no libc, and the inputs are
 * invented.  What it is, is *consistent*: the first read of anything that
 * has no value mints one from a seeded PRNG and writes it back, so every
 * later read of the same place gives the same answer, and the same room
 * shows the same run every time you walk into it.
 *
 * This is tier 0 -- the fallback that models no instruction semantics at
 * all.  Capstone says which registers an instruction reads and writes and
 * which flags it disturbs; tier 0 materialises the reads, havocs the
 * writes and clears the known bit on the flags.  That is precise
 * ignorance rather than a lie, and because it asks Capstone rather than
 * knowing anything about x86, it works on every architecture Capstone
 * decodes.  Tier 1 (real integer semantics) lands on top of it.
 */
#ifndef VM_H
#define VM_H
#include <stdint.h>
#include <stddef.h>
#include "model.h"

/* where a value came from -- the honesty mechanism.  The failure mode of
   this whole feature is a viewer who believes the numbers.            */
typedef enum { PV_NONE, PV_INVENTED, PV_FILE, PV_DERIVED, PV_CALL } Prov;

typedef enum { ST_FALL, ST_TAKEN, ST_NOT_TAKEN, ST_CALL_OVER, ST_CALL_INTO,
               ST_EXIT_PORT, ST_RET, ST_STOP } StepKind;

/* flags, in the order the panel prints them */
enum { VF_CF, VF_PF, VF_AF, VF_ZF, VF_SF, VF_OF, VF_COUNT };

#define VM_NREG   40        /* tracked registers; x86-64 fills 17 of them   */
#define VM_REGIDS 1024      /* Capstone register ids we are willing to map  */
#define VM_BUCKETS 64       /* shadow-memory hash buckets                   */
#define VM_MAXPG  256       /* hard page cap: 1 MiB, far more than a
                               function touches.  Past it the run stops
                               recording and keeps inventing.               */
#define VM_MEMLOG 8         /* the last few memory references, for the panel */
#define VM_FUEL   4000      /* steps per run, before the wisp fades         */
#define VM_TRIPS  12        /* how many times a guessed loop goes round     */
#define VM_REPMAX 4096      /* §4.5: a `rep` with an invented rcx is a bulk
                               operation of bounded size, not 2^63 steps  */
#define VM_CALLDEPTH 12     /* how deep a followed call may go before the
                               run starts stepping over them again         */
#define VM_STUBS  4096      /* synthetic addresses for symbols not in this
                               file, so a GOT call has a name to reach     */
/* The return address a run starts with.  When `ret` pops this, the function
   has returned to its caller and the run is over -- which is what turns a
   wisp that drifts until its fuel runs out into one that plainly executes a
   function and finishes.  Not a plausible address, on purpose.        */
#define VM_SENTINEL 0x00000000CAFED00Dull

typedef struct { uint64_t v; uint8_t prov; uint32_t stamp; } VReg;

/* §4.4: the vector file is stored 512 bits wide from the first line even
   while only the low 128 are ever written, so widening tier 3 to AVX2 is a
   change to a loop bound and not to the state.                       */
#define VM_NVREG  32
#define VM_VBYTES 64
typedef struct { uint8_t b[VM_VBYTES]; uint8_t prov; uint32_t stamp; } VVec;

typedef struct VmPage {
    uint64_t base;
    struct VmPage *next;
    uint8_t  b[4096];
    uint8_t  known[4096 / 8];
} VmPage;

typedef struct {
    VReg      r[VM_NREG];
    char      rname[VM_NREG][8];
    int       nreg;                 /* slots claimed so far */
    int       ncanon;               /* ... of which the architecture's own,
                                       claimed up front; the rest were met
                                       along the way and are only worth
                                       drawing once something touches them */
    int       pcSlot, spSlot;       /* -1 if this architecture has none named */
    int16_t  *rmap;                 /* Capstone register id -> slot, -1 if none */
    /* the sub-register table: `mov eax, ecx` zeroes the top half of rax and
       `mov al, cl` does not, so every id also carries the width it names and
       where in the slot it sits.  Width 0 means "the whole slot".      */
    uint8_t  *rwid, *roff;

    VVec      v[VM_NVREG];
    int16_t  *vmap;                 /* Capstone id -> vector slot, -1 if none */
    uint8_t  *vwid;                 /* ... and the width in bytes that id names */
    int       nvreg;                /* how many the architecture has */
    /* §4.4: the panel cannot show sixteen 32-byte values and stay
       readable, so it shows the one the last instruction touched, split
       into the lanes that instruction actually operated on.         */
    int       vecSlot;              /* -1 when none */
    uint8_t   vecEw, vecBytes;

    uint8_t   fl[VF_COUNT], flknown;    /* one known bit per flag */

    VmPage   *pg[VM_BUCKETS];
    int       npg, pgFull;          /* pgFull: the cap was hit, we stopped recording */

    uint64_t  stackLo, stackHi, fsBase, heapBase, heapNext;
    uint64_t  sentinel;             /* the return address that ends a run */
    uint64_t  canary;               /* the one value at fs:0x28, minted per run */
    uint64_t  stubNext;             /* next synthetic address to hand out */
    /* on the heap: a Vm is created a few thousand times by the corpus
       check, and this is the only part of it that is not small */
    struct VmStub { uint64_t addr; char name[56]; } *stub;
    int       nstub, capstub;
    uint64_t  seed, rng;
    /* §5's minting bias: what the instruction about to read an unset value
       is going to do with it.  Uniform 64-bit noise makes a loop run 2^40
       times; a value near the immediate it is compared against makes it
       run three.  Cosmetic, and the difference between a run you can watch
       and one that only ever exhausts its fuel.                     */
    uint64_t  mintHint;
    int       mintHintOn;
    const Elf *elf;

    uint32_t  steps;
    int       fuel;
    /* Pilot mode (§8 B2): a call is *made* rather than stepped over -- the
       return address really goes on the synthetic stack, so `ret` pops it
       and comes back with whatever the callee actually left in the
       registers.  §15 sketched a snapshot of the register file for this;
       the stack M3 built makes that unnecessary and less truthful.    */
    int       follow;
    int       ncall;                /* how deep in, so recursion is bounded */

    struct { uint64_t addr, val; uint8_t n, wr, prov; } mem[VM_MEMLOG];
    int       nmem;                 /* total recorded; index = nmem % VM_MEMLOG */

    /* what --vmtest reports */
    uint32_t  nTier0, nTier1, nTier2, nTier3, nCond, nDecided, nGuessed, nNoAccess;
    uint32_t  nCalls, nCallsNamed, nCallsModelled, nRelocs, nCanary;
    char      lastCall[56];         /* what the last stepped-over call was */
    uint64_t  lastCallRv;
} Vm;

void      vm_init(Vm *m, const Elf *e, uint64_t entry, uint64_t seed);
void      vm_free(Vm *m);
/* One step.  `bytes` are the instruction's own bytes and `visits` is how
   many times this wisp has already run it -- §6's loop policy needs it,
   and the Vm deliberately does not keep per-instruction state of its own.
   *next receives the address to continue at; *decided is 0 when a branch
   was guessed rather than worked out, which the renderer colours.      */
StepKind  vm_step(Vm *m, const Insn *in, const uint8_t *bytes, int visits,
                  uint64_t *next, int *decided);

uint64_t  vm_read(Vm *m, uint64_t a, int n, Prov *pr);       /* invents, records */
int       vm_peek(const Vm *m, uint64_t a, int n, uint64_t *v, Prov *pr); /* no side effect */
void      vm_write(Vm *m, uint64_t a, int n, uint64_t v);
/* a write the run did not make: the sentinel, the canary, relocations.
   Same effect, but it does not appear in the memory log.             */
void      vm_poke(Vm *m, uint64_t a, int n, uint64_t v);
/* an address standing in for a symbol this file does not define */
uint64_t  vm_stub(Vm *m, const char *name);
/* apply .rela.dyn / .rela.plt into the shadow, so the GOT resolves (§9) */
void      vm_relocs(Vm *m);
/* the name at the other end of a call, following a PLT stub if that is
   what the target is */
const char *vm_callee_name(Vm *m, uint64_t addr);
/* an address that stands for a symbol this file does not define */
int         vm_is_stub(const Vm *m, uint64_t addr);
uint64_t  vm_mint(Vm *m);                                    /* one plausible value */
/* the register file, as tier 1 sees it: by Capstone id, at the width that
   id names, with x86's zero-extend-at-4 / merge-at-1-and-2 write rule */
uint64_t  vm_reg_get(Vm *m, unsigned csreg);
void      vm_reg_set(Vm *m, unsigned csreg, uint64_t v, int prov);
int       vm_reg_width(const Vm *m, unsigned csreg);
int       vm_slot_named(const Vm *m, const char *name);
uint64_t  vm_slot_get(const Vm *m, int slot);
void      vm_slot_set(Vm *m, int slot, uint64_t v, int prov);

/* ---- the vector file (§4.4) ---------------------------------------- */
/* -1 when the id is not a vector register; the width is in bytes, so
   xmm0 and ymm0 are the same slot at 16 and 32.                      */
int       vm_vslot(const Vm *m, unsigned csreg);
int       vm_vwidth(const Vm *m, unsigned csreg);
/* read `bytes` of a slot, inventing them the first time like any other
   value; write them back so a later read agrees                      */
void      vm_vget(Vm *m, int slot, int bytes, uint8_t *out);
void      vm_vset(Vm *m, int slot, int bytes, const uint8_t *in, int prov);
/* the same write, but zeroing everything above `bytes` -- what a VEX-encoded
   instruction does and a legacy SSE one deliberately does not */
void      vm_vsetz(Vm *m, int slot, int bytes, const uint8_t *in, int prov);
/* what a value points at, if anything in the file does: "strcmp+0x12",
   ".rodata+0x40", "stack-0x20".  Returns buf, empty when nothing fits. */
const char *vm_annotate(const Vm *m, uint64_t v, char *buf, size_t n);
const char *vm_prov_name(int prov);

/* tier 1, in vmx86.c: 0 = not modelled, take tier 0; 1 = done; 2 = a
   conditional branch whose outcome is in *cond (which vm_step then uses
   instead of guessing).                                              */
int  vmx86_exec(Vm *m, const Insn *in, const InsnInfo *info, int *cond, uint64_t *ret);
/* tiers 2 and 3, in vmsimd.c: scalar float, packed moves, packed
   shuffle/compare and packed arithmetic, written width-parametric so AVX2
   is the same code at another lane count (§4.3.1).  Returns 0 when the
   instruction is not one of them, 1 when it is; *tier says which.    */
int  vmsimd_exec(Vm *m, const Insn *in, const InsnInfo *info, int *tier);
/* the call step-over contract of §6: rax is whatever the callee is
   pretending to have returned, the caller-saved registers are gone, and
   rsp is untouched because the call was never really made.           */
void vmx86_call_over(Vm *m, const Insn *in, const char *callee);
/* replay tests/vm-corpus.txt, the states hardware produced (§16) */
int  vm_corpus_check(const char *path, const Elf *elf, int show);
/* §10.1: what share of a file's strings are reachable with no analysis */
void vm_string_report(const Elf *e);
/* §4.3: what share of a file's instructions each tier takes */
void vm_tier_report(const Elf *e);

/* ---- the virtual CPU (§4.2), in vmcpu.c ---------------------------- */
/* `cpuid` is a pure function of (eax, ecx) and a small table, and a codec's
   own dispatch asks it directly -- so it can be answered honestly, and the
   answer is what decides which kernel a wisp walks into.            */
int         vm_cpu_set(const char *name);          /* 0 if not a profile */
int         vm_cpu_profile(void);
const char *vm_cpu_name(void);
const char *vm_cpu_what(void);
const char *vm_cpu_nth(int i, const char **what);  /* NULL past the end */
void        vm_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4]);
uint64_t    vm_xcr0(void);
static inline int vm_spent(const Vm *m){ return m->fuel <= 0; }

#endif
