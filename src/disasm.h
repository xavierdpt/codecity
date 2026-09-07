#ifndef DISASM_H
#define DISASM_H
#include <stdint.h>

/* what an instruction is, which decides the shape it takes in the room */
enum { IC_OTHER, IC_MOVE, IC_STACK, IC_ARITH, IC_LOGIC, IC_CMP,
       IC_JUMP, IC_CJUMP, IC_CALL, IC_RET, IC_VECTOR, IC_NOP, IC_SYSCALL, IC_COUNT };

typedef struct {
    uint64_t addr;
    uint64_t taddr;         /* branch target address, 0 if not a direct branch */
    int      target;        /* index of that target in this room, or -1 (leaves the room) */
    int      tunit;         /* -1 when the target is in this same decoding; else the
                               chamber alcove it lives in -- still the same room, so
                               the wire runs straight to the sculpture, not to a port */
    int      port;          /* index into Disasm.ports when it leaves the room, else -1 */
    uint16_t len;
    uint8_t  cls;
    char     mnem[20];
    char     ops[88];
    float    x, y, z, h;    /* where its sculpture stands, filled by the layout */
} Insn;

/* one destination the room's code leaves for.  Every distinct address gets
   exactly one port on the far wall, however many instructions target it. */
typedef struct {
    uint64_t addr;
    int      n;             /* instructions in this room that go there */
    int      cls;           /* IC_CALL / IC_JUMP / ... of the first of them */
    char     label[80];     /* what lives there, filled in by the city layer */
    float    x, y, z;       /* panel centre, filled in by the layout */
    float    hw, hh;        /* panel half-extents, in the plane of the wall */
} Port;

typedef struct {
    Insn    *ins;
    int      n;
    Port    *ports;
    int      nports;
    int      nlinks, nexits;
    int      truncated;
    uint64_t covered;       /* bytes actually decoded */
    int      laid;
    float    pitch;
    int      cols, rows;
} Disasm;

int         disasm_open(int elf_machine, int is64, int big_endian);
void        disasm_close(void);
int         disasm_ready(void);
const char *disasm_arch_name(void);

Disasm *disasm_run(const uint8_t *code, uint64_t len, uint64_t vaddr, int maxins);
/* index of the instruction at that address, or -1 */
int     disasm_index_of(const Disasm *d, uint64_t addr);
/* collect the distinct destinations that leave the room into d->ports.
   Call it once the cross-alcove links (if any) have been resolved.      */
void    disasm_ports(Disasm *d);
void    disasm_free(Disasm *d);
const char *disasm_class_name(int cls);
int  disasm_live(void);     /* how many room decodings are allocated right now */

/* ---- one instruction's register and flag effects -------------------
   A room's Disasm keeps no Capstone detail: it would be a few hundred
   bytes an instruction for something only the walker of vm.c reads, and
   the whole point of the enter/leave lifecycle is that a room is cheap.
   So the walker re-decodes the one instruction it is about to execute
   and asks Capstone what it touches.  That is generic across every
   architecture Capstone decodes, which is what makes the tier-0
   fallback work on ARM and MIPS as well as x86.                      */
#define IA_MAXREG 20
#define IA_MAXOP  8

enum { OPK_NONE, OPK_REG, OPK_IMM, OPK_MEM };

typedef struct {
    uint8_t  kind;          /* OPK_* */
    uint8_t  size;          /* bytes the operand is */
    uint8_t  rd, wr;        /* whether the instruction reads / writes it */
    uint16_t reg;           /* OPK_REG */
    int64_t  imm;           /* OPK_IMM */
    uint16_t base, index, seg;   /* OPK_MEM, Capstone register ids */
    int32_t  scale;
    int64_t  disp;
} InsnOp;

typedef struct {
    unsigned id;            /* the architecture's own instruction id */
    char     mnem[20];      /* ... and its name, for reports */
    uint8_t  len;           /* how many bytes it turned out to be */
    uint16_t rd[IA_MAXREG], wr[IA_MAXREG];  /* Capstone register ids */
    uint8_t  nrd, nwr;
    uint8_t  full;          /* 0 when only the implicit registers are known,
                               because this architecture has no operand-level
                               register access in Capstone                  */
    uint8_t  nops;
    InsnOp   op[IA_MAXOP];
    uint64_t eflags;        /* x86: the X86_EFLAGS_* mask; 0 elsewhere */
} InsnInfo;

/* 1 on success.  bytes/len are the instruction's own bytes.  */
int  disasm_info(const uint8_t *bytes, unsigned len, uint64_t addr, InsnInfo *out);
/* Capstone's name for a register id, or NULL.  Ids are architecture-specific
   and only meaningful against the handle the file was opened with.       */
const char *disasm_reg_name(unsigned id);
/* the ELF machine the handle was opened for, so a caller can tell x86 from
   the rest without reaching for the Elf */
int  disasm_machine(void);
/* Every address a RIP-relative operand names, in one pass over a range of
   code.  §10.1: `lea reg, [rip + disp]` is the compiler's universal way of
   naming a constant, and resolving it needs no dataflow at all -- which is
   why most of a binary's strings can be found without running anything. */
int  disasm_riprel(const uint8_t *code, uint64_t len, uint64_t vaddr,
                   void (*hit)(uint64_t addr, int isLea, void *u), void *u);

#endif
