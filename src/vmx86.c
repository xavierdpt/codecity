/* vmx86.c -- tier 1: what the instructions actually do, on x86 and x86-64.
 *
 * Everything here was checked against hardware.  tools/oracle.py single-steps
 * each of these forms on this machine's CPU from a chosen register state and
 * records the result in tests/vm-corpus.txt; --vmtest replays the corpus
 * through this file and fails on any divergence.  Five rules in here came
 * out of that and would otherwise have shipped as plausible wrong numbers:
 *
 *   - a shift count is masked to 5 bits at width 4 and 6 bits at width 8,
 *   - a shift of zero changes no register and no flag at all,
 *   - `inc`/`dec` leave CF exactly as they found it,
 *   - `bsf`/`bsr` leave their destination alone when the source is zero,
 *   - a write at width 4 zero-extends the whole slot; at 1 and 2 it merges
 *     (that last one lives in vm.c, with the register file).
 *
 * Which flags an instruction touches is not hard-coded: Capstone's eflags
 * mask says which it modifies and which it leaves architecturally
 * undefined, and an undefined flag has its *known* bit cleared rather than
 * being given a number we cannot justify.  That is the same honesty rule as
 * tier 0, applied one level up.
 */
#define _GNU_SOURCE
#include "vm.h"
#include "disasm.h"
#include <capstone/capstone.h>
#include <string.h>

/* ---- flags ---------------------------------------------------------- */

typedef struct { int cf, pf, af, zf, sf, of; } Fl;

static const uint64_t FL_MOD[VF_COUNT] = {
    X86_EFLAGS_MODIFY_CF | X86_EFLAGS_SET_CF | X86_EFLAGS_RESET_CF,
    X86_EFLAGS_MODIFY_PF | X86_EFLAGS_RESET_PF,
    X86_EFLAGS_MODIFY_AF | X86_EFLAGS_RESET_AF,
    X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_RESET_ZF,
    X86_EFLAGS_MODIFY_SF | X86_EFLAGS_RESET_SF,
    X86_EFLAGS_MODIFY_OF | X86_EFLAGS_RESET_OF,
};
static const uint64_t FL_UNDEF[VF_COUNT] = {
    X86_EFLAGS_UNDEFINED_CF, X86_EFLAGS_UNDEFINED_PF, X86_EFLAGS_UNDEFINED_AF,
    X86_EFLAGS_UNDEFINED_ZF, X86_EFLAGS_UNDEFINED_SF, X86_EFLAGS_UNDEFINED_OF,
};
static const uint64_t FL_TEST[VF_COUNT] = {
    X86_EFLAGS_TEST_CF, X86_EFLAGS_TEST_PF, X86_EFLAGS_TEST_AF,
    X86_EFLAGS_TEST_ZF, X86_EFLAGS_TEST_SF, X86_EFLAGS_TEST_OF,
};

/* Apply the computed flags, but only where Capstone says the instruction
   really touches them; an undefined flag becomes unknown. */
static void put_flags(Vm *m, uint64_t ef, const Fl *f){
    const int v[VF_COUNT] = { f->cf, f->pf, f->af, f->zf, f->sf, f->of };
    for (int i = 0; i < VF_COUNT; i++){
        if (ef & FL_MOD[i]){
            if (v[i] >= 0){ m->fl[i] = (uint8_t)(v[i] != 0); m->flknown |= (uint8_t)(1u << i); }
            else            m->flknown &= (uint8_t)~(1u << i);
        } else if (ef & FL_UNDEF[i]){
            m->flknown &= (uint8_t)~(1u << i);
        }
    }
}

static uint64_t msk(int w){ return w >= 8 ? ~0ull : ((1ull << (w * 8)) - 1); }
static int      msb(uint64_t v, int w){ return (int)((v >> (w * 8 - 1)) & 1); }
static int      parity(uint64_t v){
    unsigned char b = (unsigned char)v;
    b ^= (unsigned char)(b >> 4); b ^= (unsigned char)(b >> 2); b ^= (unsigned char)(b >> 1);
    return !(b & 1);
}
static int64_t  sext(uint64_t v, int w){
    if (w >= 8) return (int64_t)v;
    uint64_t sb = 1ull << (w * 8 - 1);
    return (int64_t)((v ^ sb) - sb);
}

/* the flags after a bitwise result: CF and OF cleared, AF undefined */
static Fl logic_flags(uint64_t r, int w){
    Fl f = { 0, parity(r), -1, r == 0, msb(r, w), 0 };
    return f;
}

/* ---- operands ------------------------------------------------------- */

typedef struct {
    Vm             *m;
    const Insn     *in;
    const InsnInfo *info;
    int             fault;      /* an operand we could not evaluate */
    int             prov;       /* where the last source read came from */
} Ctx;

static uint64_t ea_of(Ctx *c, const InsnOp *o){
    Vm *m = c->m;
    uint64_t a = 0;
    if (o->base){
        const char *nm = disasm_reg_name(o->base);
        if (nm && (!strcmp(nm, "rip") || !strcmp(nm, "eip")))
            a += c->in->addr + c->in->len;      /* rip-relative is off the *next* */
        else
            a += vm_reg_get(m, o->base);
    }
    if (o->index) a += vm_reg_get(m, o->index) * (uint64_t)(o->scale ? o->scale : 1);
    a += (uint64_t)o->disp;
    if (o->seg){
        const char *nm = disasm_reg_name(o->seg);
        if (nm && (!strcmp(nm, "fs") || !strcmp(nm, "gs"))) a += m->fsBase;
    }
    /* a 32-bit address size truncates, which is how `lea eax, [ebx+ecx*2]`
       differs from the 64-bit form */
    if (o->base && vm_reg_width(m, o->base) == 4) a &= 0xffffffffull;
    else if (!o->base && o->index && vm_reg_width(m, o->index) == 4) a &= 0xffffffffull;
    return a;
}

static uint64_t src(Ctx *c, int i){
    const InsnOp *o = &c->info->op[i];
    switch (o->kind){
    case OPK_REG: return vm_reg_get(c->m, o->reg);
    /* Capstone hands back an immediate already sign-extended to 64 bits.
       Masking it to its encoded width here would turn `mov rax, -1` into
       `mov rax, 0xffffffff`, so leave it alone and let the arithmetic mask
       to the destination.                                            */
    case OPK_IMM: return (uint64_t)o->imm;
    case OPK_MEM: {
        Prov p = PV_DERIVED;
        uint64_t v = vm_read(c->m, ea_of(c, o), o->size ? o->size : 8, &p);
        c->prov = p;            /* real file bytes stay marked as such */
        return v;
    }
    }
    c->fault = 1;
    return 0;
}

/* A computed value is PV_DERIVED.  A *moved* one keeps the provenance of
   where it came from, so `mov rdi, [rip+0x1234]` still reads as file bytes
   and can be named after the string it points at.                    */
static void dst_prov(Ctx *c, int i, uint64_t v, int prov);

static void dst(Ctx *c, int i, uint64_t v){ dst_prov(c, i, v, PV_DERIVED); }

static void dst_move(Ctx *c, int i, uint64_t v){ dst_prov(c, i, v, c->prov); }

static void dst_prov(Ctx *c, int i, uint64_t v, int prov){
    const InsnOp *o = &c->info->op[i];
    switch (o->kind){
    case OPK_REG: vm_reg_set(c->m, o->reg, v, prov); return;
    case OPK_MEM: vm_write(c->m, ea_of(c, o), o->size ? o->size : 8, v); return;
    default: c->fault = 1; return;
    }
}

static int opw(Ctx *c, int i){
    int w = c->info->op[i].size;
    return w ? w : 8;
}

/* ---- the stack ------------------------------------------------------ */

static int sp_width(const Vm *m){ return m->spSlot >= 0 && m->nreg > 8 ? 8 : 4; }

static void push_val(Vm *m, uint64_t v, int w){
    uint64_t sp = vm_slot_get(m, m->spSlot) - (uint64_t)w;
    vm_slot_set(m, m->spSlot, sp, PV_DERIVED);
    vm_write(m, sp, w, v);
}

static uint64_t pop_val(Vm *m, int w){
    Prov p;
    uint64_t sp = vm_slot_get(m, m->spSlot);
    uint64_t v = vm_read(m, sp, w, &p);
    vm_slot_set(m, m->spSlot, sp + (uint64_t)w, PV_DERIVED);
    return v;
}

/* ---- the sixteen conditions ----------------------------------------- */

/* 1 taken, 0 not taken, -1 a flag it needs is not known */
static int cond_of(const Vm *m, unsigned id, uint64_t ef){
    for (int i = 0; i < VF_COUNT; i++)
        if ((ef & FL_TEST[i]) && !(m->flknown & (1u << i))) return -1;
    int CF = m->fl[VF_CF], PF = m->fl[VF_PF], ZF = m->fl[VF_ZF];
    int SF = m->fl[VF_SF], OF = m->fl[VF_OF];
    switch (id){
    case X86_INS_JO:  case X86_INS_SETO:  case X86_INS_CMOVO:  return OF;
    case X86_INS_JNO: case X86_INS_SETNO: case X86_INS_CMOVNO: return !OF;
    case X86_INS_JB:  case X86_INS_SETB:  case X86_INS_CMOVB:  return CF;
    case X86_INS_JAE: case X86_INS_SETAE: case X86_INS_CMOVAE: return !CF;
    case X86_INS_JE:  case X86_INS_SETE:  case X86_INS_CMOVE:  return ZF;
    case X86_INS_JNE: case X86_INS_SETNE: case X86_INS_CMOVNE: return !ZF;
    case X86_INS_JBE: case X86_INS_SETBE: case X86_INS_CMOVBE: return CF || ZF;
    case X86_INS_JA:  case X86_INS_SETA:  case X86_INS_CMOVA:  return !CF && !ZF;
    case X86_INS_JS:  case X86_INS_SETS:  case X86_INS_CMOVS:  return SF;
    case X86_INS_JNS: case X86_INS_SETNS: case X86_INS_CMOVNS: return !SF;
    case X86_INS_JP:  case X86_INS_SETP:  case X86_INS_CMOVP:  return PF;
    case X86_INS_JNP: case X86_INS_SETNP: case X86_INS_CMOVNP: return !PF;
    case X86_INS_JL:  case X86_INS_SETL:  case X86_INS_CMOVL:  return SF != OF;
    case X86_INS_JGE: case X86_INS_SETGE: case X86_INS_CMOVGE: return SF == OF;
    case X86_INS_JLE: case X86_INS_SETLE: case X86_INS_CMOVLE: return ZF || SF != OF;
    case X86_INS_JG:  case X86_INS_SETG:  case X86_INS_CMOVG:  return !ZF && SF == OF;
    case X86_INS_JCXZ:   return vm_reg_get((Vm *)m, X86_REG_CX)  == 0;
    case X86_INS_JECXZ:  return vm_reg_get((Vm *)m, X86_REG_ECX) == 0;
    case X86_INS_JRCXZ:  return vm_reg_get((Vm *)m, X86_REG_RCX) == 0;
    default: return -1;
    }
}

int vmx86_cond(Vm *m, const InsnInfo *info){
    return cond_of(m, info->id, info->eflags);
}

/* ---- stepping over a call ------------------------------------------- */
/* The callee is in another room and the city keeps exactly one decoded at
   a time (§8), so the call is not followed.  What is emulated instead is
   the *contract*: rax is whatever it is pretending to have returned, the
   caller-saved registers are gone, and rsp is untouched -- there was no
   real call, so there is no return address to leave behind.        */

static const struct { const char *name; int kind; } LIBC[] = {
    /* 0 a small non-negative int      1 a fresh heap pointer
       2 the first argument back       3 zero, meaning success
       4 a pointer into the argument, or NULL
       5 a comparison: equal a third of the time, else either way
       6 a pointer, or NULL a third of the time                     */
    { "strlen", 0 }, { "strnlen", 0 }, { "isatty", 0 }, { "getpid", 0 },
    { "atoi", 0 }, { "read", 0 }, { "write", 0 }, { "snprintf", 0 },
    { "malloc", 1 }, { "calloc", 1 }, { "realloc", 1 }, { "strdup", 1 },
    { "xmalloc", 1 }, { "xcalloc", 1 }, { "xrealloc", 1 }, { "xstrdup", 1 },
    { "memcpy", 2 }, { "memmove", 2 }, { "memset", 2 }, { "strcpy", 2 },
    { "strcat", 2 }, { "stpcpy", 2 }, { "mempcpy", 2 },
    { "free", 3 }, { "close", 3 }, { "fflush", 3 }, { "fclose", 3 },
    { "pthread_mutex_lock", 3 }, { "pthread_mutex_unlock", 3 },
    /* A search that never finds anything is a loop that never ends, so
       these have to be able to say "yes" and "nothing".             */
    { "strcmp", 5 }, { "strncmp", 5 }, { "memcmp", 5 }, { "strcasecmp", 5 },
    { "strncasecmp", 5 }, { "strcoll", 5 },
    { "strchr", 4 }, { "strrchr", 4 }, { "strstr", 4 }, { "memchr", 4 },
    { "getenv", 6 }, { "fopen", 6 }, { "opendir", 6 }, { "readdir", 6 },
    { "fgets", 6 }, { "dlsym", 6 },
};

void vmx86_call_over(Vm *m, const Insn *in, const char *callee){
    (void)in;
    static const char *SCRATCH[] = { "rcx","rdx","rsi","rdi","r8","r9","r10","r11" };
    uint64_t arg0 = vm_slot_get(m, vm_slot_named(m, "rdi"));
    /* What an unrecognised callee returns.  Zero often enough that
       `while (fn(x))` ends: a non-zero-by-default return is the single
       commonest way a run walks until its fuel is gone.            */
    uint64_t rv = vm_mint(m);
    rv = (rv % 5) ? (rv >> 8) & 0x3f : 0;
    int kind = -1;
    if (callee){
        const char *n = callee;
        while (*n == '_' || *n == '*') n++;                  /* __strlen, *strlen */
        for (size_t i = 0; i < sizeof LIBC / sizeof *LIBC; i++){
            size_t l = strlen(LIBC[i].name);
            if (!strncmp(n, LIBC[i].name, l) &&
                (!n[l] || n[l] == '@' || n[l] == '_')){ kind = LIBC[i].kind; break; }
        }
    }
    uint64_t coin = vm_mint(m);
    switch (kind){
    case 0: rv = coin & 0x3f; break;
    case 1: rv = m->heapNext; m->heapNext += 0x400; break;   /* a pointer later
                                                                reads work against */
    case 2: rv = arg0; break;
    case 3: rv = 0; break;
    case 4: rv = (coin % 3) ? (arg0 ? arg0 + (coin & 0x1f) : 0) : 0; break;
    case 5: rv = (coin % 3) ? (uint64_t)((coin & 1) ? 1 : -1) : 0; break;
    case 6: rv = (coin % 3) ? (m->heapNext += 0x400) : 0; break;
    default: break;
    }
    for (size_t i = 0; i < sizeof SCRATCH / sizeof *SCRATCH; i++){
        int s = vm_slot_named(m, SCRATCH[i]);
        if (s >= 0) vm_slot_set(m, s, vm_mint(m), PV_CALL);
    }
    int ax = vm_slot_named(m, "rax");
    if (ax >= 0) vm_slot_set(m, ax, rv, PV_CALL);
    m->flknown = 0;                       /* a callee clobbers the flags too */
    m->nCalls++;
    if (callee) m->nCallsNamed++;
    if (kind >= 0) m->nCallsModelled++;
    snprintf(m->lastCall, sizeof m->lastCall, "%s%s",
             callee ? callee : "an unnamed callee",
             kind >= 0 ? " (modelled)" : "");
    m->lastCallRv = rv;
}

/* ---- the interpreter ------------------------------------------------ */

int vmx86_exec(Vm *m, const Insn *in, const InsnInfo *info, int *cond, uint64_t *ret){
    Ctx cx = { m, in, info, 0, PV_DERIVED };
    Ctx *c = &cx;
    uint64_t ef = info->eflags;
    unsigned id = info->id;
    *cond = -1;

    /* §5: tell the minter what this instruction is going to do with a value
       it has to invent.  Only for the comparisons, where the immediate is
       the loop bound and inventing something near it is what makes the
       loop run a plausible number of times.                          */
    m->mintHintOn = 0;
    if ((id == X86_INS_CMP || id == X86_INS_SUB || id == X86_INS_TEST ||
         id == X86_INS_ADD) && info->nops == 2 && info->op[1].kind == OPK_IMM){
        int64_t v = info->op[1].imm;
        if (v > 0 && v <= 4096){ m->mintHint = (uint64_t)v; m->mintHintOn = 1; }
    }

    switch (id){

    /* ---- nothing at all ------------------------------------------- */
    case X86_INS_NOP: case X86_INS_ENDBR64: case X86_INS_ENDBR32:
    case X86_INS_PAUSE: case X86_INS_FNOP:
        return 1;

    /* ---- control does not continue past these ---------------------- */
    /* Classified as padding, which they are -- but a walker that treats
       `hlt` as a no-op strolls out of the end of _start and into whatever
       alignment bytes follow it.                                     */
    case X86_INS_HLT: case X86_INS_UD2: case X86_INS_UD0:
    case X86_INS_INT3: case X86_INS_INT1:
        return 5;

    /* ---- moves ----------------------------------------------------- */
    case X86_INS_MOV: case X86_INS_MOVABS: {
        c->prov = PV_DERIVED;
        uint64_t v = src(c, 1);
        dst_move(c, 0, v);
        return !c->fault;
    }

    case X86_INS_MOVZX: {
        c->prov = PV_DERIVED;
        uint64_t v = src(c, 1) & msk(opw(c, 1));
        dst_move(c, 0, v);
        return !c->fault;
    }

    case X86_INS_MOVSX: case X86_INS_MOVSXD: {
        c->prov = PV_DERIVED;
        uint64_t v = (uint64_t)sext(src(c, 1), opw(c, 1)) & msk(opw(c, 0));
        dst_move(c, 0, v);
        return !c->fault;
    }

    case X86_INS_LEA:
        dst(c, 0, ea_of(c, &info->op[1]) & msk(opw(c, 0)));
        return !c->fault;

    case X86_INS_XCHG: {
        uint64_t a = src(c, 0), b = src(c, 1);
        dst(c, 0, b); dst(c, 1, a);
        return !c->fault;
    }

    case X86_INS_CMOVO:  case X86_INS_CMOVNO: case X86_INS_CMOVB:  case X86_INS_CMOVAE:
    case X86_INS_CMOVE:  case X86_INS_CMOVNE: case X86_INS_CMOVBE: case X86_INS_CMOVA:
    case X86_INS_CMOVS:  case X86_INS_CMOVNS: case X86_INS_CMOVP:  case X86_INS_CMOVNP:
    case X86_INS_CMOVL:  case X86_INS_CMOVGE: case X86_INS_CMOVLE: case X86_INS_CMOVG: {
        int t = cond_of(m, id, ef);
        if (t < 0) return 0;                       /* unknown flags -> tier 0 */
        /* a 32-bit cmov zero-extends whatever happens, taken or not */
        dst(c, 0, t ? src(c, 1) : src(c, 0));
        return !c->fault;
    }

    case X86_INS_SETO:  case X86_INS_SETNO: case X86_INS_SETB:  case X86_INS_SETAE:
    case X86_INS_SETE:  case X86_INS_SETNE: case X86_INS_SETBE: case X86_INS_SETA:
    case X86_INS_SETS:  case X86_INS_SETNS: case X86_INS_SETP:  case X86_INS_SETNP:
    case X86_INS_SETL:  case X86_INS_SETGE: case X86_INS_SETLE: case X86_INS_SETG: {
        int t = cond_of(m, id, ef);
        if (t < 0) return 0;
        dst(c, 0, (uint64_t)t);
        return !c->fault;
    }

    /* ---- sign extension of the accumulator ------------------------- */
    case X86_INS_CWDE: vm_reg_set(m, X86_REG_EAX, (uint64_t)sext(vm_reg_get(m, X86_REG_AX), 2), PV_DERIVED); return 1;
    case X86_INS_CDQE: vm_reg_set(m, X86_REG_RAX, (uint64_t)sext(vm_reg_get(m, X86_REG_EAX), 4), PV_DERIVED); return 1;
    case X86_INS_CBW:  vm_reg_set(m, X86_REG_AX,  (uint64_t)sext(vm_reg_get(m, X86_REG_AL), 1), PV_DERIVED); return 1;
    case X86_INS_CWD:  vm_reg_set(m, X86_REG_DX,  sext(vm_reg_get(m, X86_REG_AX), 2)  < 0 ? 0xffff : 0, PV_DERIVED); return 1;
    case X86_INS_CDQ:  vm_reg_set(m, X86_REG_EDX, sext(vm_reg_get(m, X86_REG_EAX), 4) < 0 ? 0xffffffffu : 0, PV_DERIVED); return 1;
    case X86_INS_CQO:  vm_reg_set(m, X86_REG_RDX, sext(vm_reg_get(m, X86_REG_RAX), 8) < 0 ? ~0ull : 0, PV_DERIVED); return 1;

    /* ---- the stack -------------------------------------------------- */
    case X86_INS_PUSH: {
        int w = opw(c, 0);
        if (w < sp_width(m)) w = sp_width(m);      /* an imm8 still pushes 8 */
        push_val(m, src(c, 0), w);
        return !c->fault;
    }
    case X86_INS_POP: {
        int w = opw(c, 0);
        if (w < sp_width(m)) w = sp_width(m);
        c->prov = PV_DERIVED;
        uint64_t v = pop_val(m, w);
        dst_move(c, 0, v);
        return !c->fault;
    }
    case X86_INS_LEAVE: {
        int w = sp_width(m);
        int bp = vm_slot_named(m, w == 8 ? "rbp" : "ebp");
        vm_slot_set(m, m->spSlot, vm_slot_get(m, bp), PV_DERIVED);
        vm_slot_set(m, bp, pop_val(m, w), PV_DERIVED);
        return 1;
    }

    /* ---- add, sub and their flag-carrying cousins ------------------ */
    case X86_INS_ADD: case X86_INS_ADC:
    case X86_INS_SUB: case X86_INS_SBB:
    case X86_INS_CMP: {
        /* the carry has to be a real one before anything is read */
        if ((id == X86_INS_ADC || id == X86_INS_SBB) && !(m->flknown & (1u << VF_CF)))
            return 0;
        int w = opw(c, 0);
        uint64_t a = src(c, 0), b = src(c, 1) & msk(w), r;
        int sub = (id == X86_INS_SUB || id == X86_INS_SBB || id == X86_INS_CMP);
        int cin = m->fl[VF_CF];
        a &= msk(w);
        Fl f;
        if (id != X86_INS_ADC && id != X86_INS_SBB) cin = 0;
        if (sub){
            r = (a - b - (uint64_t)cin) & msk(w);
            f.cf = cin ? (a <= b) : (a < b);       /* a < b + cin, without wrapping */
            f.of = (int)(((a ^ b) & (a ^ r)) >> (w * 8 - 1)) & 1;
        } else {
            r = (a + b + (uint64_t)cin) & msk(w);
            f.cf = cin ? (r <= a) : (r < a);
            f.of = (int)((~(a ^ b) & (a ^ r)) >> (w * 8 - 1)) & 1;
        }
        f.af = (int)(((a ^ b ^ r) >> 4) & 1);
        f.zf = (r == 0); f.sf = msb(r, w); f.pf = parity(r);
        put_flags(m, ef, &f);
        if (id != X86_INS_CMP) dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_INC: case X86_INS_DEC: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w);
        uint64_t r = (id == X86_INS_INC ? a + 1 : a - 1) & msk(w);
        Fl f;
        f.cf = -1;                                  /* not touched: hardware
                                                       leaves CF exactly as
                                                       it found it        */
        f.of = (id == X86_INS_INC) ? (a == (msk(w) >> 1)) : (a == ((msk(w) >> 1) + 1));
        f.af = (int)(((a ^ 1 ^ r) >> 4) & 1);
        f.zf = (r == 0); f.sf = msb(r, w); f.pf = parity(r);
        put_flags(m, ef, &f);
        dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_NEG: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w), r = (0 - a) & msk(w);
        Fl f;
        f.cf = (a != 0);
        f.of = (a == (msk(w) >> 1) + 1);
        f.af = (int)(((a ^ r) >> 4) & 1);
        f.zf = (r == 0); f.sf = msb(r, w); f.pf = parity(r);
        put_flags(m, ef, &f);
        dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_NOT:
        dst(c, 0, (~src(c, 0)) & msk(opw(c, 0)));
        return !c->fault;

    /* ---- the bitwise three, plus the two that only set flags -------- */
    case X86_INS_AND: case X86_INS_OR: case X86_INS_XOR: case X86_INS_TEST: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w), b = src(c, 1) & msk(w), r;
        r = (id == X86_INS_OR) ? (a | b) : (id == X86_INS_XOR) ? (a ^ b) : (a & b);
        Fl f = logic_flags(r, w);
        put_flags(m, ef, &f);
        if (id != X86_INS_TEST) dst(c, 0, r);
        return !c->fault;
    }

    /* ---- shifts ----------------------------------------------------- */
    case X86_INS_SHL: case X86_INS_SAL: case X86_INS_SHR: case X86_INS_SAR: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w);
        int n = (int)(src(c, 1) & (w == 8 ? 63 : 31));   /* the width-dependent mask */
        if (n == 0){
            /* No flag changes -- but the destination is still written, and
               at width 4 that zero-extends the slot.  `shl eax, 0` really
               does clear the top half of rax; the oracle caught it.    */
            dst(c, 0, a);
            return !c->fault;
        }
        uint64_t r; int cf;
        if (id == X86_INS_SHL || id == X86_INS_SAL){
            r = (a << n) & msk(w);
            cf = n <= w * 8 ? (int)((a >> (w * 8 - n)) & 1) : 0;
        } else if (id == X86_INS_SHR){
            r = (a >> n) & msk(w);
            cf = (int)((a >> (n - 1)) & 1);
        } else {
            int64_t sa = sext(a, w);
            r = (uint64_t)(sa >> (n >= 64 ? 63 : n)) & msk(w);
            cf = (int)((uint64_t)(sa >> (n - 1 >= 64 ? 63 : n - 1)) & 1);
        }
        Fl f;
        f.cf = cf;
        f.af = -1;
        f.zf = (r == 0); f.sf = msb(r, w); f.pf = parity(r);
        /* The manual defines OF only for a count of one.  Beyond that the
           oracle says hardware is not reproducible from the inputs: over
           the corpus, no rule explains `shl` past 65 of 87 vectors, and
           `shr` at width 1 disagrees with the width-8 rule.  So OF is
           declined past a count of one -- unknown, which is what the flag
           row on the panel then shows -- rather than guessed.        */
        f.of = (n == 1) ? ((id == X86_INS_SHL || id == X86_INS_SAL) ? (msb(r, w) ^ cf)
                        : (id == X86_INS_SHR) ? msb(a, w) : 0)
                        : -1;
        put_flags(m, ef, &f);
        dst(c, 0, r);
        return !c->fault;
    }

    /* ---- multiply --------------------------------------------------- */
    case X86_INS_IMUL: {
        int w = opw(c, 0);
        if (info->nops == 1){                       /* rdx:rax = rax * src */
            int64_t a = sext(vm_reg_get(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX :
                                           w == 2 ? X86_REG_AX : X86_REG_AL), w);
            int64_t b = sext(src(c, 0), w);
            __int128 p = (__int128)a * b;
            uint64_t lo = (uint64_t)p & msk(w), hi = (uint64_t)(p >> (w * 8)) & msk(w);
            if (w == 1) vm_reg_set(m, X86_REG_AX, (uint64_t)p & 0xffff, PV_DERIVED);
            else {
                vm_reg_set(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX : X86_REG_AX, lo, PV_DERIVED);
                vm_reg_set(m, w == 8 ? X86_REG_RDX : w == 4 ? X86_REG_EDX : X86_REG_DX, hi, PV_DERIVED);
            }
            int ovf = (p != (__int128)sext(lo, w));
            Fl f = { ovf, parity(lo), -1, lo == 0, msb(lo, w), ovf };
            put_flags(m, ef, &f);
            return !c->fault;
        }
        int64_t a = sext(src(c, info->nops == 3 ? 1 : 0), w);
        int64_t b = sext(src(c, info->nops == 3 ? 2 : 1), opw(c, info->nops == 3 ? 2 : 1));
        __int128 p = (__int128)a * b;
        uint64_t r = (uint64_t)p & msk(w);
        int ovf = (p != (__int128)sext(r, w));
        Fl f = { ovf, parity(r), -1, r == 0, msb(r, w), ovf };
        put_flags(m, ef, &f);
        dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_MUL: {
        int w = opw(c, 0);
        uint64_t a = vm_reg_get(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX :
                                   w == 2 ? X86_REG_AX : X86_REG_AL) & msk(w);
        uint64_t b = src(c, 0) & msk(w);
        unsigned __int128 p = (unsigned __int128)a * b;
        uint64_t lo = (uint64_t)p & msk(w), hi = (uint64_t)(p >> (w * 8)) & msk(w);
        if (w == 1) vm_reg_set(m, X86_REG_AX, (uint64_t)p & 0xffff, PV_DERIVED);
        else {
            vm_reg_set(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX : X86_REG_AX, lo, PV_DERIVED);
            vm_reg_set(m, w == 8 ? X86_REG_RDX : w == 4 ? X86_REG_EDX : X86_REG_DX, hi, PV_DERIVED);
        }
        int ovf = (hi != 0);
        Fl f = { ovf, parity(lo), -1, lo == 0, msb(lo, w), ovf };
        put_flags(m, ef, &f);
        return !c->fault;
    }

    case X86_INS_DIV: case X86_INS_IDIV: {
        int w = opw(c, 0);
        uint64_t d = src(c, 0) & msk(w);
        if (d == 0) return 0;                    /* #DE: a fault, not a result */
        uint64_t lo = vm_reg_get(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX : X86_REG_AX);
        uint64_t hi = vm_reg_get(m, w == 8 ? X86_REG_RDX : w == 4 ? X86_REG_EDX : X86_REG_DX);
        uint64_t q, r;
        if (id == X86_INS_DIV){
            unsigned __int128 n = ((unsigned __int128)(hi & msk(w)) << (w * 8)) | (lo & msk(w));
            unsigned __int128 qq = n / d;
            if (qq > (unsigned __int128)msk(w)) return 0;      /* #DE */
            q = (uint64_t)qq; r = (uint64_t)(n % d);
        } else {
            /* the shift is done unsigned and the sign put back after:
               shifting a negative value left is undefined, and UBSan is
               right that the release build was only getting away with it */
            __int128 n = (__int128)(((unsigned __int128)(uint64_t)sext(hi, w) << (w * 8))
                                    | (lo & msk(w)));
            __int128 dd = sext(d, w);
            if (dd == 0) return 0;
            __int128 qq = n / dd;
            if (qq > (__int128)(int64_t)(msk(w) >> 1) ||
                qq < -(__int128)(int64_t)(msk(w) >> 1) - 1) return 0;
            q = (uint64_t)qq & msk(w); r = (uint64_t)(n % dd) & msk(w);
        }
        vm_reg_set(m, w == 8 ? X86_REG_RAX : w == 4 ? X86_REG_EAX : X86_REG_AX, q, PV_DERIVED);
        vm_reg_set(m, w == 8 ? X86_REG_RDX : w == 4 ? X86_REG_EDX : X86_REG_DX, r, PV_DERIVED);
        Fl f = { -1, -1, -1, -1, -1, -1 };
        put_flags(m, ef, &f);
        return !c->fault;
    }

    /* ---- what the CPU says it is (§4.2) ---------------------------- */
    case X86_INS_CPUID: {
        uint32_t o[4];
        vm_cpuid((uint32_t)vm_reg_get(m, X86_REG_EAX),
                 (uint32_t)vm_reg_get(m, X86_REG_ECX), o);
        vm_reg_set(m, X86_REG_EAX, o[0], PV_DERIVED);
        vm_reg_set(m, X86_REG_EBX, o[1], PV_DERIVED);
        vm_reg_set(m, X86_REG_ECX, o[2], PV_DERIVED);
        vm_reg_set(m, X86_REG_EDX, o[3], PV_DERIVED);
        return 1;
    }
    case X86_INS_XGETBV: {
        uint64_t x = vm_xcr0();
        vm_reg_set(m, X86_REG_EAX, x & 0xffffffffu, PV_DERIVED);
        vm_reg_set(m, X86_REG_EDX, (x >> 32) & 0xffffffffu, PV_DERIVED);
        return 1;
    }

    /* ---- rotates and bit tests --------------------------------------
       Not in §3's candidate list of seventy, and between them the whole of
       what still havocked in `git` once the tiers landed.            */
    case X86_INS_ROL: case X86_INS_ROR: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w);
        int bits = w * 8;
        int nrot = (int)(src(c, 1) & (w == 8 ? 63 : 31)) % bits;
        if (nrot == 0){
            /* No flags, no rotation -- but the destination is written all
               the same, so `ror eax, 0` zero-extends rax.  Exactly the
               trap `shl eax, 0` set, caught by the same corpus.       */
            dst(c, 0, a);
            return !c->fault;
        }
        uint64_t r = (id == X86_INS_ROL)
                   ? ((a << nrot) | (a >> (bits - nrot))) & msk(w)
                   : ((a >> nrot) | (a << (bits - nrot))) & msk(w);
        Fl f = { -1, -1, -1, -1, -1, -1 };
        f.cf = (id == X86_INS_ROL) ? (int)(r & 1) : msb(r, w);
        f.of = (nrot == 1) ? (msb(r, w) ^ f.cf) : -1;
        put_flags(m, ef, &f);
        dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_BSWAP: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w), r = 0;
        for (int i = 0; i < w; i++) r = (r << 8) | ((a >> (i * 8)) & 0xff);
        dst(c, 0, r);
        return !c->fault;
    }

    case X86_INS_BT: case X86_INS_BTS: case X86_INS_BTR: case X86_INS_BTC: {
        int w = opw(c, 0);
        uint64_t a = src(c, 0) & msk(w);
        int bit = (int)(src(c, 1) & (uint64_t)(w * 8 - 1));
        Fl f = { (int)((a >> bit) & 1), -1, -1, -1, -1, -1 };
        put_flags(m, ef, &f);
        if (id != X86_INS_BT){
            uint64_t b1 = 1ull << bit;
            uint64_t r = (id == X86_INS_BTS) ? (a | b1)
                       : (id == X86_INS_BTR) ? (a & ~b1) : (a ^ b1);
            dst(c, 0, r & msk(w));
        }
        return !c->fault;
    }

    /* §4.5: a `rep` string operation with an invented rcx could be 2^63
       iterations.  Do it as one bounded bulk operation and say so. */
    case X86_INS_STOSB: case X86_INS_STOSW: case X86_INS_STOSD: case X86_INS_STOSQ:
    case X86_INS_MOVSB: case X86_INS_MOVSW: case X86_INS_MOVSD: case X86_INS_MOVSQ: {
        if (info->nops < 1 || info->op[0].kind != OPK_MEM) return 0;  /* movsd xmm */
        int w = opw(c, 0);
        int store = (id == X86_INS_STOSB || id == X86_INS_STOSW ||
                     id == X86_INS_STOSD || id == X86_INS_STOSQ);
        int di = vm_slot_named(m, "rdi"), si = vm_slot_named(m, "rsi");
        int cx = vm_slot_named(m, "rcx"), ax = vm_slot_named(m, "rax");
        uint64_t n2 = vm_slot_get(m, cx);
        if (n2 > VM_REPMAX) n2 = VM_REPMAX;          /* bounded, and labelled */
        uint64_t d2 = vm_slot_get(m, di), s2 = vm_slot_get(m, si);
        for (uint64_t k = 0; k < n2; k++){
            if (store) vm_write(m, d2, w, vm_slot_get(m, ax));
            else { Prov p; vm_write(m, d2, w, vm_read(m, s2, w, &p)); s2 += (uint64_t)w; }
            d2 += (uint64_t)w;
        }
        vm_slot_set(m, di, d2, PV_DERIVED);
        if (!store) vm_slot_set(m, si, s2, PV_DERIVED);
        vm_slot_set(m, cx, vm_slot_get(m, cx) - n2, PV_DERIVED);
        return 1;
    }

    /* ---- bit scan: the destination survives a zero source ---------- */
    case X86_INS_BSF: case X86_INS_BSR: {
        int w = opw(c, 0);
        uint64_t v = src(c, 1) & msk(w);
        Fl f = { -1, -1, -1, v == 0, -1, -1 };
        put_flags(m, ef, &f);
        if (v == 0) return !c->fault;           /* destination left alone */
        int n = 0;
        if (id == X86_INS_BSF){ while (!((v >> n) & 1)) n++; }
        else { n = w * 8 - 1; while (!((v >> n) & 1)) n--; }
        dst(c, 0, (uint64_t)n);
        return !c->fault;
    }

    /* ---- control flow: the structure is vm_step's, the condition ours */
    case X86_INS_JMP: case X86_INS_CALL:
        /* An indirect branch -- `jmp [rip+x]` through the GOT, `call rax`,
           `jmp [rax*8+table]` -- has no target in the decoding, but with
           real semantics and §9's relocations in the shadow we can just
           work it out.  That is what turns a PLT thunk from a dead end
           into a door with a name on it.                             */
        if (!in->taddr && info->nops == 1){
            const InsnOp *o = &info->op[0];
            if (o->kind == OPK_REG){ *ret = vm_reg_get(m, o->reg); return 4; }
            if (o->kind == OPK_MEM){
                Prov p;
                *ret = vm_read(m, ea_of(c, o), o->size ? o->size : 8, &p);
                return 4;
            }
        }
        return 1;

    case X86_INS_RET: {
        /* A real pop, against the frame vm_init set up.  The sentinel says
           the function has returned to whoever called it and the run is
           over; anything else is a return into this same room.       */
        int w = sp_width(m);
        uint64_t sp = vm_slot_get(m, m->spSlot);
        Prov p;
        uint64_t ra = vm_read(m, sp, w, &p);
        sp += (uint64_t)w;
        if (info->nops == 1 && info->op[0].kind == OPK_IMM)   /* `ret imm16` */
            sp += (uint64_t)info->op[0].imm;
        vm_slot_set(m, m->spSlot, sp, PV_DERIVED);
        *ret = ra;
        return 3;
    }

    case X86_INS_JO:  case X86_INS_JNO: case X86_INS_JB:  case X86_INS_JAE:
    case X86_INS_JE:  case X86_INS_JNE: case X86_INS_JBE: case X86_INS_JA:
    case X86_INS_JS:  case X86_INS_JNS: case X86_INS_JP:  case X86_INS_JNP:
    case X86_INS_JL:  case X86_INS_JGE: case X86_INS_JLE: case X86_INS_JG:
    case X86_INS_JCXZ: case X86_INS_JECXZ: case X86_INS_JRCXZ:
        *cond = cond_of(m, id, ef);
        return *cond >= 0 ? 2 : 0;

    default:
        return 0;                                /* tier 0 takes it */
    }
}
