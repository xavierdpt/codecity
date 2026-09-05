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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static csh  g_h;
static int  g_open;
static char g_arch[32];

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
    return 1;
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

static void cpy(char *d, size_t n, const char *s){
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l);
    d[l] = 0;
}

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
        /* a direct branch prints its destination as a bare address */
        if ((t->cls == IC_JUMP || t->cls == IC_CJUMP || t->cls == IC_CALL) &&
            ins[i].op_str[0] == '0' && ins[i].op_str[1] == 'x')
            t->taddr = strtoull(ins[i].op_str, NULL, 16);
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
