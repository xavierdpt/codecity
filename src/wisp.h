/* wisp.h -- the animated body that carries a Vm around a code room
 *
 * One per room, spawned and freed by room_lifecycle() so it obeys the same
 * discipline as the decoding it walks over.  It owns nothing belonging to
 * the Disasm -- only indices into it -- which is what will let the same
 * state survive a room change when pilot mode lands (§8 B2).
 */
#ifndef WISP_H
#define WISP_H
#include "vm.h"
#include "model.h"

#define WISP_RATE_MIN  0.5f
#define WISP_RATE_MAX  20.0f
#define WISP_TRAIL     16       /* §7: sixteen is plenty */
#define WISP_TRAIL_S   6.0f     /* how long a ghost card takes to fade */

typedef struct Wisp {
    Vm        vm;
    int       unit;             /* chamber alcove, -1 when the room is its own */
    int       cur, prev;        /* instruction indices in that alcove's Disasm */
    int       prevUnit;
    float     t;                /* 0..1 along the hop from prev to cur */
    float     rate;             /* instructions per second */
    float     acc;              /* seconds carried over between steps */

    int       running, paused, done;
    char      why[96];          /* why it stopped, for the panel */

    /* Pilot mode (§8 B2): the wisp leads and the player goes with it.  When
       a step wants to leave this room the wisp parks, and the app follows
       it through -- teleport_addr() does exactly what stepping through a
       port by hand does, so the one-decoding-at-a-time invariant is never
       in question.  The state survives the move because it owns nothing
       belonging to a Disasm.                                         */
    int       pilot;
    /* Step-by-step.  `stepping` means the clock never moves it: it goes one
       instruction at a time, when you say so.  A step *over* a call that is
       really made cannot be one instruction, so it becomes `overCall`: the
       wisp runs the callee on its own, at its own rate, while you stay at
       the call site, and stops again the moment the call depth comes back
       to `overDepth` -- which is the instruction after the call.       */
    int       stepping;
    int       overCall;
    int       overDepth;
    uint64_t  wants;            /* the address it wants to be taken to, or 0 */
    int       nrooms;           /* rooms this run has walked through */
    int       asked;            /* the door is open and it is waiting for you */

    uint8_t   kind;             /* StepKind of the hop in progress */
    int       decided;          /* 0 when the branch that got here was guessed */
    uint64_t  seed;

    uint16_t *visits;           /* per instruction, for §6's loop policy */
    int       nvisits, nunits;
    int      *visitOff;         /* where each alcove's counters start */

    /* the panel does not teleport: it eases after the body (§7) */
    float     px, pz;
    int       posed;
    float     clock;            /* seconds since the run started */

    /* §7's trail: the last few states, as ghost cards over the sculptures
       they belonged to.  What makes the room read as a timeline you can
       walk under and look back along, rather than a single moving dot. */
    struct WTrail {
        int      unit, idx;
        float    at;            /* w->clock when it happened */
        uint8_t  kind, decided;
        char     note[44];      /* the mnemonic, and the one register it moved */
    } trail[WISP_TRAIL];
    int       ntrail;           /* total pushed; index = n % WISP_TRAIL */

    /* what the run did, for the HUD and for --vmtest */
    int       nsteps, nports, nret, nstop, nleft;
    uint64_t  lastPort;
} Wisp;

/* Spawn in a room (a chamber starts in the alcove being stood at, or 0).
   Returns NULL when the room holds no code.  seed 0 = derive from the
   room address, which is what makes a room show the same run twice.   */
Wisp *wisp_spawn(const Building *b, const Room *r, const Elf *e, uint64_t seed);
void  wisp_free(Wisp *w);
/* advance by dt seconds; steps as many instructions as the rate allows */
void  wisp_tick(Wisp *w, const Room *r, float dt);
/* one instruction, ignoring the clock -- the single-step key, and --vmtest */
void  wisp_step_once(Wisp *w, const Room *r);
/* Step-by-step, the two halves of it.  Both put the wisp in step mode and
   leave it paused when they are done.  `over` takes one instruction and,
   when that instruction really made a call, hands the wisp its head until
   the call returns (§6's step-over, made honest: the callee is actually
   run, not contracted away).  `into` makes the call and stops on its first
   instruction -- which needs pilot mode, so it turns it on.          */
void  wisp_step_over(Wisp *w, const Room *r);
void  wisp_step_into(Wisp *w, const Room *r);
void  wisp_restart(Wisp *w, const Building *b, const Room *r, uint64_t seed);
/* Carry a run into another room, keeping the CPU state.  The trail and the
   visit counts index the old decoding and are reset; nothing in the Vm is.
   Returns 0 when the address is not in the new room after all.       */
int   wisp_rehome(Wisp *w, const Room *r, uint64_t addr);
void  wisp_stop(Wisp *w, const char *why);
/* Decline a call the wisp asked to follow: unmake it, apply the step-over
   contract instead, and carry on in this room -- §8 B1's behaviour, chosen
   one call at a time.  Returns 0 when there was nothing to decline (a
   return or a tail call has nowhere else to go, so the run ends).    */
int   wisp_decline(Wisp *w, const Room *r);
/* the decoding the wisp is walking, which is not always the one the
   player is standing at */
const Disasm *wisp_dis(const Wisp *w, const Room *r);
const Insn   *wisp_insn(const Wisp *w, const Room *r);
/* where the body is right now: eased between the previous sculpture and
   the current one, along the same arc the branch wire is drawn with   */
void  wisp_pos(const Wisp *w, const Room *r, float out[3]);

#endif
