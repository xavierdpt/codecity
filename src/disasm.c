/* disasm.c -- capstone wrapper: a room's bytes decoded into instructions
 *
 * One handle is kept open for the architecture of the file being explored.
 * Rooms are decoded on entry and thrown away on the way out, so at most one
 * room's worth of instructions is ever allocated -- a chamber counting as
 * one room, however many alcoves it holds.
 */
#define _GNU_SOURCE
#include "disasm.h"
#include <capstone/capstone.h>
#include <capstone/x86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static csh  g_h;
static int  g_open;
static char g_arch[32];
static int  g_machine;

int disasm_ready(void){ return g_open; }
const char *disasm_arch_name(void){ return g_open ? g_arch : "unsupported"; }

void disasm_close(void){
    if (g_open){ cs_close(&g_h); g_open = 0; }
    g_arch[0] = 0;
}

int disasm_open(int machine, int is64, int be){
    disasm_close();
    cs_arch arch; cs_mode mode;
    switch (machine){
    case 3:   arch = CS_ARCH_X86;   mode = CS_MODE_32; snprintf(g_arch, sizeof g_arch, "x86"); break;
    case 62:  arch = CS_ARCH_X86;   mode = CS_MODE_64; snprintf(g_arch, sizeof g_arch, "x86-64"); break;
    case 40:  arch = CS_ARCH_ARM;   mode = CS_MODE_ARM; snprintf(g_arch, sizeof g_arch, "ARM"); break;
    case 183: arch = CS_ARCH_ARM64; mode = CS_MODE_ARM; snprintf(g_arch, sizeof g_arch, "AArch64"); break;
    case 8:   arch = CS_ARCH_MIPS;  mode = is64 ? CS_MODE_MIPS64 : CS_MODE_MIPS32;
              snprintf(g_arch, sizeof g_arch, "MIPS"); break;
    case 20: case 21:
              arch = CS_ARCH_PPC;   mode = is64 ? CS_MODE_64 : CS_MODE_32;
              snprintf(g_arch, sizeof g_arch, "PowerPC"); break;
    case 2:   arch = CS_ARCH_SPARC; mode = CS_MODE_BIG_ENDIAN; snprintf(g_arch, sizeof g_arch, "SPARC"); break;
    case 22:  arch = CS_ARCH_SYSZ;  mode = CS_MODE_BIG_ENDIAN; snprintf(g_arch, sizeof g_arch, "S390"); break;
    default:  return 0;
    }
    if (be && arch != CS_ARCH_SPARC && arch != CS_ARCH_SYSZ)
        mode = (cs_mode)(mode | CS_MODE_BIG_ENDIAN);
    else if (!be && arch != CS_ARCH_SPARC && arch != CS_ARCH_SYSZ)
        mode = (cs_mode)(mode | CS_MODE_LITTLE_ENDIAN);
    if (cs_open(arch, mode, &g_h) != CS_ERR_OK) return 0;
    cs_option(g_h, CS_OPT_DETAIL, CS_OPT_ON);
    g_open = 1;
    g_machine = machine;
    return 1;
}

static void cpy(char *d, size_t n, const char *s){
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l);
    d[l] = 0;
}

int disasm_machine(void){ return g_open ? g_machine : 0; }

const char *disasm_reg_name(unsigned id){
    return g_open ? cs_reg_name(g_h, id) : NULL;
}

/* Re-decode one instruction and ask what it reads, writes and disturbs.
   cs_regs_access() covers the operands as well as the implicit registers on
   the architectures that implement it; where it does not, the instruction's
   own implicit lists are all there is, and `full` says so.              */
int disasm_info(const uint8_t *bytes, unsigned len, uint64_t addr, InsnInfo *out){
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!g_open || !bytes || !len) return 0;
    cs_insn *in = NULL;
    if (cs_disasm(g_h, bytes, len, addr, 1, &in) != 1){ if (in) cs_free(in, 0); return 0; }
    out->id = in->id;
    out->len = (uint8_t)in->size;
    cpy(out->mnem, sizeof out->mnem, in->mnemonic);

    cs_regs rd, wr; uint8_t nrd = 0, nwr = 0;
    if (in->detail && cs_regs_access(g_h, in, rd, &nrd, wr, &nwr) == CS_ERR_OK){
        out->full = 1;
    } else if (in->detail){
        nrd = in->detail->regs_read_count;
        nwr = in->detail->regs_write_count;
        for (int i = 0; i < nrd && i < 64; i++) rd[i] = in->detail->regs_read[i];
        for (int i = 0; i < nwr && i < 64; i++) wr[i] = in->detail->regs_write[i];
    }
    for (int i = 0; i < nrd && out->nrd < IA_MAXREG; i++) out->rd[out->nrd++] = rd[i];
    for (int i = 0; i < nwr && out->nwr < IA_MAXREG; i++) out->wr[out->nwr++] = wr[i];
    /* the operands, which is what tier 1 actually interprets */
    if (in->detail && (g_machine == 3 || g_machine == 62)){
        out->eflags = in->detail->x86.eflags;
        const cs_x86 *x = &in->detail->x86;
        int n = x->op_count < IA_MAXOP ? x->op_count : IA_MAXOP;
        out->nops = (uint8_t)n;
        for (int i = 0; i < n; i++){
            const cs_x86_op *o = &x->operands[i];
            InsnOp *d = &out->op[i];
            d->size = o->size;
            d->rd = (o->access & CS_AC_READ)  ? 1 : 0;
            d->wr = (o->access & CS_AC_WRITE) ? 1 : 0;
            switch (o->type){
            case X86_OP_REG: d->kind = OPK_REG; d->reg = (uint16_t)o->reg; break;
            case X86_OP_IMM: d->kind = OPK_IMM; d->imm = o->imm; break;
            case X86_OP_MEM:
                d->kind  = OPK_MEM;
                d->base  = (uint16_t)o->mem.base;
                d->index = (uint16_t)o->mem.index;
                d->seg   = (uint16_t)o->mem.segment;
                d->scale = o->mem.scale;
                d->disp  = o->mem.disp;
                break;
            default: d->kind = OPK_NONE; break;
            }
        }
    }
    cs_free(in, 1);
    return 1;
}

int disasm_riprel(const uint8_t *code, uint64_t len, uint64_t vaddr,
                  void (*hit)(uint64_t addr, int isLea, void *u), void *u){
    if (!g_open || !code || !len || !hit) return 0;
    if (g_machine != 62) return 0;              /* rip-relative is x86-64's */
    int n = 0;
    uint64_t at = 0;
    /* decoded in chunks: cs_disasm on a whole .text at once would allocate
       a few hundred megabytes of cs_insn for a big binary */
    while (at < len){
        uint64_t take = len - at;
        if (take > (1u << 20)) take = 1u << 20;
        cs_insn *ins = NULL;
        size_t got = cs_disasm(g_h, code + at, (size_t)take, vaddr + at, 0, &ins);
        if (!got){ at++; continue; }            /* not an instruction: skip a byte */
        for (size_t i = 0; i < got; i++){
            if (!ins[i].detail) continue;
            const cs_x86 *x = &ins[i].detail->x86;
            for (int k = 0; k < x->op_count; k++){
                const cs_x86_op *o = &x->operands[k];
                if (o->type != X86_OP_MEM || o->mem.base != X86_REG_RIP) continue;
                hit(ins[i].address + ins[i].size + (uint64_t)o->mem.disp,
                    ins[i].id == X86_INS_LEA, u);
                n++;
            }
        }
        uint64_t adv = ins[got-1].address + ins[got-1].size - (vaddr + at);
        cs_free(ins, got);
        at += adv ? adv : 1;
    }
    return n;
}

const char *disasm_class_name(int c){
    static const char *n[IC_COUNT] = {
        "other", "move", "stack", "arith", "logic", "compare",
        "jump", "branch", "call", "return", "vector", "padding", "system" };
    return (c >= 0 && c < IC_COUNT) ? n[c] : "?";
}

/* ---- classification ----------------------------------------------- */

static int has_pre(const char *m, const char *const *tab){
    for (int i = 0; tab[i]; i++){
        size_t l = strlen(tab[i]);
        if (!strncmp(m, tab[i], l)) return 1;
    }
    return 0;
}

static int classify(cs_insn *in){
    const char *m = in->mnemonic, *o = in->op_str;

    if (cs_insn_group(g_h, in, CS_GRP_RET))  return IC_RET;
    if (cs_insn_group(g_h, in, CS_GRP_CALL)) return IC_CALL;
    if (cs_insn_group(g_h, in, CS_GRP_JUMP)){
        if (!strcmp(m, "jmp") || !strcmp(m, "b") || !strcmp(m, "br") ||
            !strcmp(m, "bx") || !strcmp(m, "j")) return IC_JUMP;
        return IC_CJUMP;
    }
    if (cs_insn_group(g_h, in, CS_GRP_INT) || cs_insn_group(g_h, in, CS_GRP_IRET) ||
        !strcmp(m, "syscall") || !strcmp(m, "sysenter") || !strcmp(m, "svc") ||
        !strcmp(m, "hvc") || !strcmp(m, "smc")) return IC_SYSCALL;

    if (strstr(o, "xmm") || strstr(o, "ymm") || strstr(o, "zmm") ||
        (o[0] == 'v' && o[1] >= '0' && o[1] <= '9') || strstr(o, ".16b") || strstr(o, ".4s"))
        return IC_VECTOR;

    static const char *nop[]   = { "nop", "ud", "int3", "hlt", "brk", NULL };
    static const char *move[]  = { "mov", "lea", "ld", "st", "xchg", "cmov", "movz", "movs",
                                   "adr", "cset", "csel", "swp", NULL };
    static const char *stack[] = { "push", "pop", "enter", "leave", NULL };
    static const char *arith[] = { "add", "sub", "mul", "div", "inc", "dec", "neg", "adc",
                                   "sbb", "shl", "shr", "sar", "rol", "ror", "lsl", "lsr",
                                   "asr", "madd", "msub", "sdiv", "udiv", NULL };
    static const char *logic[] = { "and", "or", "xor", "not", "bt", "bs", "bic", "eor",
                                   "orr", "mvn", "clz", "rev", NULL };
    static const char *cmp[]   = { "cmp", "test", "ucomi", "comi", "tst", "ccmp", NULL };

    if (has_pre(m, cmp))   return IC_CMP;
    if (has_pre(m, nop))   return IC_NOP;
    if (has_pre(m, stack)) return IC_STACK;
    if (has_pre(m, move))  return IC_MOVE;
    if (has_pre(m, arith)) return IC_ARITH;
    if (has_pre(m, logic)) return IC_LOGIC;
    if (!strncmp(m, "endbr", 5) || !strncmp(m, "bti", 3) || !strncmp(m, "cfi", 3)) return IC_NOP;
    if (m[0] == 'v' || m[0] == 'p') return IC_VECTOR;
    return IC_OTHER;
}

/* ---- run ----------------------------------------------------------- */

int g_disasm_live;      /* live decodings -- one room's worth at a time */

Disasm *disasm_run(const uint8_t *code, uint64_t len, uint64_t vaddr, int maxins){
    if (!g_open || !code || !len) return NULL;
    if (maxins < 0) maxins = 0;          /* 0 = decode the whole room */
    cs_insn *ins = NULL;
    size_t got = cs_disasm(g_h, code, (size_t)len, vaddr, (size_t)maxins, &ins);
    if (!got){ if (ins) cs_free(ins, 0); return NULL; }

    Disasm *d = calloc(1, sizeof(Disasm));
    d->n = (int)got;
    d->ins = calloc(got, sizeof(Insn));
    for (size_t i = 0; i < got; i++){
        Insn *t = &d->ins[i];
        t->addr = ins[i].address;
        t->len  = (uint16_t)ins[i].size;
        t->cls  = (uint8_t)classify(&ins[i]);
        t->target = -1; t->tunit = -1; t->port = -1;
        cpy(t->mnem, sizeof t->mnem, ins[i].mnemonic);
        cpy(t->ops,  sizeof t->ops,  ins[i].op_str);
        /* A direct branch prints its destination as a bare address -- with a
           leading '#' on ARM and AArch64, where the immediate syntax keeps
           it.  Without the '#' every ARM branch looked indirect and the
           walker of vm.c stopped dead at the first one.               */
        if (t->cls == IC_JUMP || t->cls == IC_CJUMP || t->cls == IC_CALL){
            const char *o = ins[i].op_str;
            if (*o == '#') o++;
            if (o[0] == '0' && o[1] == 'x') t->taddr = strtoull(o, NULL, 16);
        }
        d->covered += t->len;
    }
    cs_free(ins, got);

    /* resolve the branches that land inside this room */
    for (int i = 0; i < d->n; i++){
        if (!d->ins[i].taddr) continue;
        int at = disasm_index_of(d, d->ins[i].taddr);
        if (at >= 0){ d->ins[i].target = at; d->nlinks++; }
        else d->nexits++;
    }
    d->truncated = (d->covered < len);
    g_disasm_live++;
    return d;
}

int disasm_index_of(const Disasm *d, uint64_t addr){
    if (!d || !d->n) return -1;
    int lo = 0, hi = d->n - 1;
    while (lo <= hi){
        int mid = (lo + hi) / 2;
        if (d->ins[mid].addr == addr) return mid;
        if (d->ins[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int cmp_u64(const void *a, const void *b){
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* One port per distinct destination, in address order so the wall reads
   the same way twice.  Anything still unresolved after the cross-alcove
   pass leaves the room, so it needs a way out.                          */
void disasm_ports(Disasm *d){
    if (!d) return;
    free(d->ports); d->ports = NULL; d->nports = 0;
    int nex = 0;
    for (int i = 0; i < d->n; i++){
        d->ins[i].port = -1;
        if (d->ins[i].target < 0 && d->ins[i].taddr) nex++;
    }
    if (!nex) return;

    uint64_t *a = malloc((size_t)nex * sizeof *a);
    int na = 0;
    for (int i = 0; i < d->n; i++)
        if (d->ins[i].target < 0 && d->ins[i].taddr) a[na++] = d->ins[i].taddr;
    qsort(a, (size_t)na, sizeof *a, cmp_u64);
    int m = 0;
    for (int i = 0; i < na; i++) if (!m || a[i] != a[m-1]) a[m++] = a[i];

    d->ports = calloc((size_t)m, sizeof(Port));
    d->nports = m;
    for (int i = 0; i < m; i++){ d->ports[i].addr = a[i]; d->ports[i].cls = IC_JUMP; }
    free(a);

    for (int i = 0; i < d->n; i++){
        Insn *t = &d->ins[i];
        if (t->target >= 0 || !t->taddr) continue;
        int lo = 0, hi = d->nports - 1, at = -1;
        while (lo <= hi){
            int mid = (lo + hi) / 2;
            if (d->ports[mid].addr == t->taddr){ at = mid; break; }
            if (d->ports[mid].addr < t->taddr) lo = mid + 1; else hi = mid - 1;
        }
        if (at < 0) continue;
        t->port = at;
        if (!d->ports[at].n) d->ports[at].cls = t->cls;
        d->ports[at].n++;
    }
}

void disasm_free(Disasm *d){
    if (!d) return;
    free(d->ports);
    free(d->ins);
    free(d);
    g_disasm_live--;
}

int disasm_live(void){ return g_disasm_live; }
