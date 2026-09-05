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

#endif
