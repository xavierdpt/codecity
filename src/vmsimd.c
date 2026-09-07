/* vmsimd.c -- tiers 2 and 3: scalar float, and the packed operations.
 *
 * §4.3.1 is the reason this file has the shape it does.  Between 74% and
 * 96% of the AVX in a real codec is a tier-3 operation *at another width* --
 * `vpaddw` where we already have `paddw`, sixteen lanes instead of eight,
 * three operands instead of two.  So every packed operation here is written
 * as a loop over `(element width, lane count, operand count)`, taken from
 * what Capstone says the operands are, and never as a 128-bit two-operand
 * function.  Widening to AVX2 is then a change to a loop bound.
 *
 * The tiers, as §4.3 measures them:
 *
 *   2   scalar SSE float          movss mulsd cvtsi2sd comiss
 *   3a  packed moves              movdqa movups movd pmovmskb
 *   3b  packed shuffle/compare    pcmpeqb pshufd punpcklbw pxor psrld
 *   3c  packed integer arithmetic paddw psubusb pmaddwd pavgb psadbw
 *
 * Everything here was checked against hardware the same way tier 1 was:
 * tools/oracle.py steps these forms on a real CPU with real xmm state and
 * tests/vm-corpus.txt records what it did.
 */
#define _GNU_SOURCE
#include "vm.h"
#include "disasm.h"
#include <capstone/capstone.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- operands ------------------------------------------------------- */

typedef struct {
    Vm             *m;
    const Insn     *in;
    const InsnInfo *info;
    int             fault;
    int             vex;        /* a VEX form zeroes above the destination */
} Sx;

static uint64_t ea_of(Sx *c, const InsnOp *o){
    Vm *m = c->m;
    uint64_t a = 0;
    if (o->base){
        const char *nm = disasm_reg_name(o->base);
        if (nm && (!strcmp(nm, "rip") || !strcmp(nm, "eip")))
            a += c->in->addr + c->in->len;
        else a += vm_reg_get(m, o->base);
    }
    if (o->index) a += vm_reg_get(m, o->index) * (uint64_t)(o->scale ? o->scale : 1);
    a += (uint64_t)o->disp;
    if (o->seg){
        const char *nm = disasm_reg_name(o->seg);
        if (nm && (!strcmp(nm, "fs") || !strcmp(nm, "gs"))) a += m->fsBase;
    }
    return a;
}

/* how wide an operand is, in bytes */
static int opw(const InsnInfo *in, int i){
    int w = in->op[i].size;
    return w ? w : 8;
}

/* read a vector-sized operand: a vector register, a general register, or
   memory.  `bytes` is what the instruction wants, not what the operand is. */
static void vsrc(Sx *c, int i, uint8_t *out, int bytes){
    const InsnOp *o = &c->info->op[i];
    memset(out, 0, (size_t)bytes);
    if (o->kind == OPK_REG){
        int vs = vm_vslot(c->m, o->reg);
        if (vs >= 0){ vm_vget(c->m, vs, bytes, out); return; }
        uint64_t v = vm_reg_get(c->m, o->reg);          /* movd eax -> xmm */
        int n = vm_reg_width(c->m, o->reg);
        for (int k = 0; k < n && k < bytes; k++) out[k] = (uint8_t)(v >> (k * 8));
        return;
    }
    if (o->kind == OPK_MEM){
        uint64_t a = ea_of(c, o);
        for (int k = 0; k < bytes; k += 8){
            Prov p;
            int n = bytes - k < 8 ? bytes - k : 8;
            uint64_t v = vm_read(c->m, a + (uint64_t)k, n, &p);
            for (int j = 0; j < n; j++) out[k + j] = (uint8_t)(v >> (j * 8));
        }
        return;
    }
    if (o->kind == OPK_IMM){
        uint64_t v = (uint64_t)o->imm;
        for (int k = 0; k < 8 && k < bytes; k++) out[k] = (uint8_t)(v >> (k * 8));
        return;
    }
    c->fault = 1;
}

static void vdst(Sx *c, int i, const uint8_t *in, int bytes, int prov){
    const InsnOp *o = &c->info->op[i];
    if (o->kind == OPK_REG){
        int vs = vm_vslot(c->m, o->reg);
        if (vs >= 0){
            if (c->vex) vm_vsetz(c->m, vs, bytes, in, prov);
            else        vm_vset(c->m, vs, bytes, in, prov);
            c->m->vecSlot = vs;                  /* the one the panel shows */
            c->m->vecBytes = (uint8_t)bytes;
            if (!c->m->vecEw) c->m->vecEw = (uint8_t)bytes;
            return;
        }
        uint64_t v = 0;
        int n = vm_reg_width(c->m, o->reg);
        for (int k = n - 1; k >= 0; k--) v = (v << 8) | in[k];
        vm_reg_set(c->m, o->reg, v, prov);
        return;
    }
    if (o->kind == OPK_MEM){
        uint64_t a = ea_of(c, o);
        for (int k = 0; k < bytes; k += 8){
            int n = bytes - k < 8 ? bytes - k : 8;
            uint64_t v = 0;
            for (int j = n - 1; j >= 0; j--) v = (v << 8) | in[k + j];
            vm_write(c->m, a + (uint64_t)k, n, v);
        }
        return;
    }
    c->fault = 1;
}

/* ---- lanes ---------------------------------------------------------- */

static uint64_t lget(const uint8_t *p, int off, int ew){
    uint64_t v = 0;
    for (int i = ew - 1; i >= 0; i--) v = (v << 8) | p[off + i];
    return v;
}
static void lput(uint8_t *p, int off, int ew, uint64_t v){
    for (int i = 0; i < ew; i++) p[off + i] = (uint8_t)(v >> (i * 8));
}
static int64_t lsext(uint64_t v, int ew){
    if (ew >= 8) return (int64_t)v;
    uint64_t sb = 1ull << (ew * 8 - 1);
    return (int64_t)((v ^ sb) - sb);
}
static uint64_t lmask(int ew){ return ew >= 8 ? ~0ull : ((1ull << (ew * 8)) - 1); }

static int64_t clampi(int64_t v, int64_t lo, int64_t hi){
    return v < lo ? lo : (v > hi ? hi : v);
}

/* every packed operation this file models, as one lane-wise function.
   `ew` is the element width in bytes -- the parameter §4.3.1 insists on. */
enum {
    VO_ADD, VO_SUB, VO_ADDS, VO_SUBS, VO_ADDUS, VO_SUBUS,
    VO_AND, VO_ANDN, VO_OR, VO_XOR,
    VO_CMPEQ, VO_CMPGT, VO_MINS, VO_MAXS, VO_MINU, VO_MAXU,
    VO_AVG, VO_MULLW, VO_MULHW, VO_MULHUW, VO_MULLD,
    VO_SLL, VO_SRL, VO_SRA
};

static uint64_t lane_op(int op, uint64_t a, uint64_t b, int ew){
    uint64_t mk = lmask(ew);
    int64_t sa = lsext(a, ew), sb = lsext(b, ew);
    int64_t lo = -(int64_t)(mk >> 1) - 1, hi = (int64_t)(mk >> 1);
    switch (op){
    case VO_ADD:    return (a + b) & mk;
    case VO_SUB:    return (a - b) & mk;
    case VO_ADDS:   return (uint64_t)clampi(sa + sb, lo, hi) & mk;
    case VO_SUBS:   return (uint64_t)clampi(sa - sb, lo, hi) & mk;
    case VO_ADDUS:  return (a + b) > mk ? mk : (a + b);
    case VO_SUBUS:  return a > b ? a - b : 0;
    case VO_AND:    return a & b;
    case VO_ANDN:   return (~a) & b & mk;
    case VO_OR:     return a | b;
    case VO_XOR:    return a ^ b;
    case VO_CMPEQ:  return a == b ? mk : 0;
    case VO_CMPGT:  return sa > sb ? mk : 0;
    case VO_MINS:   return (uint64_t)(sa < sb ? sa : sb) & mk;
    case VO_MAXS:   return (uint64_t)(sa > sb ? sa : sb) & mk;
    case VO_MINU:   return a < b ? a : b;
    case VO_MAXU:   return a > b ? a : b;
    case VO_AVG:    return (a + b + 1) >> 1;
    case VO_MULLW:  return (uint64_t)(sa * sb) & mk;
    case VO_MULLD:  return (uint64_t)(sa * sb) & mk;
    case VO_MULHW:  return (uint64_t)((sa * sb) >> (ew * 8)) & mk;
    case VO_MULHUW: return ((a * b) >> (ew * 8)) & mk;
    case VO_SLL:    return b >= (uint64_t)ew * 8 ? 0 : (a << b) & mk;
    case VO_SRL:    return b >= (uint64_t)ew * 8 ? 0 : (a >> b) & mk;
    case VO_SRA: {
        uint64_t n = b >= (uint64_t)ew * 8 ? (uint64_t)ew * 8 - 1 : b;
        return (uint64_t)(sa >> n) & mk;
    }
    }
    return 0;
}

/* the whole of tier 3, in one loop.  dst = op(src1, src2), lane by lane,
   for however many lanes `bytes / ew` comes to.                       */
static void packed(Sx *c, int op, int ew, int bytes, int di, int s1, int s2){
    uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
    c->m->vecEw = (uint8_t)ew;
    vsrc(c, s1, a, bytes);
    vsrc(c, s2, b, bytes);
    for (int off = 0; off + ew <= bytes; off += ew)
        lput(r, off, ew, lane_op(op, lget(a, off, ew), lget(b, off, ew), ew));
    vdst(c, di, r, bytes, PV_DERIVED);
}

/* a shift by a count in the low 64 bits of the second operand, or by an
   immediate: the count is one value, not one per lane */
static void packed_shift(Sx *c, int op, int ew, int bytes, int di, int s1, int s2){
    uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
    c->m->vecEw = (uint8_t)ew;
    vsrc(c, s1, a, bytes);
    vsrc(c, s2, b, VM_VBYTES < 16 ? VM_VBYTES : 16);
    uint64_t n = lget(b, 0, 8);
    if (c->info->op[s2].kind == OPK_IMM) n = (uint64_t)c->info->op[s2].imm & 0xff;
    for (int off = 0; off + ew <= bytes; off += ew)
        lput(r, off, ew, lane_op(op, lget(a, off, ew), n, ew));
    vdst(c, di, r, bytes, PV_DERIVED);
}

/* ---- tier 2: scalar float ------------------------------------------- */

static double f_get(const uint8_t *p, int single){
    if (single){ float f; memcpy(&f, p, 4); return (double)f; }
    double d; memcpy(&d, p, 8); return d;
}
static void f_put(uint8_t *p, int single, double v){
    if (single){ float f = (float)v; memcpy(p, &f, 4); }
    else memcpy(p, &v, 8);
}

/* An arithmetic operation on a NaN returns *that NaN's bits*, quieted --
   the first source operand's if it is one, else the second's.  Computing
   it through a double and converting back invents a different NaN, which
   is a wrong answer that only a corpus recorded bit for bit can see. */
static int f_isnan(const uint8_t *p, int single){
    if (single){ uint32_t u; memcpy(&u, p, 4);
                 return (u & 0x7f800000u) == 0x7f800000u && (u & 0x007fffffu); }
    uint64_t u; memcpy(&u, p, 8);
    return (u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
           (u & 0x000fffffffffffffull);
}
static int f_nan_result(const uint8_t *a, const uint8_t *b, int single, uint8_t *out){
    const uint8_t *src = f_isnan(a, single) ? a : (f_isnan(b, single) ? b : NULL);
    if (!src) return 0;
    if (single){ uint32_t u; memcpy(&u, src, 4); u |= 0x00400000u; memcpy(out, &u, 4); }
    else { uint64_t u; memcpy(&u, src, 8); u |= 0x0008000000000000ull; memcpy(out, &u, 8); }
    return 1;
}

/* ---- the dispatcher -------------------------------------------------- */

int vmsimd_exec(Vm *m, const Insn *in, const InsnInfo *info, int *tier){
    Sx cx = { m, in, info, 0, info->mnem[0] == 'v' };
    Sx *c = &cx;
    unsigned id = info->id;
    int n = info->nops;
    *tier = 3;
    m->vecSlot = -1;
    m->vecEw = 0;

    /* the destination's width is what decides the lane count: 16 for an
       xmm form, 32 for the ymm one, and the same code runs both */
    int bytes = n ? opw(info, 0) : 16;
    if (bytes > VM_VBYTES) bytes = VM_VBYTES;
    /* §4.3.1's promise: AVX2 is this same code at another lane count, and
       the loops below simply run at 32 bytes when told to.  The corpus is
       recorded 256 bits wide and steps the ymm forms on hardware, so this
       is checked rather than assumed.                                */

    /* a three-operand VEX form writes op0 from op1 and op2; the two-operand
       SSE form writes op0 from op0 and op1 */
    int d = 0, s1 = (n >= 3) ? 1 : 0, s2 = (n >= 3) ? 2 : 1;
    if (n < 2) s1 = s2 = 0;

#define P(OP, EW) do { packed(c, OP, EW, bytes, d, s1, s2); return !c->fault; } while (0)
#define SH(OP, EW) do { packed_shift(c, OP, EW, bytes, d, s1, s2); return !c->fault; } while (0)

    switch (id){

    /* ---- 3c: packed integer arithmetic ---------------------------- */
    case X86_INS_PADDB: case X86_INS_VPADDB: P(VO_ADD, 1);
    case X86_INS_PADDW: case X86_INS_VPADDW: P(VO_ADD, 2);
    case X86_INS_PADDD: case X86_INS_VPADDD: P(VO_ADD, 4);
    case X86_INS_PADDQ: case X86_INS_VPADDQ: P(VO_ADD, 8);
    case X86_INS_PSUBB: case X86_INS_VPSUBB: P(VO_SUB, 1);
    case X86_INS_PSUBW: case X86_INS_VPSUBW: P(VO_SUB, 2);
    case X86_INS_PSUBD: case X86_INS_VPSUBD: P(VO_SUB, 4);
    case X86_INS_PSUBQ: case X86_INS_VPSUBQ: P(VO_SUB, 8);
    case X86_INS_PADDSB: case X86_INS_VPADDSB: P(VO_ADDS, 1);
    case X86_INS_PADDSW: case X86_INS_VPADDSW: P(VO_ADDS, 2);
    case X86_INS_PSUBSB: case X86_INS_VPSUBSB: P(VO_SUBS, 1);
    case X86_INS_PSUBSW: case X86_INS_VPSUBSW: P(VO_SUBS, 2);
    case X86_INS_PADDUSB: case X86_INS_VPADDUSB: P(VO_ADDUS, 1);
    case X86_INS_PADDUSW: case X86_INS_VPADDUSW: P(VO_ADDUS, 2);
    case X86_INS_PSUBUSB: case X86_INS_VPSUBUSB: P(VO_SUBUS, 1);
    case X86_INS_PSUBUSW: case X86_INS_VPSUBUSW: P(VO_SUBUS, 2);
    case X86_INS_PAVGB: case X86_INS_VPAVGB: P(VO_AVG, 1);
    case X86_INS_PAVGW: case X86_INS_VPAVGW: P(VO_AVG, 2);
    case X86_INS_PMULLW: case X86_INS_VPMULLW: P(VO_MULLW, 2);
    case X86_INS_PMULHW: case X86_INS_VPMULHW: P(VO_MULHW, 2);
    case X86_INS_PMULHUW: case X86_INS_VPMULHUW: P(VO_MULHUW, 2);
    case X86_INS_PMULLD: case X86_INS_VPMULLD: P(VO_MULLD, 4);
    case X86_INS_PMINSB: case X86_INS_VPMINSB: P(VO_MINS, 1);
    case X86_INS_PMINSW: case X86_INS_VPMINSW: P(VO_MINS, 2);
    case X86_INS_PMINSD: case X86_INS_VPMINSD: P(VO_MINS, 4);
    case X86_INS_PMAXSB: case X86_INS_VPMAXSB: P(VO_MAXS, 1);
    case X86_INS_PMAXSW: case X86_INS_VPMAXSW: P(VO_MAXS, 2);
    case X86_INS_PMAXSD: case X86_INS_VPMAXSD: P(VO_MAXS, 4);
    case X86_INS_PMINUB: case X86_INS_VPMINUB: P(VO_MINU, 1);
    case X86_INS_PMINUW: case X86_INS_VPMINUW: P(VO_MINU, 2);
    case X86_INS_PMINUD: case X86_INS_VPMINUD: P(VO_MINU, 4);
    case X86_INS_PMAXUB: case X86_INS_VPMAXUB: P(VO_MAXU, 1);
    case X86_INS_PMAXUW: case X86_INS_VPMAXUW: P(VO_MAXU, 2);
    case X86_INS_PMAXUD: case X86_INS_VPMAXUD: P(VO_MAXU, 4);

    case X86_INS_PMADDWD: case X86_INS_VPMADDWD: {
        /* two 16-bit products summed into each 32-bit lane: the actual
           arithmetic of a video codec, and the reason 3c is a tier */
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + 4 <= bytes; off += 4){
            int64_t p0 = lsext(lget(a, off, 2), 2) * lsext(lget(b, off, 2), 2);
            int64_t p1 = lsext(lget(a, off + 2, 2), 2) * lsext(lget(b, off + 2, 2), 2);
            lput(r, off, 4, (uint64_t)(p0 + p1) & 0xffffffffu);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PMADDUBSW: case X86_INS_VPMADDUBSW: {
        /* unsigned byte times signed byte, pairs summed with saturation */
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + 2 <= bytes; off += 2){
            int32_t p0 = (int32_t)a[off]     * (int32_t)(int8_t)b[off];
            int32_t p1 = (int32_t)a[off + 1] * (int32_t)(int8_t)b[off + 1];
            lput(r, off, 2, (uint64_t)clampi(p0 + p1, -32768, 32767) & 0xffff);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PMULHRSW: case X86_INS_VPMULHRSW: {
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + 2 <= bytes; off += 2){
            int32_t p = (int32_t)lsext(lget(a, off, 2), 2) *
                        (int32_t)lsext(lget(b, off, 2), 2);
            lput(r, off, 2, (uint64_t)(((p >> 14) + 1) >> 1) & 0xffff);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PSADBW: case X86_INS_VPSADBW: {
        /* sum of absolute differences, per 64-bit group */
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        memset(r, 0, (size_t)bytes);
        for (int g = 0; g + 8 <= bytes; g += 8){
            uint64_t sum = 0;
            for (int k = 0; k < 8; k++){
                int x = a[g + k], y = b[g + k];
                sum += (uint64_t)(x > y ? x - y : y - x);
            }
            lput(r, g, 8, sum);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PABSB: case X86_INS_VPABSB:
    case X86_INS_PABSW: case X86_INS_VPABSW:
    case X86_INS_PABSD: case X86_INS_VPABSD: {
        int ew = (id == X86_INS_PABSB || id == X86_INS_VPABSB) ? 1
               : (id == X86_INS_PABSW || id == X86_INS_VPABSW) ? 2 : 4;
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, n >= 2 ? 1 : 0, a, bytes);
        for (int off = 0; off + ew <= bytes; off += ew){
            int64_t v = lsext(lget(a, off, ew), ew);
            lput(r, off, ew, (uint64_t)(v < 0 ? -v : v) & lmask(ew));
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    /* ---- 3b: shuffle, compare, logic, shifts ---------------------- */
    case X86_INS_PCMPEQB: case X86_INS_VPCMPEQB: P(VO_CMPEQ, 1);
    case X86_INS_PCMPEQW: case X86_INS_VPCMPEQW: P(VO_CMPEQ, 2);
    case X86_INS_PCMPEQD: case X86_INS_VPCMPEQD: P(VO_CMPEQ, 4);
    case X86_INS_PCMPEQQ: case X86_INS_VPCMPEQQ: P(VO_CMPEQ, 8);
    case X86_INS_PCMPGTB: case X86_INS_VPCMPGTB: P(VO_CMPGT, 1);
    case X86_INS_PCMPGTW: case X86_INS_VPCMPGTW: P(VO_CMPGT, 2);
    case X86_INS_PCMPGTD: case X86_INS_VPCMPGTD: P(VO_CMPGT, 4);
    case X86_INS_PCMPGTQ: case X86_INS_VPCMPGTQ: P(VO_CMPGT, 8);
    case X86_INS_PAND:  case X86_INS_VPAND:  case X86_INS_ANDPS:
    case X86_INS_ANDPD: case X86_INS_VANDPS: case X86_INS_VANDPD: P(VO_AND, 8);
    case X86_INS_PANDN: case X86_INS_VPANDN: case X86_INS_ANDNPS:
    case X86_INS_ANDNPD: case X86_INS_VANDNPS: case X86_INS_VANDNPD: P(VO_ANDN, 8);
    case X86_INS_POR:  case X86_INS_VPOR:  case X86_INS_ORPS:
    case X86_INS_ORPD: case X86_INS_VORPS: case X86_INS_VORPD: P(VO_OR, 8);
    case X86_INS_PXOR: case X86_INS_VPXOR: case X86_INS_XORPS:
    case X86_INS_XORPD: case X86_INS_VXORPS: case X86_INS_VXORPD: P(VO_XOR, 8);
    case X86_INS_PSLLW: case X86_INS_VPSLLW: SH(VO_SLL, 2);
    case X86_INS_PSLLD: case X86_INS_VPSLLD: SH(VO_SLL, 4);
    case X86_INS_PSLLQ: case X86_INS_VPSLLQ: SH(VO_SLL, 8);
    case X86_INS_PSRLW: case X86_INS_VPSRLW: SH(VO_SRL, 2);
    case X86_INS_PSRLD: case X86_INS_VPSRLD: SH(VO_SRL, 4);
    case X86_INS_PSRLQ: case X86_INS_VPSRLQ: SH(VO_SRL, 8);
    case X86_INS_PSRAW: case X86_INS_VPSRAW: SH(VO_SRA, 2);
    case X86_INS_PSRAD: case X86_INS_VPSRAD: SH(VO_SRA, 4);

    case X86_INS_PSLLDQ: case X86_INS_VPSLLDQ:
    case X86_INS_PSRLDQ: case X86_INS_VPSRLDQ: {
        /* a byte shift of the whole 128-bit lane, not of elements */
        int left = (id == X86_INS_PSLLDQ || id == X86_INS_VPSLLDQ);
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes);
        int sh = (int)((uint64_t)info->op[s2].imm & 0xff);
        memset(r, 0, (size_t)bytes);
        for (int g = 0; g + 16 <= bytes || (g == 0 && bytes < 16); g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            for (int k = 0; k < w; k++){
                int from = left ? k - sh : k + sh;
                r[g + k] = (from >= 0 && from < w) ? a[g + from] : 0;
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PUNPCKLBW: case X86_INS_VPUNPCKLBW:
    case X86_INS_PUNPCKLWD: case X86_INS_VPUNPCKLWD:
    case X86_INS_PUNPCKLDQ: case X86_INS_VPUNPCKLDQ:
    case X86_INS_PUNPCKLQDQ: case X86_INS_VPUNPCKLQDQ:
    case X86_INS_PUNPCKHBW: case X86_INS_VPUNPCKHBW:
    case X86_INS_PUNPCKHWD: case X86_INS_VPUNPCKHWD:
    case X86_INS_PUNPCKHDQ: case X86_INS_VPUNPCKHDQ:
    case X86_INS_PUNPCKHQDQ: case X86_INS_VPUNPCKHQDQ: {
        int ew = (id == X86_INS_PUNPCKLBW || id == X86_INS_VPUNPCKLBW ||
                  id == X86_INS_PUNPCKHBW || id == X86_INS_VPUNPCKHBW) ? 1
               : (id == X86_INS_PUNPCKLWD || id == X86_INS_VPUNPCKLWD ||
                  id == X86_INS_PUNPCKHWD || id == X86_INS_VPUNPCKHWD) ? 2
               : (id == X86_INS_PUNPCKLDQ || id == X86_INS_VPUNPCKLDQ ||
                  id == X86_INS_PUNPCKHDQ || id == X86_INS_VPUNPCKHDQ) ? 4 : 8;
        int high = (id == X86_INS_PUNPCKHBW || id == X86_INS_VPUNPCKHBW ||
                    id == X86_INS_PUNPCKHWD || id == X86_INS_VPUNPCKHWD ||
                    id == X86_INS_PUNPCKHDQ || id == X86_INS_VPUNPCKHDQ ||
                    id == X86_INS_PUNPCKHQDQ || id == X86_INS_VPUNPCKHQDQ);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        /* interleave within each 128-bit lane, which is what makes this
           the same instruction at 256 bits */
        for (int g = 0; g < bytes; g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            int base = high ? w / 2 : 0;
            for (int k = 0; k * 2 * ew < w; k++){
                memcpy(r + g + k * 2 * ew,      a + g + base + k * ew, (size_t)ew);
                memcpy(r + g + k * 2 * ew + ew, b + g + base + k * ew, (size_t)ew);
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PSHUFD: case X86_INS_VPSHUFD: {
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        int si = (n >= 3) ? 1 : 0, ii = (n >= 3) ? 2 : 1;
        vsrc(c, si, a, bytes);
        unsigned sel = (unsigned)info->op[ii].imm & 0xff;
        for (int g = 0; g < bytes; g += 16)
            for (int k = 0; k < 4 && g + k * 4 + 4 <= bytes; k++)
                memcpy(r + g + k * 4, a + g + ((sel >> (k * 2)) & 3) * 4, 4);
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PSHUFB: case X86_INS_VPSHUFB: {
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int g = 0; g < bytes; g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            for (int k = 0; k < w; k++){
                uint8_t sel = b[g + k];
                r[g + k] = (sel & 0x80) ? 0 : a[g + (sel & (uint8_t)(w - 1))];
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PACKSSWB: case X86_INS_VPACKSSWB:
    case X86_INS_PACKUSWB: case X86_INS_VPACKUSWB:
    case X86_INS_PACKSSDW: case X86_INS_VPACKSSDW:
    case X86_INS_PACKUSDW: case X86_INS_VPACKUSDW: {
        int ew = (id == X86_INS_PACKSSDW || id == X86_INS_VPACKSSDW ||
                  id == X86_INS_PACKUSDW || id == X86_INS_VPACKUSDW) ? 4 : 2;
        int uns = (id == X86_INS_PACKUSWB || id == X86_INS_VPACKUSWB ||
                   id == X86_INS_PACKUSDW || id == X86_INS_VPACKUSDW);
        int ow = ew / 2;
        int64_t lo = uns ? 0 : -(int64_t)(lmask(ow) >> 1) - 1;
        int64_t hi = uns ? (int64_t)lmask(ow) : (int64_t)(lmask(ow) >> 1);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int g = 0; g < bytes; g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            int half = w / 2 / ow;
            for (int k = 0; k < half; k++){
                lput(r, g + k * ow, ow,
                     (uint64_t)clampi(lsext(lget(a, g + k * ew, ew), ew), lo, hi) & lmask(ow));
                lput(r, g + w / 2 + k * ow, ow,
                     (uint64_t)clampi(lsext(lget(b, g + k * ew, ew), ew), lo, hi) & lmask(ow));
            }
        }
        m->vecEw = (uint8_t)(ew / 2);
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PMOVMSKB: case X86_INS_VPMOVMSKB: {
        int sw = opw(info, 1);
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, sw);
        uint64_t mask = 0;
        for (int k = 0; k < sw; k++) if (a[k] & 0x80) mask |= 1ull << k;
        vm_reg_set(m, info->op[0].reg, mask, PV_DERIVED);
        return 1;
    }
    case X86_INS_MOVMSKPS: case X86_INS_VMOVMSKPS:
    case X86_INS_MOVMSKPD: case X86_INS_VMOVMSKPD: {
        int ew = (id == X86_INS_MOVMSKPD || id == X86_INS_VMOVMSKPD) ? 8 : 4;
        int sw = opw(info, 1);
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, sw);
        uint64_t mask = 0;
        for (int k = 0; k * ew < sw; k++)
            if (a[k * ew + ew - 1] & 0x80) mask |= 1ull << k;
        vm_reg_set(m, info->op[0].reg, mask, PV_DERIVED);
        return 1;
    }

    case X86_INS_PMULUDQ: case X86_INS_VPMULUDQ:
    case X86_INS_PMULDQ:  case X86_INS_VPMULDQ: {
        /* the even 32-bit lanes, widened into 64 */
        int sgn = (id == X86_INS_PMULDQ || id == X86_INS_VPMULDQ);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + 8 <= bytes; off += 8){
            uint64_t x = lget(a, off, 4), y = lget(b, off, 4);
            uint64_t p = sgn ? (uint64_t)((int64_t)lsext(x, 4) * (int64_t)lsext(y, 4))
                             : x * y;
            lput(r, off, 8, p);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PALIGNR: case X86_INS_VPALIGNR: {
        int si1 = (n >= 4) ? 1 : 0, si2 = (n >= 4) ? 2 : 1, ii = n - 1;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, si1, a, bytes); vsrc(c, si2, b, bytes);
        int sh = (int)((uint64_t)info->op[ii].imm & 0xff);
        for (int g = 0; g < bytes; g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            for (int k = 0; k < w; k++){
                int at2 = k + sh;
                r[g + k] = at2 < w ? b[g + at2] : (at2 < 2 * w ? a[g + at2 - w] : 0);
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PSHUFLW: case X86_INS_VPSHUFLW:
    case X86_INS_PSHUFHW: case X86_INS_VPSHUFHW: {
        int high = (id == X86_INS_PSHUFHW || id == X86_INS_VPSHUFHW);
        int si = (n >= 3) ? 1 : 0, ii = (n >= 3) ? 2 : 1;
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, si, a, bytes);
        unsigned sel = (unsigned)info->op[ii].imm & 0xff;
        memcpy(r, a, (size_t)bytes);
        for (int g = 0; g < bytes; g += 16){
            int base = high ? 8 : 0;
            for (int k = 0; k < 4; k++)
                memcpy(r + g + base + k * 2, a + g + base + ((sel >> (k * 2)) & 3) * 2, 2);
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PINSRW: case X86_INS_VPINSRW: {
        int si = (n >= 4) ? 2 : 1, ii = n - 1;
        uint8_t r[VM_VBYTES];
        vsrc(c, (n >= 4) ? 1 : 0, r, bytes);
        uint64_t v = (info->op[si].kind == OPK_REG)
                   ? vm_reg_get(m, info->op[si].reg)
                   : 0;
        if (info->op[si].kind == OPK_MEM){ uint8_t t2[8]; vsrc(c, si, t2, 2); v = lget(t2, 0, 2); }
        int slot = (int)((uint64_t)info->op[ii].imm & 7);
        lput(r, slot * 2, 2, v & 0xffff);
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_PEXTRW: case X86_INS_VPEXTRW: {
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, 16);
        int slot = (int)((uint64_t)info->op[2].imm & 7);
        vm_reg_set(m, info->op[0].reg, lget(a, slot * 2, 2), PV_DERIVED);
        return 1;
    }

    case X86_INS_UNPCKLPS: case X86_INS_VUNPCKLPS:
    case X86_INS_UNPCKHPS: case X86_INS_VUNPCKHPS:
    case X86_INS_UNPCKLPD: case X86_INS_VUNPCKLPD:
    case X86_INS_UNPCKHPD: case X86_INS_VUNPCKHPD: {
        int ew = (id == X86_INS_UNPCKLPD || id == X86_INS_VUNPCKLPD ||
                  id == X86_INS_UNPCKHPD || id == X86_INS_VUNPCKHPD) ? 8 : 4;
        int high = (id == X86_INS_UNPCKHPS || id == X86_INS_VUNPCKHPS ||
                    id == X86_INS_UNPCKHPD || id == X86_INS_VUNPCKHPD);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int g = 0; g < bytes; g += 16){
            int w = bytes - g < 16 ? bytes - g : 16;
            int base = high ? w / 2 : 0;
            for (int k = 0; k * 2 * ew < w; k++){
                memcpy(r + g + k * 2 * ew,      a + g + base + k * ew, (size_t)ew);
                memcpy(r + g + k * 2 * ew + ew, b + g + base + k * ew, (size_t)ew);
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_MOVLHPS: case X86_INS_MOVHLPS:
    case X86_INS_VMOVLHPS: case X86_INS_VMOVHLPS: {
        int lh = (id == X86_INS_MOVLHPS || id == X86_INS_VMOVLHPS);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, 16); vsrc(c, s2, b, 16);
        memcpy(r, a, 16);
        if (lh) memcpy(r + 8, b, 8);            /* low of src -> high of dst */
        else    memcpy(r, b + 8, 8);            /* high of src -> low of dst */
        vdst(c, d, r, 16, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_SHUFPS: case X86_INS_VSHUFPS:
    case X86_INS_SHUFPD: case X86_INS_VSHUFPD: {
        int pd = (id == X86_INS_SHUFPD || id == X86_INS_VSHUFPD);
        int si1 = (n >= 4) ? 1 : 0, si2 = (n >= 4) ? 2 : 1, ii = n - 1;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, si1, a, bytes); vsrc(c, si2, b, bytes);
        unsigned sel = (unsigned)info->op[ii].imm & 0xff;
        for (int g = 0; g < bytes; g += 16){
            if (pd){
                memcpy(r + g,     a + g + ((sel >> (g / 16 * 2 + 0)) & 1) * 8, 8);
                memcpy(r + g + 8, b + g + ((sel >> (g / 16 * 2 + 1)) & 1) * 8, 8);
            } else {
                for (int k = 0; k < 2; k++)
                    memcpy(r + g + k * 4, a + g + ((sel >> (k * 2)) & 3) * 4, 4);
                for (int k = 2; k < 4; k++)
                    memcpy(r + g + k * 4, b + g + ((sel >> (k * 2)) & 3) * 4, 4);
            }
        }
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    /* ---- packed float: the same lane loop with a float element ----- */
    case X86_INS_ADDPS: case X86_INS_ADDPD: case X86_INS_SUBPS: case X86_INS_SUBPD:
    case X86_INS_MULPS: case X86_INS_MULPD: case X86_INS_DIVPS: case X86_INS_DIVPD:
    case X86_INS_VADDPS: case X86_INS_VADDPD: case X86_INS_VSUBPS: case X86_INS_VSUBPD:
    case X86_INS_VMULPS: case X86_INS_VMULPD: case X86_INS_VDIVPS: case X86_INS_VDIVPD: {
        int pd = (id == X86_INS_ADDPD || id == X86_INS_SUBPD || id == X86_INS_MULPD ||
                  id == X86_INS_DIVPD || id == X86_INS_VADDPD || id == X86_INS_VSUBPD ||
                  id == X86_INS_VMULPD || id == X86_INS_VDIVPD);
        int ew = pd ? 8 : 4;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + ew <= bytes; off += ew){
            if (f_nan_result(a + off, b + off, !pd, r + off)) continue;
            double x = f_get(a + off, !pd), y = f_get(b + off, !pd), z;
            switch (id){
            case X86_INS_ADDPS: case X86_INS_ADDPD:
            case X86_INS_VADDPS: case X86_INS_VADDPD: z = x + y; break;
            case X86_INS_SUBPS: case X86_INS_SUBPD:
            case X86_INS_VSUBPS: case X86_INS_VSUBPD: z = x - y; break;
            case X86_INS_MULPS: case X86_INS_MULPD:
            case X86_INS_VMULPS: case X86_INS_VMULPD: z = x * y; break;
            default:                                  z = x / y; break;
            }
            f_put(r + off, !pd, z);
        }
        m->vecEw = (uint8_t)ew;
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PMOVZXBW: case X86_INS_VPMOVZXBW:
    case X86_INS_PMOVZXBD: case X86_INS_VPMOVZXBD:
    case X86_INS_PMOVZXBQ: case X86_INS_VPMOVZXBQ:
    case X86_INS_PMOVZXWD: case X86_INS_VPMOVZXWD:
    case X86_INS_PMOVZXWQ: case X86_INS_VPMOVZXWQ:
    case X86_INS_PMOVZXDQ: case X86_INS_VPMOVZXDQ:
    case X86_INS_PMOVSXBW: case X86_INS_VPMOVSXBW:
    case X86_INS_PMOVSXBD: case X86_INS_VPMOVSXBD:
    case X86_INS_PMOVSXBQ: case X86_INS_VPMOVSXBQ:
    case X86_INS_PMOVSXWD: case X86_INS_VPMOVSXWD:
    case X86_INS_PMOVSXWQ: case X86_INS_VPMOVSXWQ:
    case X86_INS_PMOVSXDQ: case X86_INS_VPMOVSXDQ: {
        /* widen every element, zero- or sign-extending */
        int sw, dw2, sgn = 0;
        switch (id){
        case X86_INS_PMOVSXBW: case X86_INS_VPMOVSXBW: sgn = 1; /* fall through */
        case X86_INS_PMOVZXBW: case X86_INS_VPMOVZXBW: sw = 1; dw2 = 2; break;
        case X86_INS_PMOVSXBD: case X86_INS_VPMOVSXBD: sgn = 1; /* fall through */
        case X86_INS_PMOVZXBD: case X86_INS_VPMOVZXBD: sw = 1; dw2 = 4; break;
        case X86_INS_PMOVSXBQ: case X86_INS_VPMOVSXBQ: sgn = 1; /* fall through */
        case X86_INS_PMOVZXBQ: case X86_INS_VPMOVZXBQ: sw = 1; dw2 = 8; break;
        case X86_INS_PMOVSXWD: case X86_INS_VPMOVSXWD: sgn = 1; /* fall through */
        case X86_INS_PMOVZXWD: case X86_INS_VPMOVZXWD: sw = 2; dw2 = 4; break;
        case X86_INS_PMOVSXWQ: case X86_INS_VPMOVSXWQ: sgn = 1; /* fall through */
        case X86_INS_PMOVZXWQ: case X86_INS_VPMOVZXWQ: sw = 2; dw2 = 8; break;
        case X86_INS_PMOVSXDQ: case X86_INS_VPMOVSXDQ: sgn = 1; /* fall through */
        default:                                        sw = 4; dw2 = 8; break;
        }
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, bytes / (dw2 / sw) > 0 ? bytes / (dw2 / sw) : sw);
        for (int k = 0; (k + 1) * dw2 <= bytes; k++){
            uint64_t v = lget(a, k * sw, sw);
            lput(r, k * dw2, dw2, sgn ? ((uint64_t)lsext(v, sw) & lmask(dw2)) : v);
        }
        m->vecEw = (uint8_t)dw2;
        vdst(c, 0, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PSIGNB: case X86_INS_VPSIGNB:
    case X86_INS_PSIGNW: case X86_INS_VPSIGNW:
    case X86_INS_PSIGND: case X86_INS_VPSIGND: {
        int ew = (id == X86_INS_PSIGNB || id == X86_INS_VPSIGNB) ? 1
               : (id == X86_INS_PSIGNW || id == X86_INS_VPSIGNW) ? 2 : 4;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, s1, a, bytes); vsrc(c, s2, b, bytes);
        for (int off = 0; off + ew <= bytes; off += ew){
            int64_t sgn = lsext(lget(b, off, ew), ew);
            uint64_t v = lget(a, off, ew);
            lput(r, off, ew, sgn < 0 ? ((uint64_t)(-(int64_t)lsext(v, ew)) & lmask(ew))
                                     : (sgn == 0 ? 0 : v));
        }
        m->vecEw = (uint8_t)ew;
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_PBLENDW: case X86_INS_VPBLENDW: {
        int si1 = (n >= 4) ? 1 : 0, si2 = (n >= 4) ? 2 : 1, ii = n - 1;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, si1, a, bytes); vsrc(c, si2, b, bytes);
        unsigned sel = (unsigned)info->op[ii].imm & 0xff;
        for (int g = 0; g < bytes; g += 16)
            for (int k = 0; k < 8 && g + k * 2 + 2 <= bytes; k++)
                memcpy(r + g + k * 2, ((sel >> k) & 1 ? b : a) + g + k * 2, 2);
        m->vecEw = 2;
        vdst(c, d, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_MOVDDUP: case X86_INS_VMOVDDUP: {
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, bytes);
        for (int g = 0; g + 16 <= bytes || (g == 0 && bytes >= 8); g += 16){
            memcpy(r + g, a + g, 8);
            if (g + 16 <= bytes) memcpy(r + g + 8, a + g, 8);
        }
        m->vecEw = 8;
        vdst(c, 0, r, bytes, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_MOVSLDUP: case X86_INS_VMOVSLDUP:
    case X86_INS_MOVSHDUP: case X86_INS_VMOVSHDUP: {
        int odd = (id == X86_INS_MOVSHDUP || id == X86_INS_VMOVSHDUP);
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, bytes);
        for (int k = 0; (k + 1) * 4 <= bytes; k++)
            memcpy(r + k * 4, a + ((k & ~1) + (odd ? 1 : 0)) * 4, 4);
        m->vecEw = 4;
        vdst(c, 0, r, bytes, PV_DERIVED);
        return !c->fault;
    }

    /* ---- the lane-crossing operations ------------------------------
       §4.3.1's short list: the only AVX2 that does *not* fall out of a
       tier-3 loop at another width, because nothing in SSE moves data
       between 128-bit lanes.  About ten instructions, and they are the
       whole of the "genuinely new" column.                          */
    case X86_INS_VPBROADCASTB: case X86_INS_VPBROADCASTW:
    case X86_INS_VPBROADCASTD: case X86_INS_VPBROADCASTQ:
    case X86_INS_VBROADCASTSS: case X86_INS_VBROADCASTSD: {
        int ew = (id == X86_INS_VPBROADCASTB) ? 1
               : (id == X86_INS_VPBROADCASTW) ? 2
               : (id == X86_INS_VPBROADCASTD || id == X86_INS_VBROADCASTSS) ? 4 : 8;
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, 16);
        for (int off = 0; off + ew <= bytes; off += ew) memcpy(r + off, a, (size_t)ew);
        m->vecEw = (uint8_t)ew;
        vdst(c, 0, r, bytes, PV_DERIVED);
        return !c->fault;
    }
    /* Capstone 4.0.2 does not decode `vbroadcasti128`, so only the
       float form and the EVEX spelling ever arrive here. */
    case X86_INS_VBROADCASTI32X4: case X86_INS_VBROADCASTF128: {
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, 16);
        memcpy(r, a, 16);
        memcpy(r + 16, a, 16);
        vdst(c, 0, r, 32, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VINSERTI128: case X86_INS_VINSERTF128: {
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, 32); vsrc(c, 2, b, 16);
        memcpy(r, a, 32);
        memcpy(r + ((info->op[3].imm & 1) ? 16 : 0), b, 16);
        vdst(c, 0, r, 32, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VEXTRACTI128: case X86_INS_VEXTRACTF128: {
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, 32);
        vdst(c, 0, a + ((info->op[2].imm & 1) ? 16 : 0), 16, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VPERM2I128: case X86_INS_VPERM2F128: {
        uint8_t a[VM_VBYTES], b[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, 32); vsrc(c, 2, b, 32);
        unsigned sel = (unsigned)info->op[3].imm & 0xff;
        for (int h = 0; h < 2; h++){
            unsigned f = (sel >> (h * 4)) & 0xf;
            if (f & 0x8) memset(r + h * 16, 0, 16);
            else memcpy(r + h * 16, (f & 2 ? b : a) + (f & 1 ? 16 : 0), 16);
        }
        vdst(c, 0, r, 32, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VPERMQ: case X86_INS_VPERMPD: {
        uint8_t a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, a, 32);
        unsigned sel = (unsigned)info->op[2].imm & 0xff;
        for (int k = 0; k < 4; k++) memcpy(r + k * 8, a + ((sel >> (k * 2)) & 3) * 8, 8);
        m->vecEw = 8;
        vdst(c, 0, r, 32, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VPERMD: case X86_INS_VPERMPS: {
        /* the index vector is the *first* source for vpermd */
        uint8_t idx[VM_VBYTES], a[VM_VBYTES], r[VM_VBYTES];
        vsrc(c, 1, idx, 32); vsrc(c, 2, a, 32);
        for (int k = 0; k < 8; k++)
            memcpy(r + k * 4, a + (int)(lget(idx, k * 4, 4) & 7) * 4, 4);
        m->vecEw = 4;
        vdst(c, 0, r, 32, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_VZEROUPPER: case X86_INS_VZEROALL: {
        int all = (id == X86_INS_VZEROALL);
        uint8_t z[VM_VBYTES];
        memset(z, 0, sizeof z);
        for (int i = 0; i < m->nvreg && i < 16; i++){
            if (all){ vm_vsetz(m, i, 32, z, PV_DERIVED); continue; }
            uint8_t keep[VM_VBYTES];
            vm_vget(m, i, VM_VBYTES, keep);
            memset(keep + 16, 0, (size_t)(VM_VBYTES - 16));
            vm_vset(m, i, VM_VBYTES, keep, m->v[i].prov);
        }
        return 1;
    }

    /* ---- 3a: the bulk moves --------------------------------------- */
    case X86_INS_MOVDQA: case X86_INS_MOVDQU: case X86_INS_MOVAPS:
    case X86_INS_MOVAPD: case X86_INS_MOVUPS: case X86_INS_MOVUPD:
    case X86_INS_VMOVDQA: case X86_INS_VMOVDQU: case X86_INS_VMOVAPS:
    case X86_INS_VMOVAPD: case X86_INS_VMOVUPS: case X86_INS_VMOVUPD:
    case X86_INS_MOVNTDQ: case X86_INS_MOVNTPS: case X86_INS_LDDQU:
    case X86_INS_VLDDQU: case X86_INS_MOVDQ2Q: {
        int w = opw(info, 0);
        if (w > VM_VBYTES) w = VM_VBYTES;
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, w);
        vdst(c, 0, a, w, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_MOVD: case X86_INS_VMOVD:
    case X86_INS_MOVQ: case X86_INS_VMOVQ: {
        int w = (id == X86_INS_MOVD || id == X86_INS_VMOVD) ? 4 : 8;
        uint8_t a[VM_VBYTES];
        memset(a, 0, sizeof a);
        vsrc(c, 1, a, w);
        /* into a vector register it zero-extends the whole thing */
        int vs = info->op[0].kind == OPK_REG ? vm_vslot(m, info->op[0].reg) : -1;
        if (vs >= 0){
            /* the legacy form zeroes bits 127:32 and leaves 255:128 alone;
               only the VEX form clears the whole register */
            uint8_t z[VM_VBYTES];
            memset(z, 0, sizeof z);
            memcpy(z, a, (size_t)w);
            if (c->vex) vm_vsetz(m, vs, 16, z, PV_DERIVED);
            else        vm_vset(m, vs, 16, z, PV_DERIVED);
            m->vecSlot = vs; m->vecBytes = 16;
            if (!m->vecEw) m->vecEw = (uint8_t)w;
        }
        else vdst(c, 0, a, w, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_MOVLPS: case X86_INS_MOVLPD: case X86_INS_MOVHPS:
    case X86_INS_MOVHPD: case X86_INS_MOVSD: case X86_INS_MOVSS: {
        /* movss/movsd between registers merge; from memory they zero the
           rest, which is the distinction the corpus is there to check  */
        int single = (id == X86_INS_MOVSS);
        int w = (id == X86_INS_MOVLPS || id == X86_INS_MOVLPD ||
                 id == X86_INS_MOVHPS || id == X86_INS_MOVHPD) ? 8
              : (single ? 4 : 8);
        int high = (id == X86_INS_MOVHPS || id == X86_INS_MOVHPD);
        if (n < 2){ *tier = 2; return 0; }
        uint8_t a[VM_VBYTES], dstv[VM_VBYTES];
        memset(a, 0, sizeof a);
        vsrc(c, 1, a, w);
        int dv = info->op[0].kind == OPK_REG ? vm_vslot(m, info->op[0].reg) : -1;
        if (dv >= 0){
            vm_vget(m, dv, VM_VBYTES, dstv);
            if (info->op[1].kind == OPK_MEM && !high &&
                (id == X86_INS_MOVSS || id == X86_INS_MOVSD))
                memset(dstv, 0, 16);            /* from memory: zeroes the rest */
            memcpy(dstv + (high ? 8 : 0), a, (size_t)w);
            if (c->vex) vm_vsetz(m, dv, 16, dstv, PV_DERIVED);
            else        vm_vset(m, dv, 16, dstv, PV_DERIVED);
        } else {
            uint8_t src[VM_VBYTES];
            vsrc(c, 1, src, VM_VBYTES);
            vdst(c, 0, src + (high ? 8 : 0), w, PV_DERIVED);
        }
        *tier = (id == X86_INS_MOVSS || id == X86_INS_MOVSD) ? 2 : 3;
        return !c->fault;
    }

    /* ---- 2: scalar float ------------------------------------------ */
    case X86_INS_MINSS: case X86_INS_MINSD: case X86_INS_MAXSS: case X86_INS_MAXSD: {
        /* The result is one of the two operands, bit for bit -- DEST when
           the comparison holds and SRC otherwise, which is why a NaN in
           either gives SRC.  Recomputing it through a double quiets a
           signalling NaN and changes the answer; hardware caught that. */
        *tier = 2;
        int single = (id == X86_INS_MINSS || id == X86_INS_MAXSS);
        int w = single ? 4 : 8;
        int mx = (id == X86_INS_MAXSS || id == X86_INS_MAXSD);
        uint8_t a[VM_VBYTES], b[VM_VBYTES], dv[VM_VBYTES];
        vsrc(c, s1, a, VM_VBYTES);
        vsrc(c, s2, b, w);
        double x = f_get(a, single), y = f_get(b, single);
        int keepDest = mx ? (x > y) : (x < y);
        memcpy(dv, a, VM_VBYTES);
        if (!keepDest) memcpy(dv, b, (size_t)w);
        vdst(c, d, dv, 16, PV_DERIVED);
        return !c->fault;
    }

    case X86_INS_ADDSS: case X86_INS_ADDSD: case X86_INS_SUBSS: case X86_INS_SUBSD:
    case X86_INS_MULSS: case X86_INS_MULSD: case X86_INS_DIVSS: case X86_INS_DIVSD: {
        *tier = 2;
        int single = (id == X86_INS_ADDSS || id == X86_INS_SUBSS ||
                      id == X86_INS_MULSS || id == X86_INS_DIVSS);
        int w = single ? 4 : 8;
        uint8_t a[VM_VBYTES], b[VM_VBYTES], dv[VM_VBYTES];
        vsrc(c, s1, a, VM_VBYTES);
        vsrc(c, s2, b, w);
        memcpy(dv, a, VM_VBYTES);
        if (f_nan_result(a, b, single, dv)){
            vdst(c, d, dv, 16, PV_DERIVED);
            return !c->fault;
        }
        double x = f_get(a, single), y = f_get(b, single), r;
        switch (id){
        case X86_INS_ADDSS: case X86_INS_ADDSD: r = x + y; break;
        case X86_INS_SUBSS: case X86_INS_SUBSD: r = x - y; break;
        case X86_INS_MULSS: case X86_INS_MULSD: r = x * y; break;
        default:                                r = x / y; break;
        }
        f_put(dv, single, r);
        vdst(c, d, dv, 16, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_SQRTSS: case X86_INS_SQRTSD: {
        *tier = 2;
        int single = (id == X86_INS_SQRTSS);
        uint8_t a[VM_VBYTES], dv[VM_VBYTES];
        vsrc(c, n >= 2 ? 1 : 0, a, VM_VBYTES);
        vsrc(c, 0, dv, VM_VBYTES);
        f_put(dv, single, sqrt(f_get(a, single)));
        vdst(c, d, dv, 16, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_COMISS: case X86_INS_COMISD:
    case X86_INS_UCOMISS: case X86_INS_UCOMISD: {
        *tier = 2;
        int single = (id == X86_INS_COMISS || id == X86_INS_UCOMISS);
        uint8_t a[VM_VBYTES], b[VM_VBYTES];
        vsrc(c, 0, a, VM_VBYTES);
        vsrc(c, 1, b, single ? 4 : 8);
        double x = f_get(a, single), y = f_get(b, single);
        int un = isnan(x) || isnan(y);
        m->fl[VF_ZF] = (uint8_t)(un || x == y);
        m->fl[VF_CF] = (uint8_t)(un || x < y);
        m->fl[VF_PF] = (uint8_t)un;
        m->fl[VF_OF] = m->fl[VF_SF] = m->fl[VF_AF] = 0;
        m->flknown = (1u << VF_COUNT) - 1;
        return 1;
    }
    case X86_INS_CVTSI2SS: case X86_INS_CVTSI2SD: {
        *tier = 2;
        int single = (id == X86_INS_CVTSI2SS);
        int si = n >= 3 ? 2 : 1;
        uint64_t v = 0;
        if (info->op[si].kind == OPK_REG) v = vm_reg_get(m, info->op[si].reg);
        else { uint8_t t2[8]; vsrc(c, si, t2, opw(info, si)); v = lget(t2, 0, opw(info, si)); }
        int64_t sv = lsext(v, opw(info, si));
        uint8_t dv[VM_VBYTES];
        vsrc(c, 0, dv, VM_VBYTES);
        f_put(dv, single, (double)sv);
        vdst(c, d, dv, 16, PV_DERIVED);
        return !c->fault;
    }
    case X86_INS_CVTSS2SI: case X86_INS_CVTSD2SI:
    case X86_INS_CVTTSS2SI: case X86_INS_CVTTSD2SI: {
        *tier = 2;
        int single = (id == X86_INS_CVTSS2SI || id == X86_INS_CVTTSS2SI);
        int chop = (id == X86_INS_CVTTSS2SI || id == X86_INS_CVTTSD2SI);
        int dw = opw(info, 0);
        uint8_t a[VM_VBYTES];
        vsrc(c, 1, a, VM_VBYTES);
        double x = f_get(a, single);
        double v = chop ? trunc(x) : nearbyint(x);
        /* Out of range, or NaN, gives the "integer indefinite" value --
           not whatever a C cast happens to do, which is undefined and was
           returning zero.  Another one the corpus caught.            */
        double lim = (dw >= 8) ? 9223372036854775808.0 : 2147483648.0;
        uint64_t r;
        if (isnan(v) || v >= lim || v < -lim)
            r = (dw >= 8) ? 0x8000000000000000ull : 0xffffffff80000000ull;
        else
            r = (uint64_t)(int64_t)v;
        vm_reg_set(m, info->op[0].reg, r, PV_DERIVED);
        return 1;
    }
    case X86_INS_CVTSS2SD: case X86_INS_CVTSD2SS: {
        *tier = 2;
        int fromSingle = (id == X86_INS_CVTSS2SD);
        uint8_t a[VM_VBYTES], dv[VM_VBYTES];
        vsrc(c, n >= 3 ? 2 : 1, a, VM_VBYTES);
        vsrc(c, 0, dv, VM_VBYTES);
        f_put(dv, !fromSingle, f_get(a, fromSingle));
        vdst(c, d, dv, 16, PV_DERIVED);
        return !c->fault;
    }

    default:
        return 0;                       /* tier 0 takes it, and says so */
    }
#undef P
#undef SH
}
