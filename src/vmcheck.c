/* vmcheck.c -- replay the hardware corpus through the interpreter.
 *
 * tests/vm-corpus.txt was produced by single-stepping each instruction on a
 * real CPU (tools/gen-corpus.sh, §16).  This reads it back, runs the same
 * instruction from the same state through vmx86.c, and reports every
 * divergence.  It needs neither gdb nor an x86 host -- which is the whole
 * reason the corpus is checked in as data rather than generated on the fly.
 *
 * A flag Capstone reports as architecturally *undefined* for an instruction
 * is not compared: hardware puts something there, the manual does not say
 * what, and the interpreter's answer is to admit it does not know.
 */
#define _GNU_SOURCE
#include "vm.h"
#include "disasm.h"
#include <capstone/capstone.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN 512

static const char *GPR[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                               "r8","r9","r10","r11","r12","r13","r14","r15" };
static const char *FN[VF_COUNT] = { "CF","PF","AF","ZF","SF","OF" };
static const int FLBIT[VF_COUNT] = { 0, 2, 4, 6, 7, 11 };   /* in eflags */

#define NXMM 8

typedef struct {
    uint64_t r[16], flags;
    uint8_t  x[NXMM][32];           /* the vector state a run started from,
                                       256 bits wide: a VEX form zeroes the
                                       upper half and that has to be visible */
    uint64_t winaddr;
    uint8_t  win[WIN];
    uint64_t at;                    /* where the instruction ran */
} Base;

static int hexnib(int c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(const char *s, uint8_t *out, int max){
    int n = 0;
    while (n < max && hexnib(s[0]) >= 0 && hexnib(s[1]) >= 0){
        out[n++] = (uint8_t)(hexnib(s[0]) * 16 + hexnib(s[1]));
        s += 2;
    }
    return n;
}

/* set the machine to the corpus's state, then apply the per-test overrides */
static void prime(Vm *m, const Base *b, const char *pre){
    for (int i = 0; i < 16; i++)
        vm_slot_set(m, vm_slot_named(m, GPR[i]), b->r[i], PV_DERIVED);
    for (int i = 0; i < VF_COUNT; i++)
        m->fl[i] = (uint8_t)((b->flags >> FLBIT[i]) & 1);
    m->flknown = (1u << VF_COUNT) - 1;
    for (int i = 0; i < NXMM; i++){
        uint8_t wide[VM_VBYTES];
        memset(wide, 0, sizeof wide);
        memcpy(wide, b->x[i], 32);
        vm_vset(m, i, 32, wide, PV_DERIVED);
    }
    for (int i = 0; i < WIN; i++) vm_write(m, b->winaddr + (uint64_t)i, 1, b->win[i]);
    m->nmem = 0;
    if (!pre) return;
    /* p:rdx=0,rcx=1f */
    for (const char *p = pre; *p; ){
        char name[8]; int n = 0;
        while (*p && *p != '=' && n < 7) name[n++] = *p++;
        name[n] = 0;
        if (*p != '=') break;
        p++;
        uint64_t v = strtoull(p, (char **)&p, 16);
        vm_slot_set(m, vm_slot_named(m, name), v, PV_DERIVED);
        if (*p == ',') p++;
    }
}

int vm_corpus_check(const char *path, const Elf *elf, int show){
    FILE *f = fopen(path, "r");
    if (!f){
        printf("  corpus       %s: not found -- run tools/gen-corpus.sh on an x86 host\n", path);
        return 0;
    }
    Base b;
    memset(&b, 0, sizeof b);
    char line[4096];
    int nvec = 0, nbad = 0, nskip = 0, shown = 0, ndecl = 0;
    long lineno = 0;
    /* flags the interpreter declined to compute, by form: visible rather
       than quietly excused, because a decline is how a missing rule would
       hide from this test */
    char decl[8][48]; int ndeclk = 0;

    while (fgets(line, sizeof line, f)){
        lineno++;
        if (line[0] == '#' || line[0] == '\n') continue;

        if (line[0] == 'c' && line[1] == ' '){ b.at = strtoull(line + 2, NULL, 16); continue; }

        if (line[0] == 's' && line[1] == ' '){
            const char *p = line + 2;
            for (int i = 0; i < 16; i++) b.r[i] = strtoull(p, (char **)&p, 16);
            b.flags = strtoull(p, (char **)&p, 16);
            continue;
        }
        if (line[0] == 'v' && line[1] == ' '){
            const char *p2 = line + 2;
            for (int i = 0; i < NXMM; i++){
                while (*p2 == ' ') p2++;
                /* printed most significant byte first, as one big number */
                uint8_t big[32];
                if (unhex(p2, big, 32) != 32) break;
                for (int k = 0; k < 32; k++) b.x[i][k] = big[31 - k];
                p2 += 64;
            }
            continue;
        }
        if (line[0] == 'w' && line[1] == ' '){
            const char *p = line + 2;
            b.winaddr = strtoull(p, (char **)&p, 16);
            while (*p == ' ') p++;
            unhex(p, b.win, WIN);
            continue;
        }
        if (line[0] == 'x' && line[1] == ' '){ nskip++; continue; }   /* faulted on hardware */
        if (line[0] != 't' || line[1] != ' ') continue;

        /* t <bytes>[ p:overrides] <deltas...> ; <disassembly> */
        char *semi = strchr(line, ';');
        if (semi) *semi = 0;
        const char *text = semi ? semi + 1 : "";
        char *tok = strtok(line + 2, " \t\n");
        if (!tok) continue;
        uint8_t code[16];
        int len = unhex(tok, code, 16);
        if (!len) continue;

        char pre[128] = "";
        uint64_t want[16];
        for (int i = 0; i < 16; i++) want[i] = b.r[i];
        uint8_t wantx[NXMM][32];
        memcpy(wantx, b.x, sizeof wantx);
        uint64_t wflags = b.flags;
        uint8_t wwin[WIN];
        memcpy(wwin, b.win, WIN);
        long wrip = -1;

        while ((tok = strtok(NULL, " \t\n"))){
            if (!strncmp(tok, "p:", 2)){ snprintf(pre, sizeof pre, "%s", tok + 2); continue; }
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            const char *val = eq + 1;
            if (!strcmp(tok, "f")){ wflags = strtoull(val, NULL, 16); continue; }
            if (!strcmp(tok, "rip")){ wrip = strtol(val, NULL, 10); continue; }
            if (tok[0] == 'm' && tok[1] >= '0' && tok[1] <= '9'){
                int off = atoi(tok + 1);
                uint8_t got[WIN];
                int n = unhex(val, got, WIN);
                if (off >= 0 && off + n <= WIN) memcpy(wwin + off, got, (size_t)n);
                continue;
            }
            if (!strncmp(tok, "xmm", 3)){
                int i = atoi(tok + 3);
                uint8_t big[32];
                if (i >= 0 && i < NXMM && unhex(val, big, 32) == 32)
                    for (int k = 0; k < 32; k++) wantx[i][k] = big[31 - k];
                continue;
            }
            for (int i = 0; i < 16; i++)
                if (!strcmp(tok, GPR[i])) want[i] = strtoull(val, NULL, 16);
        }
        /* an override is part of the state the test ran from */
        if (pre[0]){
            for (const char *p = pre; *p; ){
                char name[8]; int n = 0;
                while (*p && *p != '=' && n < 7) name[n++] = *p++;
                name[n] = 0;
                if (*p != '=') break;
                p++;
                uint64_t v = strtoull(p, (char **)&p, 16);
                for (int i = 0; i < 16; i++)
                    if (!strcmp(name, GPR[i]) && want[i] == b.r[i]) want[i] = v;
                if (*p == ',') p++;
            }
        }

        /* run it */
        Insn in;
        memset(&in, 0, sizeof in);
        in.addr = b.at; in.len = (uint16_t)len;
        InsnInfo info;
        if (!disasm_info(code, (unsigned)len, in.addr, &info)){
            nskip++;
            if (show && shown++ < 20) printf("  NODECODE  line %ld  %s\n", lineno, text + 1);
            continue;
        }

        Vm m;
        vm_init(&m, elf, in.addr, 0x51ED);
        prime(&m, &b, pre[0] ? pre : NULL);
        int cond = -1; uint64_t retaddr = 0, tier = 1;
        int rc = vmx86_exec(&m, &in, &info, &cond, &retaddr);
        if (!rc){
            int t2 = 3;
            if (vmsimd_exec(&m, &in, &info, &t2)){ rc = 1; tier = (uint64_t)t2; }
        }
        (void)tier;
        nvec++;

        if (!rc){
            nbad++;
            if (show && shown++ < 20)
                printf("  MISS      line %ld  %s\n", lineno, text + 1);
            vm_free(&m);
            continue;
        }

        int bad = 0;
        char why[256] = "";
        for (int i = 0; i < 16 && !bad; i++){
            uint64_t got = vm_slot_get(&m, vm_slot_named(&m, GPR[i]));
            if (got != want[i]){
                snprintf(why, sizeof why, "%s = %llx, hardware says %llx",
                         GPR[i], (unsigned long long)got, (unsigned long long)want[i]);
                bad = 1;
            }
        }
        for (int i = 0; i < VF_COUNT && !bad; i++){
            if (info.eflags & (X86_EFLAGS_UNDEFINED_CF << 0)) { /* placeholder */ }
            static const uint64_t UND[VF_COUNT] = {
                X86_EFLAGS_UNDEFINED_CF, X86_EFLAGS_UNDEFINED_PF, X86_EFLAGS_UNDEFINED_AF,
                X86_EFLAGS_UNDEFINED_ZF, X86_EFLAGS_UNDEFINED_SF, X86_EFLAGS_UNDEFINED_OF };
            if (info.eflags & UND[i]) continue;          /* the manual says nothing */
            int w = (int)((wflags >> FLBIT[i]) & 1);
            if (!(m.flknown & (1u << i))){
                ndecl++;
                char key[48];
                char mn[24]; int k = 0;
                for (const char *q = text + 1; *q && *q != ' ' && k < 23; q++) mn[k++] = *q;
                mn[k] = 0;
                snprintf(key, sizeof key, "%s %s", mn, FN[i]);
                int seen = 0;
                for (int q = 0; q < ndeclk; q++) if (!strcmp(decl[q], key)) seen = 1;
                if (!seen && ndeclk < 8) snprintf(decl[ndeclk++], 48, "%s", key);
                continue;
            }
            if (m.fl[i] != w){
                snprintf(why, sizeof why, "%s = %d, hardware says %d", FN[i], m.fl[i], w);
                bad = 1;
            }
        }
        for (int i = 0; i < NXMM && !bad; i++){
            uint8_t got[VM_VBYTES];
            vm_vget(&m, i, VM_VBYTES, got);
            if (memcmp(got, wantx[i], 32)){
                char g[72], e2[72];
                for (int k = 0; k < 32; k++){
                    snprintf(g + k * 2, 3, "%02x", got[31 - k]);
                    snprintf(e2 + k * 2, 3, "%02x", wantx[i][31 - k]);
                }
                snprintf(why, sizeof why, "ymm%d = %s, hardware says %s", i, g, e2);
                bad = 1;
            }
        }
        for (int i = 0; i < WIN && !bad; i++){
            uint64_t v; Prov pv;
            if (!vm_peek(&m, b.winaddr + (uint64_t)i, 1, &v, &pv)) continue;
            if ((uint8_t)v != wwin[i]){
                snprintf(why, sizeof why, "memory +%d = %02x, hardware says %02x",
                         i, (unsigned)(v & 0xff), wwin[i]);
                bad = 1;
            }
        }
        if (!bad && rc == 2){
            int taken = (wrip >= 0);
            if (cond != taken){
                snprintf(why, sizeof why, "branch %s, hardware %s",
                         cond ? "taken" : "not taken", taken ? "took it" : "did not");
                bad = 1;
            }
        }
        if (bad){
            nbad++;
            if (show && shown++ < 20)
                printf("  DIVERGE line %ld  %-28s %s\n", lineno, text + 1, why);
        }
        vm_free(&m);
    }
    fclose(f);

    printf("  corpus        %d vectors from hardware, %d diverged, %d not modelled\n",
           nvec, nbad, nskip);
    if (ndecl){
        printf("  declined      %d flag comparisons the interpreter would not guess:", ndecl);
        for (int i = 0; i < ndeclk; i++) printf(" %s%s", decl[i], i + 1 < ndeclk ? "," : "");
        printf("\n");
    }
    if (nbad && !show)
        printf("                (run --vmtest -v to see the first twenty)\n");
    return nbad;
}

/* ------------------------------------------------------------------ */
/* §10.1: how much of a binary's text is reachable without any analysis */
/* ------------------------------------------------------------------ */
/* Two mechanisms, neither of which needs a line of the VM: a RIP-relative
   operand naming an address in .rodata, and an R_X86_64_RELATIVE addend
   pointing there.  Between them they account for most of the strings in a
   binary, which is what lets the panel say what a pointer points at
   instead of printing sixteen hex digits.                          */

typedef struct { uint64_t *at; int n, cap; uint8_t *byLea, *byRel; } Strs;
static Strs g_strs;

static int str_index(const Strs *s, uint64_t a){
    int lo = 0, hi = s->n - 1;
    while (lo <= hi){
        int mid = (lo + hi) / 2;
        if (s->at[mid] == a) return mid;
        if (s->at[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static void strs_add(Strs *s, uint64_t a){
    if (s->n >= s->cap){
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->at = realloc(s->at, (size_t)s->cap * sizeof *s->at);
    }
    s->at[s->n++] = a;
}

static void hit_riprel(uint64_t addr, int isLea, void *u){
    Strs *s = u;
    int i = str_index(s, addr);
    if (i >= 0 && isLea) s->byLea[i] = 1;
}

/* a printable NUL-terminated run of four characters or more */
static int printable(unsigned char c){ return (c >= 32 && c < 127) || c == '\t' || c == '\n'; }

void vm_string_report(const Elf *e){
    Strs *s = &g_strs;
    memset(s, 0, sizeof *s);
    for (int i = 0; i < e->nsec; i++){
        const Sec *sc = &e->sec[i];
        /* .rodata only, as §10.1 measured it.  Widening this to every
           allocated section drags in .dynstr, whose contents are symbol
           names that no instruction ever names with a lea.          */
        if (strncmp(sc->name, ".rodata", 7)) continue;
        if (!sc->data || !sc->addr || !sc->datasz) continue;
        uint64_t run = 0;
        for (uint64_t k = 0; k < sc->datasz; k++){
            unsigned char c = sc->data[k];
            if (printable(c)){ run++; continue; }
            if (!c && run >= 4) strs_add(s, sc->addr + k - run);
            run = 0;
        }
    }
    if (!s->n){ free(s->at); memset(s, 0, sizeof *s); return; }
    s->byLea = calloc((size_t)s->n, 1);
    s->byRel = calloc((size_t)s->n, 1);

    for (int i = 0; i < e->nsec; i++){
        const Sec *sc = &e->sec[i];
        if ((sc->flags & 0x4) && sc->data && sc->addr)            /* SHF_EXECINSTR */
            disasm_riprel(sc->data, sc->datasz, sc->addr, hit_riprel, s);
    }
    /* the relocation half: an addend that points at a string is a pointer
       in a table, which is how every array-of-strings is built */
    int w = e->is64 ? 8 : 4;
    for (int i = 0; i < e->nsec; i++){
        const Sec *sc = &e->sec[i];
        if (sc->type != 4 || !sc->data || !sc->entsize) continue;    /* SHT_RELA */
        for (uint64_t k = 0; k + sc->entsize <= sc->datasz; k += sc->entsize){
            uint64_t add = elf_rdw(e, sc->data + k + 2 * (uint64_t)w);
            int at = str_index(s, add);
            if (at >= 0) s->byRel[at] = 1;
        }
    }
    int nl = 0, nr = 0, both = 0;
    for (int i = 0; i < s->n; i++){
        if (s->byLea[i]) nl++;
        if (s->byRel[i]) nr++;
        if (s->byLea[i] || s->byRel[i]) both++;
    }
    printf("  strings       %d in .rodata;  %.1f%% named by a rip-relative lea,"
           " %.1f%% by a relocation,  %.1f%% by either (%d left over)\n",
           s->n, 100.0 * nl / s->n, 100.0 * nr / s->n, 100.0 * both / s->n,
           s->n - both);
    free(s->at); free(s->byLea); free(s->byRel);
    memset(s, 0, sizeof *s);
}


/* ------------------------------------------------------------------ */
/* §4.3's table, measured the same way: which tier takes each mnemonic  */
/* ------------------------------------------------------------------ */
/* Not a table of names -- the classifier *is* the interpreter.  Every
   instruction in the file's executable sections is offered to tier 1 and
   then to tiers 2 and 3 on a throwaway machine, and whichever takes it is
   what the wisp would use.  A list of mnemonics maintained beside the
   switch would drift away from it; this cannot.                    */

void vm_tier_report(const Elf *e){
    if (disasm_machine() != 62 && disasm_machine() != 3) return;
    long n = 0, t[4] = { 0, 0, 0, 0 }, nodec = 0;
    typedef struct { char mnem[20]; long n; } Miss;
    Miss miss[64];
    int nmiss = 0;
    Vm m;
    vm_init(&m, e, 0, 0xC0FFEE);
    /* Sweep function by function where there are symbols to sweep by.  A
       blind linear sweep of a section resyncs a byte at a time through
       anything Capstone cannot decode -- AVX-512, or data -- and the
       garbage it makes on the way out is then counted as unmodelled
       instructions.  On `libdav1d` that invented an `in`, an `outsd` and a
       `loop` that objdump says are not there at all.                */
    int usesyms = 0;
    if (!e->stripped)
        for (int i = 0; i < e->nsym; i++)
            if (e->sym[i].type == 2 && e->sym[i].size && e->sym[i].value){ usesyms = 1; break; }
    int resync = 0;

    for (int i = 0; i < e->nsec; i++){
        const Sec *sc = &e->sec[i];
        if (!(sc->flags & 0x4) || !sc->data || !sc->addr || !sc->datasz) continue;
        for (int fn = -1; fn < (usesyms ? e->nsym : 0); fn++){
        uint64_t at, end;
        if (!usesyms){ at = 0; end = sc->datasz; }
        else if (fn < 0) continue;
        else {
            const Sym *sy = &e->sym[fn];
            if (sy->type != 2 || !sy->size || !sy->value) continue;
            if (sy->value < sc->addr || sy->value >= sc->addr + sc->datasz) continue;
            at = sy->value - sc->addr;
            end = at + sy->size;
            if (end > sc->datasz) end = sc->datasz;
        }
        while (at < end){
            InsnInfo info;
            unsigned len = 0;
            const uint8_t *p = sc->data + at;
            uint64_t left = end - at;
            unsigned take = (unsigned)(left > 15 ? 15 : left);
            if (!disasm_info(p, take, sc->addr + at, &info)){
                /* Whatever this is -- AVX-512, or data -- the sweep is now
                   inside it.  Stepping out a byte at a time decodes the
                   tail of it as instructions that are not there, so
                   nothing counts until three in a row have decoded. */
                nodec++; at++; resync = 3;
                continue;
            }
            len = info.len;
            if (!len){ at++; continue; }

            if (resync){ resync--; at += len; continue; }

            Insn in;
            memset(&in, 0, sizeof in);
            in.addr = sc->addr + at; in.len = (uint16_t)len;
            int cond = -1, tier = 3;
            uint64_t ret = 0;
            n++;
            if (vmx86_exec(&m, &in, &info, &cond, &ret)) t[1]++;
            else if (vmsimd_exec(&m, &in, &info, &tier)) t[tier == 2 ? 2 : 3]++;
            else {
                t[0]++;
                int at2 = -1;
                for (int q = 0; q < nmiss; q++)
                    if (!strcmp(miss[q].mnem, info.mnem)){ at2 = q; break; }
                if (at2 < 0 && nmiss < 64){
                    at2 = nmiss++;
                    snprintf(miss[at2].mnem, sizeof miss[at2].mnem, "%s", info.mnem);
                    miss[at2].n = 0;
                }
                if (at2 >= 0) miss[at2].n++;
            }
            at += len;
            /* the throwaway machine is only here to be executed against;
               keep it from filling up with a whole binary's worth of
               invented memory */
            if ((n & 0xffff) == 0){ vm_free(&m); vm_init(&m, e, 0, 0xC0FFEE); }
        }
        if (!usesyms) break;
        }
    }
    vm_free(&m);
    if (!n) return;
    printf("  coverage      %ld instructions%s:"
           "  tier 1 %.1f%%,  tier 2 %.1f%%,\n"
           "                tier 3 %.1f%%,  modelled %.1f%%,  havoc %.1f%%%s\n",
           n, usesyms ? " in the functions the symbols name" : " in the executable sections",
           100.0 * t[1] / n, 100.0 * t[2] / n, 100.0 * t[3] / n,
           100.0 * (n - t[0]) / n, 100.0 * t[0] / n, "");
    if (nodec)
        printf("                %ld bytes Capstone could not decode at all\n", nodec);
    if (nmiss){
        for (int i = 1; i < nmiss; i++){          /* the biggest first */
            int j = i;
            while (j > 0 && miss[j-1].n < miss[j].n){
                Miss tmp = miss[j-1];
                miss[j-1] = miss[j]; miss[j] = tmp; j--;
            }
        }
        printf("  havoc is      ");
        for (int i = 0; i < nmiss && i < 8; i++)
            printf("%s %.1f%%%s", miss[i].mnem, 100.0 * miss[i].n / n,
                   i + 1 < nmiss && i < 7 ? ", " : "");
        printf("\n");
    }
}
