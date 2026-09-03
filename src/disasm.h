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
    uint16_t len;
    uint8_t  cls;
    char     mnem[20];
    char     ops[88];
    float    x, y, z, h;    /* where its sculpture stands, filled by disasm_layout */
} Insn;

typedef struct {
    Insn    *ins;
    int      n;
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
void    disasm_free(Disasm *d);
const char *disasm_class_name(int cls);
int  disasm_live(void);     /* how many room decodings are allocated right now */

#endif
