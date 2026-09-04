/* ehframe.h -- recover function boundaries from unwind tables
 *
 * .eh_frame is loaded at runtime for stack unwinding, so strip(1) cannot
 * remove it.  Every FDE names one function's start and length, which makes
 * it an exact function table for 99% of the binaries in /usr/bin.
 */
#ifndef EHFRAME_H
#define EHFRAME_H
#include <stdint.h>
#include "model.h"

typedef struct { uint64_t addr, size; } Unitrange;

/* All FDE ranges in the file, sorted by address.  Caller frees *out.
   Returns the count, or 0 if the file has no usable .eh_frame.          */
int ehframe_units(const Elf *e, Unitrange **out);

/* Byte-level seeds for spans no FDE covers: direct call targets, plus
   endbr64 landing pads that are not the target of a jump.  Appends into
   *out (realloc'd) and returns the new total, still sorted and unique.  */
int seed_units(const Elf *e, const Sec *s, uint64_t lo, uint64_t hi,
               Unitrange **out, int n);

#endif
