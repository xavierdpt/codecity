/* oracle.c -- a controlled place for gdb to single-step one instruction.
 *
 * Not part of codecity.  It is the inferior half of the differential oracle
 * of §16: tools/oracle.py drives it under gdb, writing one instruction into
 * `scratch`, setting the sixteen GPRs and eflags to a chosen vector,
 * pointing rip at the page and stepping once.  The process never runs free,
 * a fault is caught by gdb rather than being fatal, and nothing from the
 * file under study is ever executed -- the bytes are assembled by us, from
 * tools/oracle-forms.s.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

unsigned char *scratch;         /* RWX: the instruction under test */
unsigned char *data;            /* RW : operands and a stack       */

void probe(void){ }             /* gdb breaks here */

int main(void){
    scratch = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    data    = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED || data == MAP_FAILED){ perror("mmap"); return 1; }
    /* cld (0xfc), not int3 and not nop.  Trap padding makes gdb mis-step
       the *following* test; plain nops are benign but silent.  `cld` is
       benign AND leaves a mark: the driver presets DF and, if DF comes
       back clear, a pad byte ran and the step is redone.  Roughly every
       other stepi on this gdb resumes from the previous stop instead of
       the pc we set, and that tripwire is what catches it.          */
    memset(scratch, 0xfc, 4096);
    memset(data, 0, 8192);
    probe();
    /* keep the mappings referenced so nothing is optimised away */
    return scratch[0] == 0 && data[0] == 0;
}
