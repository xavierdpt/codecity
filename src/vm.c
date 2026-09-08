/* vm.c -- the tier-0 machine: registers, flags, three memory layers
 *
 * See vm.h for what this is and is not.  The parts, in order:
 *
 *   the PRNG and minting     -- where invented values come from
 *   the register file        -- Capstone ids mapped onto named slots
 *   the memory layers        -- shadow, then the file, then invention
 *   tier 0                   -- read, havoc, forget the flags
 *   the walk                 -- §6, minus everything that needs a stack
 */
#define _GNU_SOURCE
#include "vm.h"
#include "disasm.h"
#include <capstone/capstone.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* invention                                                           */
/* ------------------------------------------------------------------ */

static uint64_t sm64(uint64_t *s){
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform 64-bit noise is useless to look at and produces absurd branches.
   Most invented values are small integers; some are pointers into the
   synthetic stack, and some point at real bytes in the file, which is what
   makes vm_annotate() ever have anything to say.                      */
uint64_t vm_mint(Vm *m){
    uint64_t x = sm64(&m->rng);
    /* about to be compared against a small immediate: land on or near it,
       so the loop it controls runs a plausible number of times */
    if (m->mintHintOn){
        uint64_t h = m->mintHint;
        switch (x % 4){
        case 0: return h;
        case 1: return h ? (x >> 8) % h : 0;
        case 2: return h + 1;
        default: break;
        }
    }
    switch (x % 10){
    case 0: case 1: case 2: case 3: case 4: case 5:
        return (x >> 8) & 0xff;                          /* a small integer */
    case 6:
        return (x >> 8) & 0xffff;
    case 7: case 8:
        /* A pointer -- and pointers invented separately have to land near
           each other, or `while (p != end) p++` walks 2^61 times before
           the fuel runs out.  One small window, so two of them are a
           plausible number of elements apart.                        */
        return m->heapBase + (((x >> 12) % 0x400) & ~7ull);
    default: break;
    }
    /* into a real section, so the value has something to be named after */
    const Elf *e = m->elf;
    if (e){
        int cand[16], nc = 0;
        for (int i = 0; i < e->nsec && nc < 16; i++)
            if ((e->sec[i].flags & 0x2) && e->sec[i].addr && e->sec[i].size) cand[nc++] = i;
        if (nc){
            const Sec *s = &e->sec[cand[(x >> 16) % (unsigned)nc]];
            return s->addr + ((x >> 24) % s->size);
        }
    }
    return (x >> 8) & 0xffff;
}

const char *vm_prov_name(int p){
    static const char *n[] = { "", "invented", "file", "derived", "call", "live" };
    return (p >= 0 && p <= PV_LIVE) ? n[p] : "";
}

/* ------------------------------------------------------------------ */
/* the register file                                                   */
/* ------------------------------------------------------------------ */
/* Slots are claimed by name, not by Capstone id, so the panel shows rax
   once rather than rax/eax/ax/al four times -- and so that the id numbers,
   which mean different things per architecture and can move between
   Capstone releases, never leak out of this file.                     */

/* An alias is a name for part of a slot: its width in bytes and where in
   the slot it starts.  Width 8 (or the slot's own width) is the whole
   thing; `ah` is the one that is not at offset 0.                     */
typedef struct { const char *name; uint8_t w, off; } Alias;
typedef struct { Alias alias[6]; } RegRow;

/* the display order of §1, each row a slot and its sub-registers */
static const RegRow X86_64_REGS[] = {
    {{{"rax",8,0},{"eax",4,0},{"ax",2,0},{"al",1,0},{"ah",1,1}}},
    {{{"rcx",8,0},{"ecx",4,0},{"cx",2,0},{"cl",1,0},{"ch",1,1}}},
    {{{"rdx",8,0},{"edx",4,0},{"dx",2,0},{"dl",1,0},{"dh",1,1}}},
    {{{"rbx",8,0},{"ebx",4,0},{"bx",2,0},{"bl",1,0},{"bh",1,1}}},
    {{{"rsi",8,0},{"esi",4,0},{"si",2,0},{"sil",1,0}}},
    {{{"rdi",8,0},{"edi",4,0},{"di",2,0},{"dil",1,0}}},
    {{{"rbp",8,0},{"ebp",4,0},{"bp",2,0},{"bpl",1,0}}},
    {{{"rsp",8,0},{"esp",4,0},{"sp",2,0},{"spl",1,0}}},
    {{{"r8",8,0},{"r8d",4,0},{"r8w",2,0},{"r8b",1,0}}},
    {{{"r9",8,0},{"r9d",4,0},{"r9w",2,0},{"r9b",1,0}}},
    {{{"r10",8,0},{"r10d",4,0},{"r10w",2,0},{"r10b",1,0}}},
    {{{"r11",8,0},{"r11d",4,0},{"r11w",2,0},{"r11b",1,0}}},
    {{{"r12",8,0},{"r12d",4,0},{"r12w",2,0},{"r12b",1,0}}},
    {{{"r13",8,0},{"r13d",4,0},{"r13w",2,0},{"r13b",1,0}}},
    {{{"r14",8,0},{"r14d",4,0},{"r14w",2,0},{"r14b",1,0}}},
    {{{"r15",8,0},{"r15d",4,0},{"r15w",2,0},{"r15b",1,0}}},
    {{{"rip",8,0},{"eip",4,0}}},
};
static const RegRow X86_32_REGS[] = {
    {{{"eax",4,0},{"ax",2,0},{"al",1,0},{"ah",1,1}}},
    {{{"ecx",4,0},{"cx",2,0},{"cl",1,0},{"ch",1,1}}},
    {{{"edx",4,0},{"dx",2,0},{"dl",1,0},{"dh",1,1}}},
    {{{"ebx",4,0},{"bx",2,0},{"bl",1,0},{"bh",1,1}}},
    {{{"esi",4,0},{"si",2,0}}},   {{{"edi",4,0},{"di",2,0}}},
    {{{"ebp",4,0},{"bp",2,0}}},   {{{"esp",4,0},{"sp",2,0}}},
    {{{"eip",4,0}}},
};
static const RegRow ARM_REGS[] = {
    {{{"r0",4,0}}},
    {{{"r1",4,0}}},
    {{{"r2",4,0}}},
    {{{"r3",4,0}}},
    {{{"r4",4,0}}},
    {{{"r5",4,0}}},
    {{{"r6",4,0}}},
    {{{"r7",4,0}}},
    {{{"r8",4,0}}},
    {{{"r9",4,0}}},
    {{{"r10",4,0}}},
    {{{"r11",4,0}}},
    {{{"r12",4,0}}},
    {{{"sp",4,0}}}, {{{"lr",4,0}}}, {{{"pc",4,0}}},
};
static const RegRow ARM64_REGS[] = {
    {{{"x0",8,0},{"w0",4,0}}},
    {{{"x1",8,0},{"w1",4,0}}},
    {{{"x2",8,0},{"w2",4,0}}},
    {{{"x3",8,0},{"w3",4,0}}},
    {{{"x4",8,0},{"w4",4,0}}},
    {{{"x5",8,0},{"w5",4,0}}},
    {{{"x6",8,0},{"w6",4,0}}},
    {{{"x7",8,0},{"w7",4,0}}},
    {{{"x8",8,0},{"w8",4,0}}},
    {{{"x9",8,0},{"w9",4,0}}},
    {{{"x10",8,0},{"w10",4,0}}},
    {{{"x11",8,0},{"w11",4,0}}},
    {{{"x12",8,0},{"w12",4,0}}},
    {{{"x13",8,0},{"w13",4,0}}},
    {{{"x14",8,0},{"w14",4,0}}},
    {{{"x15",8,0},{"w15",4,0}}},
    {{{"x16",8,0},{"w16",4,0}}},
    {{{"x17",8,0},{"w17",4,0}}},
    {{{"x18",8,0},{"w18",4,0}}},
    {{{"x19",8,0},{"w19",4,0}}},
    {{{"x20",8,0},{"w20",4,0}}},
    {{{"x21",8,0},{"w21",4,0}}},
    {{{"x22",8,0},{"w22",4,0}}},
    {{{"x23",8,0},{"w23",4,0}}},
    {{{"x24",8,0},{"w24",4,0}}},
    {{{"x25",8,0},{"w25",4,0}}},
    {{{"x26",8,0},{"w26",4,0}}},
    {{{"x27",8,0},{"w27",4,0}}},
    {{{"x28",8,0},{"w28",4,0}}},
    {{{"x29",8,0},{"w29",4,0}}},
    {{{"x30",8,0},{"w30",4,0}}},
    {{{"sp",8,0},{"wsp",4,0}}}, {{{"pc",8,0}}},
};

static int slot_named(Vm *m, const char *name){
    for (int i = 0; i < m->nreg; i++)
        if (!strcmp(m->rname[i], name)) return i;
    if (m->nreg >= VM_NREG) return -1;
    int s = m->nreg++;
    snprintf(m->rname[s], sizeof m->rname[s], "%s", name);
    return s;
}

/* Walk every register id Capstone will name and file it under the slot
   whose alias list mentions it.  Ids the table does not know keep a slot
   of their own, claimed the first time an instruction touches one --
   which is how MIPS, PowerPC and the rest get a panel at all.         */
static void map_regs(Vm *m, const RegRow *tab, int ntab){
    for (int i = 0; i < ntab; i++) slot_named(m, tab[i].alias[0].name);
    m->ncanon = m->nreg;
    for (unsigned id = 1; id < VM_REGIDS; id++){
        const char *nm = disasm_reg_name(id);
        if (!nm || !*nm) continue;
        for (int i = 0; i < ntab && m->rmap[id] < 0; i++)
            for (int k = 0; k < 6 && tab[i].alias[k].name; k++)
                if (!strcmp(nm, tab[i].alias[k].name)){
                    m->rmap[id] = (int16_t)i;
                    m->rwid[id] = tab[i].alias[k].w;
                    m->roff[id] = tab[i].alias[k].off;
                    break;
                }
    }
}

/* the slot an id belongs to, claiming one on first sight.  The flag
   register is not one of them: flags have their own model with a known
   bit each, and a slot holding a havocked copy of them would say the
   opposite of what the flag row says.                                 */
/* xmm/ymm/zmm live in their own file, not among the general registers:
   they are 64 bytes wide and the panel treats them differently (§4.4) */
static int vec_name(const char *nm, int *slot, int *bytes){
    int w = 0;
    if (!strncmp(nm, "xmm", 3)) w = 16;
    else if (!strncmp(nm, "ymm", 3)) w = 32;
    else if (!strncmp(nm, "zmm", 3)) w = 64;
    else return 0;
    const char *d = nm + 3;
    if (*d < '0' || *d > '9') return 0;
    int n = 0;
    while (*d >= '0' && *d <= '9') n = n * 10 + (*d++ - '0');
    if (*d || n >= VM_NVREG) return 0;
    *slot = n; *bytes = w;
    return 1;
}

int vm_vslot(const Vm *m, unsigned csreg){
    return (m->vmap && csreg < VM_REGIDS) ? m->vmap[csreg] : -1;
}
int vm_vwidth(const Vm *m, unsigned csreg){
    return (m->vwid && csreg < VM_REGIDS && m->vwid[csreg]) ? m->vwid[csreg] : 16;
}

void vm_vget(Vm *m, int slot, int bytes, uint8_t *out){
    if (slot < 0 || slot >= VM_NVREG){ memset(out, 0, (size_t)bytes); return; }
    VVec *v = &m->v[slot];
    if (v->prov == PV_NONE){                 /* materialise on first read */
        for (int i = 0; i < VM_VBYTES; i++) v->b[i] = (uint8_t)(vm_mint(m) & 0xff);
        v->prov = PV_INVENTED;
        v->stamp = m->steps;
    }
    memcpy(out, v->b, (size_t)(bytes > VM_VBYTES ? VM_VBYTES : bytes));
}

static void vset(Vm *m, int slot, int bytes, const uint8_t *in, int prov, int zero){
    if (slot < 0 || slot >= VM_NVREG) return;
    VVec *v = &m->v[slot];
    if (bytes > VM_VBYTES) bytes = VM_VBYTES;
    if (v->prov == PV_NONE && (zero || bytes < VM_VBYTES)){
        /* the part we are not writing has to exist before it is kept */
        for (int i = 0; i < VM_VBYTES; i++) v->b[i] = (uint8_t)(vm_mint(m) & 0xff);
    }
    memcpy(v->b, in, (size_t)bytes);
    /* A VEX-encoded write zeroes the bits above the destination width; the
       legacy SSE form of the same operation leaves them exactly as they
       were.  Only a corpus recorded 256 bits wide can tell the two
       apart, which is why M7 widened it before lifting M6's gate.   */
    if (zero) memset(v->b + bytes, 0, (size_t)(VM_VBYTES - bytes));
    v->prov = (uint8_t)prov;
    v->stamp = m->steps;
}

void vm_vset(Vm *m, int slot, int bytes, const uint8_t *in, int prov){
    vset(m, slot, bytes, in, prov, 0);
}
void vm_vsetz(Vm *m, int slot, int bytes, const uint8_t *in, int prov){
    vset(m, slot, bytes, in, prov, 1);
}

static int is_flagreg(const char *nm){
    return !strcmp(nm, "eflags") || !strcmp(nm, "rflags") || !strcmp(nm, "flags") ||
           !strcmp(nm, "cpsr")   || !strcmp(nm, "nzcv")   || !strcmp(nm, "fpsr");
}

static int reg_slot(Vm *m, unsigned id){
    if (id >= VM_REGIDS) return -1;
    if (m->rmap[id] >= 0) return m->rmap[id];
    if (m->vmap[id] >= 0) return -1;            /* it lives in the vector file */
    const char *nm = disasm_reg_name(id);
    if (!nm || !*nm || is_flagreg(nm)) return -1;
    int s = slot_named(m, nm);
    m->rmap[id] = (int16_t)s;
    return s;
}

static void reg_touch(Vm *m, int s);

/* ---- the sub-register rule ------------------------------------------
   x86: a write at width 4 zero-extends into the whole 64-bit slot, a write
   at width 1 or 2 merges.  Getting this backwards produces pointers with
   junk in the high bits -- visibly, embarrassingly wrong.            */

int vm_slot_named(const Vm *m, const char *name){
    for (int i = 0; i < m->nreg; i++) if (!strcmp(m->rname[i], name)) return i;
    return -1;
}

uint64_t vm_slot_get(const Vm *m, int slot){
    return (slot >= 0 && slot < m->nreg) ? m->r[slot].v : 0;
}

void vm_slot_set(Vm *m, int slot, uint64_t v, int prov){
    if (slot < 0 || slot >= m->nreg) return;
    m->r[slot].v = v;
    m->r[slot].prov = (uint8_t)prov;
    m->r[slot].stamp = m->steps;
}

int vm_reg_width(const Vm *m, unsigned csreg){
    return (csreg < VM_REGIDS && m->rwid[csreg]) ? m->rwid[csreg] : 8;
}

static uint64_t wmask(int w){ return w >= 8 ? ~0ull : ((1ull << (w * 8)) - 1); }

uint64_t vm_reg_get(Vm *m, unsigned csreg){
    if (csreg >= VM_REGIDS) return 0;
    int s = m->rmap[csreg];
    if (s < 0) return 0;
    reg_touch(m, s);
    int w = m->rwid[csreg] ? m->rwid[csreg] : 8, off = m->roff[csreg];
    return (m->r[s].v >> (off * 8)) & wmask(w);
}

void vm_reg_set(Vm *m, unsigned csreg, uint64_t v, int prov){
    if (csreg >= VM_REGIDS) return;
    int s = m->rmap[csreg];
    if (s < 0) return;
    int w = m->rwid[csreg] ? m->rwid[csreg] : 8, off = m->roff[csreg];
    uint64_t nv;
    if (w >= 8)      nv = v;
    else if (w == 4) nv = v & 0xffffffffull;        /* zero-extends */
    else {                                          /* merges */
        reg_touch(m, s);
        uint64_t mk = wmask(w) << (off * 8);
        nv = (m->r[s].v & ~mk) | ((v & wmask(w)) << (off * 8));
    }
    m->r[s].v = nv;
    m->r[s].prov = (uint8_t)prov;
    m->r[s].stamp = m->steps;
}

static void reg_touch(Vm *m, int s){         /* materialise on first read */
    if (s < 0) return;
    if (m->r[s].prov == PV_NONE){
        m->r[s].v = vm_mint(m);
        m->r[s].prov = PV_INVENTED;
        m->r[s].stamp = m->steps;
    }
}

static void reg_havoc(Vm *m, int s){
    if (s < 0 || s == m->pcSlot) return;     /* the walk owns the pc */
    m->r[s].v = vm_mint(m);
    m->r[s].prov = PV_INVENTED;
    m->r[s].stamp = m->steps;
}

/* ------------------------------------------------------------------ */
/* memory: shadow, then the file, then invention                       */
/* ------------------------------------------------------------------ */

static VmPage *page_find(const Vm *m, uint64_t base){
    unsigned h = (unsigned)((base >> 12) * 0x9E3779B1u) & (VM_BUCKETS - 1);
    for (VmPage *p = m->pg[h]; p; p = p->next) if (p->base == base) return p;
    return NULL;
}

static VmPage *page_get(Vm *m, uint64_t base){
    VmPage *p = page_find(m, base);
    if (p) return p;
    if (m->npg >= VM_MAXPG){ m->pgFull = 1; return NULL; }
    unsigned h = (unsigned)((base >> 12) * 0x9E3779B1u) & (VM_BUCKETS - 1);
    p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->base = base;
    p->next = m->pg[h]; m->pg[h] = p;
    m->npg++;
    return p;
}

static int shadow_byte(const Vm *m, uint64_t a, uint8_t *out){
    VmPage *p = page_find(m, a & ~0xfffull);
    if (!p) return 0;
    unsigned o = (unsigned)(a & 0xfff);
    if (!(p->known[o >> 3] & (1u << (o & 7)))) return 0;
    *out = p->b[o];
    return 1;
}

static void shadow_put(Vm *m, uint64_t a, uint8_t v){
    VmPage *p = page_get(m, a & ~0xfffull);
    if (!p) return;                    /* past the cap: invent, do not record */
    unsigned o = (unsigned)(a & 0xfff);
    p->b[o] = v;
    p->known[o >> 3] |= (uint8_t)(1u << (o & 7));
}

/* layer 2: the mapped file.  Read-only -- the city is showing those same
   bytes and they must not change under it.                            */
static int file_byte(const Vm *m, uint64_t a, uint8_t *out){
    const Elf *e = m->elf;
    if (!e) return 0;
    for (int i = 0; i < e->nsec; i++){
        const Sec *s = &e->sec[i];
        if (!(s->flags & 0x2) || !s->addr || !s->data) continue;   /* SHF_ALLOC */
        if (a < s->addr || a >= s->addr + s->datasz) continue;
        *out = s->data[a - s->addr];
        return 1;
    }
    return 0;
}

/* layer 1.5: the live process, when there is one.  Above the file,
   because a running program's .data is what it *is* rather than what it
   started as, and above invention because a real address has a real
   value.  §8.4: if invention ever fires under a live wisp, that is worth
   knowing about rather than silently papering over.               */
static int live_byte(const Vm *m, uint64_t a, uint8_t *out){
    if (!m->liveread) return 0;
    return m->liveread(m->livectx, a, 1, out);
}

static uint64_t assemble(const Vm *m, const uint8_t *b, int n){
    uint64_t v = 0;
    if (m->elf && m->elf->be) for (int i = 0; i < n; i++) v = (v << 8) | b[i];
    else                      for (int i = n - 1; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

int vm_peek(const Vm *m, uint64_t a, int n, uint64_t *v, Prov *pr){
    if (n < 1 || n > 8) return 0;
    uint8_t b[8];
    Prov worst = PV_FILE;
    int live = 0;
    for (int i = 0; i < n; i++){
        uint64_t at = a + (uint64_t)i;
        /* The process outranks the shadow.  The shadow holds what a
           *simulated* run wrote -- the fake stack, the minted canary, the
           relocations vm_relocs() applied -- and none of that is more
           authoritative than the memory of a program that is actually
           running.  A live wisp makes no writes of its own, so there is
           nothing of its own to lose by looking past it.            */
        if (live_byte(m, at, &b[i])){ live = 1; continue; }
        if (shadow_byte(m, at, &b[i])) continue;
        if (file_byte(m, at, &b[i])){ continue; }
        return 0;
    }
    if (live) worst = PV_LIVE;
    /* a byte that came out of the shadow may itself have been invented --
       the shadow does not remember which, so peek reports the weaker of
       the two.  vm_read is the call that tags a value properly.       */
    if (!m->liveread && page_find(m, a & ~0xfffull)) worst = PV_DERIVED;
    if (v) *v = assemble(m, b, n);
    if (pr) *pr = worst;
    return 1;
}

static void memlog(Vm *m, uint64_t a, uint64_t v, int n, int wr, int prov){
    int i = m->nmem % VM_MEMLOG;
    m->mem[i].addr = a; m->mem[i].val = v;
    m->mem[i].n = (uint8_t)n; m->mem[i].wr = (uint8_t)wr; m->mem[i].prov = (uint8_t)prov;
    m->nmem++;
}

uint64_t vm_read(Vm *m, uint64_t a, int n, Prov *pr){
    if (n < 1 || n > 8) n = 8;
    if (a == m->fsBase + 0x28) m->nCanary++;
    uint8_t b[8];
    /* PV_FILE only when every byte really came out of the mapped image;
       one invented byte makes the whole value invented */
    Prov p = (!m->liveread && page_find(m, a & ~0xfffull)) ? PV_DERIVED : PV_FILE;
    int anylive = 0;
    for (int i = 0; i < n; i++){
        uint64_t at = a + (uint64_t)i;
        if (live_byte(m, at, &b[i])){ anylive = 1; continue; }   /* see vm_peek */
        if (shadow_byte(m, at, &b[i])) continue;
        if (file_byte(m, at, &b[i])) continue;
        /* a byte read on its own is nearly always a character being walked
           over, so make it one -- with a NUL often enough that the walk
           ends (§5's table).  Wider reads keep the general bias.     */
        if (n == 1){
            uint64_t r = sm64(&m->rng);
            b[i] = (r % 6) ? (uint8_t)(32 + (r >> 8) % 95) : 0;
        } else {
            b[i] = (uint8_t)(vm_mint(m) & 0xff);
        }
        shadow_put(m, at, b[i]);        /* invented once, then remembered */
        p = PV_INVENTED;
    }
    uint64_t v = assemble(m, b, n);
    /* one invented byte still makes the whole value invented; short of
       that, a value the process supplied is a measurement */
    if (anylive && p != PV_INVENTED) p = PV_LIVE;
    if (pr) *pr = p;
    memlog(m, a, v, n, 0, p);
    return v;
}

void vm_poke(Vm *m, uint64_t a, int n, uint64_t v){
    if (n < 1 || n > 8) n = 8;
    for (int i = 0; i < n; i++){
        int sh = m->elf && m->elf->be ? (n - 1 - i) * 8 : i * 8;
        shadow_put(m, a + (uint64_t)i, (uint8_t)(v >> sh));
    }
}

void vm_write(Vm *m, uint64_t a, int n, uint64_t v){
    vm_poke(m, a, n, v);
    memlog(m, a, v, n, 1, PV_DERIVED);
}

/* Symbols this file does not define -- every libc call a dynamic binary
   makes -- get an address of their own so the GOT resolves to something
   with a name instead of to a zero from the file.  Nothing is ever read
   or executed there; it exists to be recognised.                    */
uint64_t vm_stub(Vm *m, const char *name){
    for (int i = 0; i < m->nstub; i++)
        if (!strcmp(m->stub[i].name, name)) return m->stub[i].addr;
    if (m->nstub >= VM_STUBS) return m->stubNext;
    if (m->nstub >= m->capstub){
        m->capstub = m->capstub ? m->capstub * 2 : 64;
        m->stub = realloc(m->stub, (size_t)m->capstub * sizeof *m->stub);
        if (!m->stub){ m->capstub = 0; m->nstub = 0; return m->stubNext; }
    }
    uint64_t a = m->stubNext;
    m->stubNext += 16;
    m->stub[m->nstub].addr = a;
    snprintf(m->stub[m->nstub].name, sizeof m->stub[m->nstub].name, "%s", name);
    m->nstub++;
    return a;
}

/* ------------------------------------------------------------------ */
/* relocations (§9): the tables we already parse and never used         */
/* ------------------------------------------------------------------ */
/* Applied into the shadow before a run starts, so a GOT-mediated call
   resolves to something with a name instead of to the zero the file
   holds.  A symbol this file does not define gets a synthetic stub
   address; an IFUNC gets one too and its resolver is never run (§4.2) --
   that resolver dereferences ld.so's data, which is not in this file, so
   running it would be a coin flip dressed up as computation.        */

static int reloc_kind(int machine, uint32_t type){
    /* 1 = the addend alone, 2 = the symbol, 3 = an IFUNC stub, 0 = skip */
    if (machine == 62) switch (type){            /* x86-64 */
        case 8: return 1;                        /* RELATIVE  */
        case 6: case 7: case 1: return 2;        /* GLOB_DAT, JUMP_SLOT, 64 */
        case 37: return 3;                       /* IRELATIVE */
        default: return 0; }
    if (machine == 3) switch (type){             /* i386 */
        case 8: return 1; case 6: case 7: case 1: return 2;
        case 42: return 3; default: return 0; }
    if (machine == 183) switch (type){           /* AArch64 */
        case 1027: return 1; case 1025: case 1026: case 257: return 2;
        case 1032: return 3; default: return 0; }
    return 0;
}

void vm_relocs(Vm *m){
    const Elf *e = m->elf;
    if (!e) return;
    int w = e->is64 ? 8 : 4;
    for (int i = 0; i < e->nsec; i++){
        const Sec *s = &e->sec[i];
        int rela = (s->type == 4);                       /* SHT_RELA */
        if (!rela && s->type != 9) continue;             /* SHT_REL  */
        if (!s->data || !s->entsize) continue;
        const Sec *sy = ((int)s->link > 0 && (int)s->link < e->nsec) ? &e->sec[s->link] : NULL;
        const Sec *str = (sy && (int)sy->link > 0 && (int)sy->link < e->nsec)
                         ? &e->sec[sy->link] : NULL;
        uint64_t n = s->datasz / s->entsize;
        for (uint64_t k = 0; k < n && m->nRelocs < 20000; k++){
            const uint8_t *p = s->data + k * s->entsize;
            uint64_t off  = elf_rdw(e, p);
            uint64_t info = elf_rdw(e, p + w);
            int64_t  add  = rela ? (int64_t)elf_rdw(e, p + 2 * (uint64_t)w) : 0;
            uint32_t type = e->is64 ? (uint32_t)(info & 0xffffffffu) : (uint32_t)(info & 0xff);
            uint64_t sym  = e->is64 ? (info >> 32) : (info >> 8);
            int kind = reloc_kind(e->machine, type);
            if (!kind || !off) continue;

            uint64_t val;
            if (kind == 1){
                val = (uint64_t)add;              /* the file's own address space */
            } else {
                const char *nm = NULL;
                uint64_t sval = 0;
                int shndx = 0;
                if (sy && sy->data && sy->entsize &&
                    (sym + 1) * sy->entsize <= sy->datasz){
                    const uint8_t *q = sy->data + sym * sy->entsize;
                    uint32_t nameoff = elf_rd32(e, q);
                    if (e->is64){ shndx = elf_rd16(e, q + 6); sval = elf_rd64(e, q + 8); }
                    else        { sval = elf_rd32(e, q + 4); shndx = elf_rd16(e, q + 14); }
                    if (str && str->data && nameoff < str->datasz)
                        nm = (const char *)str->data + nameoff;
                }
                if (kind == 3){
                    char b[64];
                    snprintf(b, sizeof b, "ifunc@%llx", (unsigned long long)add);
                    val = vm_stub(m, nm && *nm ? nm : b);   /* never the resolver */
                } else if (shndx && sval){
                    val = sval + (uint64_t)add;             /* defined right here */
                } else {
                    val = vm_stub(m, nm && *nm ? nm : "unresolved");
                }
            }
            vm_poke(m, off, w, val);
            m->nRelocs++;
        }
    }
}

/* What is at the other end of a call.  A direct call in a dynamically
   linked binary almost always lands in `.plt`, whose only job is
   `jmp *GOT[n]` -- so the symbol is not at the call's target, it is in the
   relocation §9 already wrote into the shadow.  Following the one
   instruction the stub contains turns "call 0x1ef80" into "call memcpy".  */
int vm_is_stub(const Vm *m, uint64_t addr){
    for (int i = 0; i < m->nstub; i++)
        if (addr >= m->stub[i].addr && addr < m->stub[i].addr + 16) return 1;
    return 0;
}

const char *vm_callee_name(Vm *m, uint64_t addr){
    if (!m->elf || !addr) return NULL;
    for (int i = 0; i < m->nstub; i++)
        if (addr >= m->stub[i].addr && addr < m->stub[i].addr + 16)
            return m->stub[i].name;
    const char *sym = elf_sym_at(m->elf, addr, NULL);
    if (sym && *sym) return sym;

    const Sec *plt = NULL;
    for (int i = 0; i < m->elf->nsec; i++){
        const Sec *sc = &m->elf->sec[i];
        if (strncmp(sc->name, ".plt", 4) || !sc->data || !sc->addr) continue;
        if (addr >= sc->addr && addr < sc->addr + sc->datasz){ plt = sc; break; }
    }
    if (!plt) return NULL;

    uint64_t at = addr;
    for (int k = 0; k < 3 && at < plt->addr + plt->datasz; k++){
        InsnInfo in;
        const uint8_t *p = plt->data + (at - plt->addr);
        uint64_t left = plt->addr + plt->datasz - at;
        if (!disasm_info(p, (unsigned)(left > 15 ? 15 : left), at, &in)) break;
        unsigned len = 0;
        {   /* disasm_info does not hand back the length, so ask again */
            const Insn *dummy = NULL; (void)dummy;
            for (unsigned l = 1; l <= 15 && l <= left; l++){
                InsnInfo t2;
                if (disasm_info(p, l, at, &t2) && t2.id == in.id){ len = l; break; }
            }
        }
        if (!len) break;
        for (int o = 0; o < in.nops; o++){
            const InsnOp *op = &in.op[o];
            if (op->kind != OPK_MEM || !op->base) continue;
            const char *bn = disasm_reg_name(op->base);
            if (!bn || strcmp(bn, "rip")) continue;
            uint64_t got = at + len + (uint64_t)op->disp;
            uint64_t v;
            Prov pr;
            if (!vm_peek(m, got, 8, &v, &pr)) continue;
            for (int i = 0; i < m->nstub; i++)
                if (v >= m->stub[i].addr && v < m->stub[i].addr + 16)
                    return m->stub[i].name;
            const char *s2 = elf_sym_at(m->elf, v, NULL);
            if (s2 && *s2) return s2;
        }
        at += len;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* naming a value                                                      */
/* ------------------------------------------------------------------ */

const char *vm_annotate(const Vm *m, uint64_t v, char *buf, size_t n){
    buf[0] = 0;
    if (!m->elf || !v) return buf;
    if (v == m->sentinel){ snprintf(buf, n, "the caller"); return buf; }
    for (int i = 0; i < m->nstub; i++)
        if (v >= m->stub[i].addr && v < m->stub[i].addr + 16){
            snprintf(buf, n, "%s (not in this file)", m->stub[i].name);
            return buf;
        }
    if (v >= m->stackLo && v < m->stackHi){
        long off = (long)(v - m->r[m->spSlot >= 0 ? m->spSlot : 0].v);
        snprintf(buf, n, "stack%+ld", off);
        return buf;
    }
    uint64_t off = 0;
    const char *sym = elf_sym_at(m->elf, v, &off);
    if (sym && *sym){
        if (off) snprintf(buf, n, "%s+0x%llx", sym, (unsigned long long)off);
        else     snprintf(buf, n, "%s", sym);
        return buf;
    }
    for (int i = 0; i < m->elf->nsec; i++){
        const Sec *s = &m->elf->sec[i];
        if (!(s->flags & 0x2) || !s->addr || !s->size) continue;
        if (v < s->addr || v >= s->addr + s->size) continue;
        /* §10.1: if it points at a string, say the string.  That is the
           difference between a panel that dumps hex and one that
           explains, and it costs a bounds check and a loop.        */
        if (s->data && v - s->addr < s->datasz){
            const uint8_t *p = s->data + (v - s->addr);
            uint64_t room = s->datasz - (v - s->addr), k = 0;
            while (k < room && k < 40 && p[k] >= 32 && p[k] < 127) k++;
            if (k >= 4 && k < room && p[k] == 0){
                snprintf(buf, n, "\"%.*s\"%s", (int)(k > 28 ? 28 : k), (const char *)p,
                         k > 28 ? "..." : "");
                return buf;
            }
        }
        snprintf(buf, n, "%s+0x%llx", s->name, (unsigned long long)(v - s->addr));
        return buf;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

void vm_init(Vm *m, const Elf *e, uint64_t entry, uint64_t seed){
    memset(m, 0, sizeof *m);
    m->elf = e;
    m->seed = seed;
    m->rng = seed ? seed : 0x2545F4914F6CDD1Dull;
    m->fuel = VM_FUEL;
    m->pcSlot = m->spSlot = -1;
    m->vecSlot = -1;
    m->rmap = malloc(VM_REGIDS * sizeof *m->rmap);
    m->vmap = malloc(VM_REGIDS * sizeof *m->vmap);
    m->rwid = calloc(VM_REGIDS, 1);
    m->roff = calloc(VM_REGIDS, 1);
    m->vwid = calloc(VM_REGIDS, 1);
    for (int i = 0; i < VM_REGIDS; i++){ m->rmap[i] = -1; m->vmap[i] = -1; }
    for (unsigned id = 1; id < VM_REGIDS; id++){
        const char *nm = disasm_reg_name(id);
        int sl, by;
        if (nm && *nm && vec_name(nm, &sl, &by)){
            m->vmap[id] = (int16_t)sl;
            m->vwid[id] = (uint8_t)by;
            if (sl + 1 > m->nvreg) m->nvreg = sl + 1;
        }
    }

    /* the synthetic stack of §5.  Tier 0 has no stack semantics -- push and
       ret are havoc like anything else -- but the region has to exist for
       the memory layers to have a third answer, and for a minted pointer
       to have somewhere plausible to point.                            */
    m->stackLo  = 0x00007ffffff00000ull;
    m->stackHi  = m->stackLo + 0x10000;
    m->fsBase   = 0x00007ffff7d00000ull;
    /* heapBase is the window invented pointers land in and never moves --
       two of them have to stay near each other.  heapNext is what a
       modelled malloc hands out, and does.                          */
    m->heapBase = 0x0000555555600000ull;
    m->heapNext = 0x0000555555700000ull;
    m->stubNext = 0x00007ffff0000000ull;
    m->sentinel = VM_SENTINEL;

    switch (disasm_machine()){
    case 62: map_regs(m, X86_64_REGS, (int)(sizeof X86_64_REGS / sizeof *X86_64_REGS)); break;
    case 3:  map_regs(m, X86_32_REGS, (int)(sizeof X86_32_REGS / sizeof *X86_32_REGS)); break;
    case 40: map_regs(m, ARM_REGS,    (int)(sizeof ARM_REGS    / sizeof *ARM_REGS));    break;
    case 183:map_regs(m, ARM64_REGS,  (int)(sizeof ARM64_REGS  / sizeof *ARM64_REGS));  break;
    default: break;      /* slots claimed as instructions touch registers */
    }
    for (int i = 0; i < m->nreg; i++){
        if (!strcmp(m->rname[i], "rip") || !strcmp(m->rname[i], "eip") ||
            !strcmp(m->rname[i], "pc")) m->pcSlot = i;
        if (!strcmp(m->rname[i], "rsp") || !strcmp(m->rname[i], "esp") ||
            !strcmp(m->rname[i], "sp"))  m->spSlot = i;
    }
    if (m->spSlot >= 0){
        /* The frame a run starts in: rsp in the middle of the region with
           the sentinel return address already on it, and rbp equal to it,
           which is what `leave` and the epilogue expect to find.      */
        uint64_t sp = m->stackLo + 0x8000;
        int w = m->nreg > 8 ? 8 : 4;
        vm_poke(m, sp, w, m->sentinel);
        m->r[m->spSlot].v = sp;
        m->r[m->spSlot].prov = PV_DERIVED;
        int bp = vm_slot_named(m, w == 8 ? "rbp" : "ebp");
        if (bp >= 0){ m->r[bp].v = sp; m->r[bp].prov = PV_DERIVED; }
    }
    /* One constant stack canary at fs:0x28.  `git` alone loads it 4466
       times; without a value that compares equal at the end of a function,
       every protected function fails its check and dives into
       __stack_chk_fail.  §5 asks for this in the first version.     */
    m->canary = vm_mint(m) | 0xff00ull;
    m->canary &= ~0xffull;                  /* the real one ends in a NUL byte */
    vm_poke(m, m->fsBase + 0x28, 8, m->canary);
    if (m->pcSlot >= 0){
        m->r[m->pcSlot].v = entry;
        m->r[m->pcSlot].prov = PV_DERIVED;
    }
    /* the GOT, before anything reads it */
    vm_relocs(m);
}

void vm_free(Vm *m){
    for (int i = 0; i < VM_BUCKETS; i++){
        VmPage *p = m->pg[i];
        while (p){ VmPage *n = p->next; free(p); p = n; }
        m->pg[i] = NULL;
    }
    free(m->stub); m->stub = NULL; m->nstub = m->capstub = 0;
    free(m->rmap); m->rmap = NULL;
    free(m->rwid); m->rwid = NULL;
    free(m->roff); m->roff = NULL;
    free(m->vmap); m->vmap = NULL;
    free(m->vwid); m->vwid = NULL;
    m->npg = 0;
}

/* ------------------------------------------------------------------ */
/* tier 0                                                              */
/* ------------------------------------------------------------------ */

/* Capstone's eflags mask says which flags an instruction modifies, sets,
   resets or tests.  Anything in the first three we no longer know.    */
static uint8_t flags_disturbed(uint64_t ef){
    uint8_t k = 0;
    if (ef & (X86_EFLAGS_MODIFY_CF | X86_EFLAGS_RESET_CF | X86_EFLAGS_SET_CF)) k |= 1u << VF_CF;
    if (ef & (X86_EFLAGS_MODIFY_PF | X86_EFLAGS_RESET_PF))                     k |= 1u << VF_PF;
    if (ef & (X86_EFLAGS_MODIFY_AF | X86_EFLAGS_RESET_AF))                     k |= 1u << VF_AF;
    if (ef & (X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_RESET_ZF))                     k |= 1u << VF_ZF;
    if (ef & (X86_EFLAGS_MODIFY_SF | X86_EFLAGS_RESET_SF))                     k |= 1u << VF_SF;
    if (ef & (X86_EFLAGS_MODIFY_OF | X86_EFLAGS_RESET_OF))                     k |= 1u << VF_OF;
    return k;
}

static void tier0(Vm *m, const InsnInfo *ac){
    m->nTier0++;
    if (!ac->full && !ac->nrd && !ac->nwr) m->nNoAccess++;
    for (int i = 0; i < ac->nrd; i++){
        int vs = vm_vslot(m, ac->rd[i]);
        if (vs >= 0){ uint8_t tmp[VM_VBYTES]; vm_vget(m, vs, VM_VBYTES, tmp); }
        else reg_touch(m, reg_slot(m, ac->rd[i]));
    }
    for (int i = 0; i < ac->nwr; i++){
        int vs = vm_vslot(m, ac->wr[i]);
        if (vs >= 0){
            uint8_t tmp[VM_VBYTES];
            for (int k = 0; k < VM_VBYTES; k++) tmp[k] = (uint8_t)(vm_mint(m) & 0xff);
            vm_vset(m, vs, 16, tmp, PV_INVENTED);   /* the low lane, as SSE does */
        } else reg_havoc(m, reg_slot(m, ac->wr[i]));
    }
    m->flknown &= (uint8_t)~flags_disturbed(ac->eflags);
}

/* Tier 1 first, tier 0 for whatever it will not take.  The tiers are not a
   fallback chain by accident: tier 1 declines cleanly, before any side
   effect, so an instruction it does not model is still *disturbed*
   correctly rather than half-executed.                               */
static int exec_one(Vm *m, const Insn *in, const uint8_t *bytes,
                    int *cond, uint64_t *ret, const InsnInfo **out){
    static InsnInfo ac;
    *cond = -1; *ret = 0; *out = NULL;
    if (!disasm_info(bytes, in->len, in->addr, &ac)){
        m->nTier0++; m->nNoAccess++;
        return 0;
    }
    *out = &ac;
    int mach = disasm_machine();
    if (mach == 3 || mach == 62){
        int rc = vmx86_exec(m, in, &ac, cond, ret);
        if (rc){ m->nTier1++; return rc; }
        int tier = 3;
        if (vmsimd_exec(m, in, &ac, &tier)){
            if (tier == 2) m->nTier2++; else m->nTier3++;
            return 1;
        }
    }
    tier0(m, &ac);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the walk (§6)                                                       */
/* ------------------------------------------------------------------ */

/* With no flag model there is nothing to decide a conditional with, so
   guess -- but guess with a policy, so loops go round a plausible number
   of times and the room does not turn into a coin-flipping machine.  A
   guessed edge is drawn in a different colour from a decided one, which
   is the cheapest honesty feature in the whole design.               */
static int guess_branch(Vm *m, const Insn *in, int visits){
    if (in->taddr && in->taddr <= in->addr) return visits < VM_TRIPS;   /* a loop */
    return (sm64(&m->rng) % 100) < 35;                  /* forward: mostly falls through */
}

StepKind vm_step(Vm *m, const Insn *in, const uint8_t *bytes, int visits,
                 uint64_t *next, int *decided){
    uint64_t fall = in->addr + in->len;
    *next = fall;
    *decided = 1;
    if (m->fuel <= 0){ *next = in->addr; return ST_STOP; }
    m->fuel--;
    m->steps++;

    int cond = -1;
    uint64_t retaddr = 0;
    const InsnInfo *info = NULL;
    int rc = exec_one(m, in, bytes, &cond, &retaddr, &info);
    m->mintHintOn = 0;

    StepKind k = ST_FALL;
    if (rc == 5){                       /* hlt, ud2, int3: nothing follows */
        *decided = 0;
        if (m->pcSlot >= 0){ m->r[m->pcSlot].v = in->addr; }
        return ST_STOP;
    }
    switch (in->cls){
    case IC_RET:
        /* rc 3 means a real pop happened.  The sentinel is the return
           address vm_init put on the frame: popping it means the function
           returned to its caller and the run is done.  Anything else is a
           return into this same room, which the wisp resolves.       */
        if (rc == 3 && retaddr != m->sentinel){
            if (m->ncall > 0) m->ncall--;
            *next = retaddr; k = ST_TAKEN;
        } else k = ST_RET;
        break;

    case IC_CALL: {
        /* Stepped over by default: following it needs another room decoded,
           which the city deliberately will not do (§8).  An indirect call
           still gets a name, because rc 4 resolved where it was going.
           In pilot mode the player moves with the wisp, so the call can
           actually be made.                                          */
        uint64_t to = in->taddr ? in->taddr : (rc == 4 ? retaddr : 0);
        const char *nm = to ? vm_callee_name(m, to) : NULL;
        /* Not into a symbol this file does not define: there is nothing
           there to walk, so the step-over contract is the honest answer
           even in pilot mode.                                        */
        /* The one hand-off every dynamically linked C program makes.
           __libc_start_main is not in this file and never will be, but its
           first argument is main -- which is, and is where the walk the
           whole feature exists for actually begins.                   */
        if (m->follow && to && vm_is_stub(m, to) && nm &&
            !strncmp(nm, "__libc_start_", 13)){
            int di = vm_slot_named(m, m->nreg > 8 ? "rdi" : "eax");
            uint64_t main_ = vm_slot_get(m, di);
            if (main_ && !vm_is_stub(m, main_)){
                snprintf(m->lastCall, sizeof m->lastCall, "%s -> main", nm);
                m->lastCallRv = 0; m->nCalls++; m->nCallsNamed++;
                *next = main_;
                k = ST_CALL_INTO;
                break;
            }
        }
        if (m->follow && to && !vm_is_stub(m, to) &&
            m->ncall < VM_CALLDEPTH && m->spSlot >= 0){
            int w = m->nreg > 8 ? 8 : 4;
            uint64_t sp = vm_slot_get(m, m->spSlot) - (uint64_t)w;
            vm_slot_set(m, m->spSlot, sp, PV_DERIVED);
            vm_write(m, sp, w, fall);        /* a real return address */
            m->ncall++;
            snprintf(m->lastCall, sizeof m->lastCall, "%s", nm ? nm : "an unnamed callee");
            m->lastCallRv = 0;
            m->nCalls++;
            if (nm) m->nCallsNamed++;
            *next = to;
            k = ST_CALL_INTO;
            break;
        }
        if (disasm_machine() == 3 || disasm_machine() == 62)
            vmx86_call_over(m, in, nm);
        k = ST_CALL_OVER;
        break;
    }

    case IC_JUMP: {
        uint64_t to = in->taddr ? in->taddr : (rc == 4 ? retaddr : 0);
        if (!to){ *decided = 0; k = ST_STOP; break; }
        /* A jump into a symbol this file does not define is a tail call --
           which is to say a call and a return in one instruction.  The PLT
           thunk that ends `jmp *GOT[n]` is the shape it almost always
           takes.  Running the contract and then returning is what keeps a
           walk going through the caller chain instead of stopping at the
           first library function.                                     */
        if (vm_is_stub(m, to)){
            if (disasm_machine() == 3 || disasm_machine() == 62)
                vmx86_call_over(m, in, vm_callee_name(m, to));
            if (m->spSlot >= 0){
                int w = m->nreg > 8 ? 8 : 4;
                Prov p;
                uint64_t sp = vm_slot_get(m, m->spSlot);
                uint64_t ra = vm_read(m, sp, w, &p);
                vm_slot_set(m, m->spSlot, sp + (uint64_t)w, PV_DERIVED);
                if (ra == m->sentinel){ k = ST_RET; break; }
                if (m->ncall > 0) m->ncall--;
                *next = ra; k = ST_TAKEN;
                break;
            }
        }
        *next = to;
        k = (in->taddr && in->port >= 0) ? ST_EXIT_PORT : ST_TAKEN;
        break;
    }

    case IC_CJUMP: {
        if (!in->taddr){ *decided = 0; k = ST_STOP; break; }
        m->nCond++;
        int taken;
        if (cond >= 0){                     /* real arithmetic decided it */
            taken = cond; *decided = 1; m->nDecided++;
        } else {
            taken = guess_branch(m, in, visits); *decided = 0; m->nGuessed++;
        }
        if (!taken){ k = ST_NOT_TAKEN; break; }
        *next = in->taddr;
        k = in->port >= 0 ? ST_EXIT_PORT : ST_TAKEN;
        break;
    }

    default:
        break;
    }

    /* the pc follows the decision, not the fall-through: a taken branch
       must leave rip on the target or the panel contradicts the ring */
    if (m->pcSlot >= 0){
        m->r[m->pcSlot].v = *next;
        m->r[m->pcSlot].prov = PV_DERIVED;
        m->r[m->pcSlot].stamp = m->steps;
    }
    return k;
}
