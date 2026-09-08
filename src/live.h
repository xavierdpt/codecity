/* live.h -- a real process, stopped under gdb
 *
 * The wisp of vm.h carries invented values.  This carries measured ones: a
 * process launched (or later attached to) under gdb, stopped, its registers
 * and memory read out over GDB/MI.  Nothing here knows about rooms, wisps or
 * GL -- it is the transport and nothing else, so it can be tested headless
 * before anything is built on it.  See docs/live-wisps.md; this file is L0.
 *
 * Two rules the rest of the program depends on:
 *
 *   1. The child is always reaped.  A codecity that dies leaving a gdb
 *      holding a stopped inferior has frozen a process on the user's
 *      machine.  The child gets PR_SET_PDEATHSIG so even SIGKILL on us
 *      takes it with us, and live_close() escalates -gdb-exit -> SIGTERM ->
 *      SIGKILL rather than trusting any one of them.
 *   2. Nothing blocks forever.  Every read has a deadline; a gdb that stops
 *      answering is an error, never a hang.  The frame loop will one day be
 *      on the other end of this.
 */
#ifndef LIVE_H
#define LIVE_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct Live Live;

#define LIVE_ERRLEN   256
#define LIVE_MAXREG   128       /* scalar registers we will carry back */
#define LIVE_NAMES    512       /* register names gdb may offer at all */
#define LIVE_MAXMOD   96        /* mapped files we will track */
#define LIVE_WAIT_MS  10000     /* a command that takes longer has hung */
/* Waiting for the loader to reach a library is not a command round trip:
   it is the program's own startup, and a GUI program bringing up a window
   and a GL driver under a debugger that stops on every solib event takes
   far longer than LIVE_WAIT_MS.  Treating that as a failure was reported
   for a while as "the program exited", which it had not.            */
#define LIVE_SETUP_MS 30000

typedef struct { char name[16]; uint64_t v; } LiveReg;

/* One mapped file in the inferior.  `bias` is what to add to a file
   address to get a live one -- the load address minus the lowest p_vaddr
   of the file's PT_LOAD segments, which is the load address for a PIE and
   zero for a classic ET_EXEC.                                          */
typedef struct {
    uint64_t lo, hi, bias;
    char     path[256];
} LiveMod;

/* How the session came by its inferior, which decides what cleanup means:
   one we launched is ours to kill, one we attached to must be left running.
   Only LIVE_OWN_KILL exists at L0; attach is L6.                        */
typedef enum { LIVE_OWN_NONE, LIVE_OWN_KILL, LIVE_OWN_DETACH } LiveOwn;

/* Launch `prog` under gdb and stop it before its first instruction.
   `args` is the inferior's own argv[1..], `nargs` long, or NULL.
   Returns NULL and fills `err` on every failure -- gdb missing, file
   missing, not an ELF, exec refused.                                   */
Live *live_launch(const char *prog, const char *const *args, int nargs,
                  char *err, size_t errlen);
/* Attach to a process that is already running (L6).  It is left running
   on close -- killing someone else's job because we looked at it would be
   inexcusable.  Refusal is the common case on a stock desktop: see
   live_ptrace_scope(), and expect the error to be about permission rather
   than about anything this program did.                             */
Live *live_attach(int pid, char *err, size_t errlen);
/* Yama's /proc/sys/kernel/yama/ptrace_scope, or -1 when the file is not
   there (no Yama, so no restriction from it).  0 any process, 1
   descendants only, 2 admin only, 3 nobody.                         */
int   live_ptrace_scope(void);
/* One line saying whether an attach to `pid` can be expected to work, and
   what to do if not.  Written for someone who pressed a key, not for a
   log.                                                              */
void  live_attach_advice(int pid, char *out, size_t n);
/* Detach or kill per LiveOwn, wait for gdb, reap it.  NULL-safe, and safe
   to call twice.                                                       */
void  live_close(Live *L);

const char *live_err(const Live *L);
int      live_alive(const Live *L);     /* the inferior still exists */
/* ...and is stopped, so its registers and memory can be read.  A running
   inferior is alive but has nothing to read: gdb answers "No registers." */
int      live_stopped(const Live *L);
pid_t    live_gdb_pid(const Live *L);
pid_t    live_inferior_pid(const Live *L);

/* The executable's entry point *as loaded* -- relocated for PIE, so it is
   not e_entry.  Only meaningful once the process exists.  0 on failure.
   (The general file<->live map is L1; this is the one address L0 needs.) */
uint64_t live_entry(Live *L);

int      live_run_to(Live *L, uint64_t addr);  /* temporary break, continue */

/* ---- not blocking the frame (L5) -----------------------------------
   A step is bounded and may be waited for; a *continue* is not.  The
   callee might return in a microsecond or block on a read forever, and
   the frame loop cannot be the place that finds out.  So the continue
   path is issued and left running, drained a frame at a time, and always
   escapable.                                                        */

/* Set the breakpoint and let go.  Returns 1 when the inferior is running;
   the stop arrives later, through live_poll().                      */
int      live_continue_async(Live *L, uint64_t addr);
/* Drain whatever gdb has said since the last call, without ever waiting
   for it.  Returns 1 on the frame the inferior comes to rest.       */
int      live_poll(Live *L);
/* a continue is outstanding and the inferior has not stopped yet */
int      live_waiting(const Live *L);
/* Take it back.  The stop arrives through live_poll() like any other, so
   an unbounded wait is always escapable.                            */
int      live_interrupt(Live *L);
int      live_step(Live *L);                   /* one instruction */
uint64_t live_pc(const Live *L);
/* why the inferior last stopped, as MI reported it: "breakpoint-hit",
   "end-stepping-range", "signal-received", "exited-normally", ...      */
const char *live_stop_reason(const Live *L);
const char *live_stop_signal(const Live *L);   /* "" unless a signal */

/* The scalar registers, by gdb's own names.  Vector registers are skipped
   at L0: MI renders them as aggregates ("{v8_bfloat16 = {...}}") and the
   Vm has nowhere to put them until L7.
   Returns how many the stop *had*, snprintf-style, having written at most
   `max` of them -- so a return greater than `max` means truncation and is
   not something a caller should have to guess at.  -1 on error.
   (112 on this machine's x86-64: 16 GPR, rip, eflags, the segment and
   base registers, the x87 stack and its control words, and k0-k7.)     */
int      live_regs(Live *L, LiveReg *out, int max);
/* Restrict what live_regs() asks gdb for, by name.  Without this it asks
   for every register the target has -- 112 scalars on this x86-64 -- and
   throws away the ones nothing can hold.  The flags register is always
   included whether or not it is named.  Call once, before the first
   live_regs(); it is what makes a per-step read affordable.        */
void     live_regs_want(Live *L, const char *const *names, int n);
/* One register by gdb's name for it.  0 when the target has no such
   register, so a caller can try "rsp" then "sp" without knowing the
   architecture.                                                     */
int      live_reg(Live *L, const char *name, uint64_t *out);
int      live_mem(Live *L, uint64_t addr, int n, uint8_t *out);
/* The Vm's fourth memory layer (L7), through a one-page cache dropped
   whenever the process moves.  The signature is Vm.liveread's, so vm.c
   never has to know what a Live is; pass the Live * as ctx.        */
int      live_mem_cb(void *ctx, uint64_t addr, int n, uint8_t *out);
/* Run a gdb console command and return its printed output, malloc'd for
   the caller to free, or NULL.  The MI commands above are the interface;
   this is the escape hatch for the handful of things MI has no record
   for -- and for checking MI's answers against gdb's own (L2).      */
char    *live_console(Live *L, const char *cmd);
/* What lives at a live address, as gdb names it: "malloc" out of
   "malloc + 4 in section .text of /lib/.../libc.so.6".  Empty when gdb
   has no symbol there.  One round trip, so it is for the handful of
   addresses a person will read -- never per step.                  */
void     live_symbol_at(Live *L, uint64_t addr, char *out, size_t n);

/* ---- the address map (L1) ------------------------------------------
   Everything in the city is a *file* address: city_find_addr() compares
   against Sec.addr, straight out of the ELF.  A live PIE process is
   somewhere else entirely, so every address crossing between the two
   worlds goes through here and nowhere else.  In particular the bias does
   not live on the City: city_find_addr() is called with file addresses
   from the disassembly all over the program, and a City that silently
   biased them would break every one of those callers.               */

/* Re-read the inferior's mapped files.  Called for you at launch and after
   anything that could load a library; safe to call at a stop.        */
int      live_map_refresh(Live *L);
int      live_nmods(const Live *L);
const LiveMod *live_mod(const Live *L, int i);
const LiveMod *live_mod_at(const Live *L, uint64_t liveaddr);

/* Name the file the city was built from -- usually the program itself, but
   pointing the city at a library and launching something that loads it is
   the interesting case, and it works the same way.  `lowest_vaddr` is the
   lowest p_vaddr among that file's PT_LOAD segments (0 for a typical PIE,
   0x400000 for a classic non-PIE); the caller has the Elf and live.c
   deliberately does not.  0 and a filled `err` when that file is not
   mapped in this inferior at all.                                    */
int      live_set_subject(Live *L, const char *path, uint64_t lowest_vaddr,
                          char *err, size_t errlen);
/* Let the loader run until `path` is mapped, at most `maxevents` library
   loads.  At the first stop only the loader and the executable are there,
   so a library subject is not yet available to be named -- this is what
   makes "explore a .so, run the program that loads it" work.        */
int      live_await_module(Live *L, const char *path, int maxevents,
                           char *err, size_t errlen);
int      live_have_subject(const Live *L);
uint64_t live_bias(const Live *L);

/* The two crossings.  live_to_file() returns 0 for an address outside the
   subject -- which *is* the off-map test the walker needs (§4), so it is
   one function and not a bool plus an out-parameter.                 */
uint64_t live_to_file(const Live *L, uint64_t live);
uint64_t file_to_live(const Live *L, uint64_t file);

/* L0's pass criterion, headless: `cycles` full launch/step/kill rounds
   plus the abort paths.  Returns 0 on success.                         */
int      live_gdbtest(const char *prog, int cycles, int verbose);

#endif
