/* wisp.c -- the body: which instruction the state is standing on, and how
 * it gets to the next one.
 *
 * The Vm decides what happens; this decides where it is while it happens,
 * and keeps the two things that the Vm deliberately does not: the visit
 * count per instruction (§6's loop policy) and which alcove of a chamber
 * the run is in.
 */
#define _GNU_SOURCE
#include "wisp.h"
#include "live.h"
#include "render.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static const Disasm *unit_dis(const Room *r, int unit){
    if (!r) return NULL;
    if (unit < 0) return r->dis;
    if (!r->units || unit >= r->nunits) return NULL;
    return r->units[unit].dis;
}

/* the bytes behind that decoding, and the address the first of them has */
static const uint8_t *unit_code(const Room *r, int unit, uint64_t *base, uint64_t *len){
    const uint8_t *p; uint64_t a, n;
    if (unit < 0){ p = r->data; a = r->addr ? r->addr : r->fileoff; n = r->datasz; }
    else {
        if (!r->units || unit >= r->nunits) return NULL;
        const Unit *u = &r->units[unit];
        p = u->data; a = u->addr ? u->addr : u->fileoff; n = u->datasz;
    }
    if (!p) return NULL;
    *base = a; *len = n;
    return p;
}

const Disasm *wisp_dis(const Wisp *w, const Room *r){
    return w ? unit_dis(r, w->unit) : NULL;
}

const Insn *wisp_insn(const Wisp *w, const Room *r){
    const Disasm *d = wisp_dis(w, r);
    if (!d || w->cur < 0 || w->cur >= d->n) return NULL;
    return &d->ins[w->cur];
}

static void size_visits(Wisp *w, const Room *r);

static uint16_t *visit_cell(Wisp *w, int unit, int idx){
    int u = unit < 0 ? 0 : unit;
    if (u >= w->nunits || idx < 0) return NULL;
    int at = w->visitOff[u] + idx;
    if (at < 0 || at >= w->nvisits) return NULL;
    return &w->visits[at];
}

/* splitmix64 on the room address: the same room shows the same run every
   time you walk into it, which makes this a property of the code rather
   than a slot machine (§5). */
static uint64_t seed_of(uint64_t a){
    uint64_t z = a + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (z ^ (z >> 31)) | 1ull;
}

/* ------------------------------------------------------------------ */

static void start_at(Wisp *w, const Building *b, const Room *r, uint64_t seed){
    (void)b;
    w->seed = seed;
    w->done = 0; w->t = 1.0f; w->acc = 0; w->posed = 0;
    w->overCall = 0; w->overDepth = 0;
    w->paused = w->stepping;          /* a reseed in step mode stays stepped */
    w->clock = 0; w->ntrail = 0; w->wants = 0; w->asked = 0; w->nrooms = 1;
    memset(w->trail, 0, sizeof w->trail);
    w->kind = ST_FALL; w->decided = 1;
    w->nsteps = w->nports = w->nret = w->nstop = w->nleft = 0;
    w->lastPort = 0;
    w->why[0] = 0;
    if (w->visits) memset(w->visits, 0, (size_t)w->nvisits * sizeof *w->visits);

    /* the alcove being stood at, or the first one that holds code */
    int u = (r->kind == RT_GROUP) ? (r->activeUnit >= 0 ? r->activeUnit : 0) : -1;
    if (u >= 0 && !unit_dis(r, u)){
        u = -1;
        for (int i = 0; i < r->nunits; i++) if (unit_dis(r, i)){ u = i; break; }
        if (u < 0) u = 0;
    }
    w->unit = u; w->prevUnit = u;
    w->cur = w->prev = 0;

    const Disasm *d = unit_dis(r, w->unit);
    /* start at the file's entry point when it happens to be in here */
    if (d && d->n){
        int at = disasm_index_of(d, w->vm.elf ? w->vm.elf->entry : 0);
        if (at > 0) w->cur = w->prev = at;
    }
    uint64_t at = d && d->n ? d->ins[w->cur].addr : 0;
    vm_init(&w->vm, w->vm.elf, at, seed);
    uint16_t *c = visit_cell(w, w->unit, w->cur);
    if (c) (*c)++;
}

Wisp *wisp_spawn(const Building *b, const Room *r, const Elf *e, uint64_t seed){
    if (!r) return NULL;
    int nu = (r->kind == RT_GROUP && r->units) ? r->nunits : 1;
    int total = 0, any = 0;
    for (int i = 0; i < nu; i++){
        const Disasm *d = unit_dis(r, nu == 1 ? -1 : i);
        if (d && d->n) any = 1;
        total += d ? d->n : 0;
    }
    if (!any) return NULL;

    (void)total;
    Wisp *w = calloc(1, sizeof *w);
    size_visits(w, r);
    w->rate = 4.0f;
    w->running = 1;
    w->vm.elf = e;                       /* start_at hands it to vm_init */
    start_at(w, b, r, seed ? seed : seed_of(r->addr ? r->addr : r->fileoff));
    return w;
}

/* ------------------------------------------------------------------ */
/* the live wisp (L2): values read out of a stopped process            */
/* ------------------------------------------------------------------ */

static int stand_at(Wisp *w, const Room *r, uint64_t addr);

Wisp *wisp_spawn_live(const Building *b, const Room *r, const Elf *e,
                      uint64_t fileaddr){
    /* The ordinary spawn builds the body, the visit counters and a Vm with
       this architecture's register file already mapped -- all of which a
       live wisp wants exactly as they are.  What it must not keep is the
       invented state, and wisp_live_sync() overwrites that.          */
    Wisp *w = wisp_spawn(b, r, e, 1);
    if (!w) return NULL;
    w->src = WS_LIVE;
    w->paused = 1;                     /* frozen: the clock never moves it */
    w->stepping = 1;
    w->running = 0;
    if (fileaddr && !stand_at(w, r, fileaddr)){ wisp_free(w); return NULL; }
    return w;
}

/* the six flags the panel prints, at their eflags bit positions */
static const int EFL_BIT[VF_COUNT] = { 0, 2, 4, 6, 7, 11 };

int wisp_live_sync(Wisp *w, const Room *r, Live *L, uint64_t fileaddr){
    if (!w || !r || !L) return 0;
    Vm *m = &w->vm;
    /* L7: reads go to the process now, above the file and above
       invention.  Set every sync, not once, because a rehome or a
       restart can have rebuilt the Vm underneath us.              */
    m->liveread = live_mem_cb;
    m->livectx  = L;
    /* stand_at, not wisp_rehome: this is placement inside the room the
       wisp is already in, and rehome would clear the trail and count
       another room every time the state was refreshed.              */
    if (fileaddr && !stand_at(w, r, fileaddr)) return 0;

    /* Ask gdb only for the registers this architecture's table can hold.
       gdb offers 112 scalars on x86-64 and the Vm has slots for 17 of
       them; fetching the rest is pure cost, and at one read per step it
       is most of the cost there is.                                  */
    if (!w->askedFor){
        const char *names[VM_NREG];
        int nn = 0;
        for (int i = 0; i < m->nreg && nn < VM_NREG; i++)
            if (m->rname[i][0]) names[nn++] = m->rname[i];
        if (nn) live_regs_want(L, names, nn);
        w->askedFor = 1;
    }

    LiveReg reg[LIVE_MAXREG];
    int n = live_regs(L, reg, LIVE_MAXREG);
    if (n < 0) return 0;
    if (n > LIVE_MAXREG) n = LIVE_MAXREG;

    /* Every value the process has, by gdb's name for it.  A register gdb
       knows and the Vm does not is dropped rather than invented a slot
       for: the panel's job is to show this architecture's register file,
       not gdb's view of it.                                         */
    int set = 0;
    for (int i = 0; i < n; i++){
        /* The flags register is read for its bits whether or not this
           architecture's table gives it a slot of its own -- x86-64's does
           not, and without this the panel would show six '?' beside a
           banner claiming everything on it was measured.            */
        if (!strcmp(reg[i].name, "eflags") || !strcmp(reg[i].name, "cpsr")){
            for (int f = 0; f < VF_COUNT; f++)
                m->fl[f] = (uint8_t)((reg[i].v >> EFL_BIT[f]) & 1);
            /* Nothing here is approximated.  The invented VM clears the
               known bit when it is unsure; a real CPU never is.      */
            m->flknown = (uint8_t)((1u << VF_COUNT) - 1);
        }
        int slot = vm_slot_named(m, reg[i].name);
        if (slot < 0) continue;
        vm_slot_set(m, slot, reg[i].v, PV_LIVE);
        set++;
    }
    /* Every register holds what the process holds, verbatim -- rsp points
       into the real stack, and rip is the real rip.  It is tempting to put
       the *file* address in rip instead, since that is the address the city
       speaks, but then the register file would be a mix of two address
       spaces and every value on the panel would need a note saying which
       one it was in.  The translation stays at the boundary (L1): the
       wisp's position in the room comes from the file address, by index,
       and the registers are left alone.                              */
    /* The synthetic stack of §5 is fiction a live wisp does not need, but
       vm_annotate() uses its bounds to say "stack-0x20" rather than a bare
       number.  Point them at the real stack so the annotation keeps
       working and tells the truth.                                  */
    if (m->spSlot >= 0){
        uint64_t sp = vm_slot_get(m, m->spSlot);
        if (sp){
            m->stackLo = (sp - 0x8000) & ~0xfffull;
            m->stackHi = (sp + 0x8000) & ~0xfffull;
        }
    }
    m->steps++;
    return set;
}

void wisp_restart(Wisp *w, const Building *b, const Room *r, uint64_t seed){
    if (!w) return;
    const Elf *e = w->vm.elf;
    vm_free(&w->vm);
    w->vm.elf = e;
    start_at(w, b, r, seed);
    w->running = 1;
}

void wisp_free(Wisp *w){
    if (!w) return;
    vm_free(&w->vm);
    free(w->visits);
    free(w->visitOff);
    free(w);
}

/* ------------------------------------------------------------------ */
/* one instruction                                                     */
/* ------------------------------------------------------------------ */

static void stop(Wisp *w, const char *why){
    w->done = 1; w->nstop++; w->overCall = 0;
    snprintf(w->why, sizeof w->why, "%s", why);
}

void wisp_stop(Wisp *w, const char *why){ if (w) stop(w, why); }

/* how many instruction slots a room needs, and where each alcove starts */
static void size_visits(Wisp *w, const Room *r){
    int nu = (r->kind == RT_GROUP && r->units) ? r->nunits : 1;
    free(w->visitOff);
    w->visitOff = calloc((size_t)(nu ? nu : 1), sizeof *w->visitOff);
    w->nunits = nu;
    int off = 0;
    for (int i = 0; i < nu; i++){
        w->visitOff[i] = off;
        const Disasm *d = unit_dis(r, nu == 1 ? -1 : i);
        off += d ? d->n : 0;
    }
    free(w->visits);
    w->nvisits = off;
    w->visits = calloc((size_t)(off ? off : 1), sizeof *w->visits);
}

int wisp_rehome(Wisp *w, const Room *r, uint64_t addr){
    if (!w || !r) return 0;
    int nu = (r->kind == RT_GROUP && r->units) ? r->nunits : 1;
    int unit = -1, at = -1;
    for (int i = 0; i < nu; i++){
        int u = (nu == 1) ? -1 : i;
        const Disasm *d = unit_dis(r, u);
        if (!d) continue;
        int k = disasm_index_of(d, addr);
        if (k >= 0){ unit = u; at = k; break; }
    }
    if (at < 0) return 0;
    size_visits(w, r);
    w->unit = w->prevUnit = unit;
    w->cur  = w->prev = at;
    w->t = 1.0f; w->acc = 0; w->posed = 0;
    w->kind = ST_FALL; w->decided = 1;
    w->wants = 0; w->asked = 0;
    w->nrooms++;
    /* the trail indexes the decoding we just left */
    w->ntrail = 0;
    memset(w->trail, 0, sizeof w->trail);
    uint16_t *c = visit_cell(w, w->unit, w->cur);
    if (c) (*c)++;
    return 1;
}

/* The ghost card for the step just taken: the mnemonic, and the one
   register it moved -- which is the whole of what a trail entry is.
   Shared by the simulated and the live step on purpose: the trail, the
   visit counts and the hop animation are the *body*, and the only thing
   that differs between the two is where the values came from.       */
static void push_trail(Wisp *w, const Insn *in, StepKind k, int decided,
                       uint32_t before){
    struct WTrail *e = &w->trail[w->ntrail % WISP_TRAIL];
    e->unit = w->unit; e->idx = w->cur; e->at = w->clock;
    e->kind = (uint8_t)k; e->decided = (uint8_t)decided;
    const Vm *m = &w->vm;
    int moved = -1;
    for (int i = 0; i < m->nreg; i++)
        if (i != m->pcSlot && m->r[i].prov != PV_NONE && m->r[i].stamp > before){
            moved = i; break;
        }
    char rn[8] = "";                /* copied out: e->note lives in *w too */
    if (moved >= 0) memcpy(rn, m->rname[moved], sizeof rn - 1);
    if (moved >= 0)
        snprintf(e->note, sizeof e->note, "%s  %s=%llx", in->mnem, rn,
                 (unsigned long long)m->r[moved].v);
    else
        snprintf(e->note, sizeof e->note, "%s %.12s", in->mnem, in->ops);
    w->ntrail++;
}

void wisp_step_once(Wisp *w, const Room *r){
    if (!w || w->done || w->wants) return;
    w->vm.follow = w->pilot;
    const Disasm *d = unit_dis(r, w->unit);
    if (!d || w->cur < 0 || w->cur >= d->n){ stop(w, "nothing here to run"); return; }

    uint64_t base = 0, len = 0;
    const uint8_t *code = unit_code(r, w->unit, &base, &len);
    const Insn *in = &d->ins[w->cur];
    const uint8_t *bytes = NULL;
    if (code && in->addr >= base && in->addr + in->len <= base + len)
        bytes = code + (in->addr - base);

    uint16_t *vc = visit_cell(w, w->unit, w->cur);
    int visits = vc ? *vc : 0;

    uint64_t next = 0; int decided = 1;
    uint32_t before = w->vm.steps;
    StepKind k = vm_step(&w->vm, in, bytes, visits, &next, &decided);
    w->kind = (uint8_t)k;
    w->decided = decided;
    w->nsteps++;

    /* The call being stepped over has returned -- the depth is back where it
       was when you pressed the key.  Whatever else this step does (it is
       usually a `ret` crossing back into the room you are standing in, so
       it also sets `wants`), the wisp is yours again.               */
    if (w->overCall && w->vm.ncall <= w->overDepth){
        w->overCall = 0;
        w->paused = 1;
        w->acc = 0;
    }

    push_trail(w, in, k, decided, before);

    w->prev = w->cur; w->prevUnit = w->unit; w->t = 0.0f;

    switch (k){
    case ST_RET:
        w->nret++;
        stop(w, "ret -- the run is over");
        return;
    case ST_EXIT_PORT:
        w->nports++;
        w->lastPort = next;
        if (w->pilot){ w->wants = next; return; }     /* the app follows */
        snprintf(w->why, sizeof w->why, "left the room for 0x%llx",
                 (unsigned long long)next);
        w->done = 1;
        return;
    case ST_STOP:
        stop(w, in->taddr ? "stopped" : "indirect branch, target unknown");
        return;
    case ST_CALL_INTO:
    case ST_TAKEN:
        if (in->tunit >= 0 && in->target >= 0){        /* another alcove */
            const Disasm *od = unit_dis(r, in->tunit);
            if (!od || in->target >= od->n){ stop(w, "the target alcove is not decoded"); return; }
            w->unit = in->tunit; w->cur = in->target;
            break;
        }
        if (in->target >= 0){ w->cur = in->target; break; }
        /* fall through to the address lookup */
        /* FALLTHROUGH */
    default: {
        int at = disasm_index_of(d, next);
        if (at < 0){
            /* Not a stall: control really did go somewhere else -- a return
               to a caller that is not in this room, or an indirect branch
               §9's relocations resolved to another function.  Same outcome
               as leaving by a port, and counted with them.            */
            w->nleft++;
            w->lastPort = next;
            if (w->pilot){ w->wants = next; return; }  /* the app follows */
            snprintf(w->why, sizeof w->why, "left the room for 0x%llx",
                     (unsigned long long)next);
            w->done = 1;
            return;
        }
        w->cur = at;
        break;
    }
    }

    if (vm_spent(&w->vm)){ stop(w, "out of fuel"); return; }
    vc = visit_cell(w, w->unit, w->cur);
    if (vc && *vc < 0xffff) (*vc)++;
}

/* ------------------------------------------------------------------ */
/* step by step                                                        */
/* ------------------------------------------------------------------ */

/* Step over.  One instruction -- except when that instruction is a call the
   VM really makes, and then one instruction is not an answer: the honest
   step-over runs the callee.  So the wisp is let go at its own rate, with
   the app keeping the player where they are, and step mode comes back the
   moment the call depth returns to what it was (wisp_step_once does that
   check).  A call the VM declines to follow -- a stub, the depth limit,
   §6's contract -- never raises the depth, so it is over at once.    */
/* Stand on an instruction of the room we are already in.  Not
   wisp_rehome(): that is for a *room change* -- it clears the trail,
   which indexes the decoding being left, and counts another room.  Doing
   that once per step gives a wisp with a one-card history that claims to
   have walked ten thousand rooms.                                    */
static int stand_at(Wisp *w, const Room *r, uint64_t addr){
    int nu = (r->kind == RT_GROUP && r->units) ? r->nunits : 1;
    for (int i = 0; i < nu; i++){
        int u = (nu == 1) ? -1 : i;
        const Disasm *d = unit_dis(r, u);
        if (!d) continue;
        int k = disasm_index_of(d, addr);
        if (k < 0) continue;
        w->unit = u;
        w->cur = k;
        uint16_t *c = visit_cell(w, w->unit, w->cur);
        if (c) (*c)++;
        return 1;
    }
    return 0;
}

/* Finish a step once the CPU has come to rest: work out what kind of step
   it was, take the registers, push the card for the instruction just
   executed, and move.  Shared by the immediate path and the one that had
   to wait a few frames for a call to come back.                     */
static int finish_live_step(Wisp *w, const Room *r, Live *L, const Insn *in,
                            uint8_t cls, uint64_t wasend, uint32_t before,
                            int wentOver, uint64_t *offmap){
    uint64_t lpc = live_pc(L), fpc = live_to_file(L, lpc);

    /* What kind of step it was, read off the addresses rather than worked
       out.  A live wisp never guesses, so `decided` is always 1 -- on
       every architecture, including the ones the simulated wisp can only
       havoc (§7).                                                     */
    StepKind k;
    if (!fpc)                     k = ST_EXIT_PORT;
    else if (wentOver)            k = ST_CALL_OVER;
    else if (fpc == wasend)       k = (cls == IC_CJUMP) ? ST_NOT_TAKEN : ST_FALL;
    else if (cls == IC_RET)       k = ST_RET;
    else if (cls == IC_CALL)      k = ST_CALL_INTO;
    else                          k = ST_TAKEN;

    wisp_live_sync(w, r, L, 0);       /* registers first, as step_once does */
    push_trail(w, in, k, 1, before);  /* ... then the card for `in` */
    w->kind = (uint8_t)k;
    w->decided = 1;
    w->nsteps++;
    w->prev = w->cur; w->prevUnit = w->unit; w->t = 0.0f;

    if (!fpc || !stand_at(w, r, fpc)){
        w->nports++;
        w->lastPort = fpc ? fpc : lpc;
        if (fpc){
            /* Still in the file, but in another room -- a call to a
               function the packer put elsewhere, which is the ordinary
               case and not an excursion at all.  Ask to be taken there,
               exactly as the simulated wisp does when it steps through a
               port: the caller moves the player and the decoding, so the
               one-room-at-a-time invariant is never in question.     */
            w->wants = fpc;
            return 0;
        }
        /* §4's C1.  The code left the file with nowhere to come back to:
           a tail call, a jump through the GOT, a `ret` out of the bottom.
           Not an error and not a guess -- the run parks, and says where
           it went.                                                   */
        if (offmap) *offmap = lpc;
        if (!w->wentTo[0]) live_symbol_at(L, lpc, w->wentTo, sizeof w->wentTo);
        w->nparked++;
        stop(w, "left the file");
        return 0;
    }
    return 1;
}

/* Where a call that left the file will come back to, or 0 when nothing
   will.  §4's C2.                                                    */
static uint64_t return_address(Wisp *w, Live *L, uint8_t cls, uint64_t wasend){
    if (cls == IC_CALL) return file_to_live(L, wasend);
    /* Not a call -- and this is the *common* case, not the corner one.  A
       library call goes `call snprintf@plt`, which lands in the file's own
       .plt, and the stub then does `jmp *GOT`.  The instruction that
       actually leaves is a jump, so there is no next-instruction to come
       back to.  But the call that got us into the stub pushed a return
       address and the jump did not touch it, so the top of the stack is
       exactly where the callee will come back to.                   */
    uint64_t sp = 0;
    if (!live_reg(L, "rsp", &sp) && !live_reg(L, "esp", &sp) &&
        !live_reg(L, "sp", &sp)) return 0;
    uint8_t b[8];
    int nb = w->vm.elf && w->vm.elf->is64 ? 8 : 4;
    if (!sp || !live_mem(L, sp, nb, b)) return 0;
    uint64_t cand = 0;
    for (int i = nb - 1; i >= 0; i--) cand = (cand << 8) | b[i];
    return (cand && live_to_file(L, cand)) ? cand : 0;
}

/* One real instruction.  There is no arithmetic here at all: the CPU
   executed it, and all this does is read where it ended up and what
   changed.
   Returns 1 stepped, 0 stopped or parked, and 2 when a call out of the
   file was let go of and the answer will arrive in some later frame --
   see wisp_live_poll().                                             */
int wisp_step_live(Wisp *w, const Room *r, Live *L, uint64_t *offmap){
    if (offmap) *offmap = 0;
    if (!w || !r || !L || w->done || w->waiting) return 0;
    const Disasm *d = unit_dis(r, w->unit);
    if (!d || w->cur < 0 || w->cur >= d->n){ stop(w, "nothing here to run"); return 0; }

    const Insn *in = &d->ins[w->cur];
    uint8_t cls = in->cls;
    uint64_t wasend = in->addr + in->len;
    uint32_t before = w->vm.steps;

    if (!live_step(L)){
        stop(w, live_alive(L) ? "gdb would not step" : "the process ended");
        return 0;
    }
    int wentOver = 0;
    if (!live_to_file(L, live_pc(L)) && !w->naive){
        uint64_t back = return_address(w, L, cls, wasend);
        w->wentTo[0] = 0;               /* a new excursion, a new answer */
        if (back){
            live_symbol_at(L, live_pc(L), w->wentTo, sizeof w->wentTo);
            if (w->async){
                /* The callee might return in a microsecond or block on a
                   read forever, and the frame loop must not be the place
                   that finds out.  Let it go and come back to it.    */
                if (live_continue_async(L, back)){
                    w->waiting = 1;
                    w->pendUnit = w->unit; w->pendIdx = w->cur;
                    w->pendCls = cls; w->pendEnd = wasend; w->pendBefore = before;
                    return 2;
                }
            } else if (live_run_to(L, back)){
                wentOver = 1;
                w->nover++;
            }
        }
    }
    return finish_live_step(w, r, L, in, cls, wasend, before, wentOver, offmap);
}

/* Called every frame while the wisp is waiting for a call to come back.
   1 when the step finished this frame, 0 while it is still out there. */
int wisp_live_poll(Wisp *w, const Room *r, Live *L, uint64_t *offmap){
    if (offmap) *offmap = 0;
    if (!w || !r || !L || !w->waiting) return 0;
    if (!live_poll(L)) return 0;                 /* still running */
    w->waiting = 0;
    const Disasm *d = unit_dis(r, w->pendUnit);
    if (!d || w->pendIdx < 0 || w->pendIdx >= d->n){
        stop(w, "the room went away while a call was out");
        return 0;
    }
    if (!live_alive(L)){
        /* the call never came back -- the program ended inside it, which
           is what `call __libc_start_main` does every time */
        w->nparked++;
        stop(w, "left the file for good");
        if (offmap) *offmap = live_pc(L);
        return 0;
    }
    w->nover++;
    finish_live_step(w, r, L, &d->ins[w->pendIdx], w->pendCls, w->pendEnd,
                     w->pendBefore, 1, offmap);
    return 1;
}

/* Let a live wisp run at its own rate.  The clock only moves it when it is
   not already waiting on something.                                  */
void wisp_tick_live(Wisp *w, const Room *r, Live *L, float dt){
    if (!w || !r || !L || w->done) return;
    w->clock += dt;
    if (w->t < 1.0f) w->t += dt * w->rate * 1.6f;
    if (w->t > 1.0f) w->t = 1.0f;
    if (w->waiting){ wisp_live_poll(w, r, L, NULL); return; }
    if (w->paused || w->stepping) return;
    w->acc += dt;
    float per = 1.0f / (w->rate > 0.01f ? w->rate : 0.01f);
    /* One step a frame at most.  The rate is at most 20/s and a frame is
       16 ms, so this never throttles anything a person asked for -- it
       stops a long dt (a stall, a breakpoint, a window drag) from turning
       into a burst of gdb round trips inside one frame.             */
    if (w->acc >= per){
        w->acc -= per;
        if (w->acc > per) w->acc = per;
        uint64_t gone = 0;
        int rc = wisp_step_live(w, r, L, &gone);
        if (rc == 1) w->paused = 0;        /* keep going */
    }
}

void wisp_step_over(Wisp *w, const Room *r){
    if (!w || w->done) return;
    w->stepping = 1;
    w->running = 1;
    if (w->overCall) return;             /* one is already running */
    w->paused = 1;
    w->acc = 0;
    int depth = w->vm.ncall;
    wisp_step_once(w, r);
    if (!w->done && w->vm.ncall > depth){
        w->overCall = 1;
        w->overDepth = depth;
        w->paused = 0;
    }
}

/* Step into.  The same one instruction, but the call is followed and the
   wisp stops on the callee's first instruction.  Following a call means
   another room may have to be decoded, which is pilot mode's whole job --
   so this turns it on rather than pretending it can do without.      */
void wisp_step_into(Wisp *w, const Room *r){
    if (!w || w->done) return;
    w->stepping = 1;
    w->running = 1;
    if (w->overCall) return;
    w->pilot = 1;
    w->vm.follow = 1;
    w->paused = 1;
    w->acc = 0;
    wisp_step_once(w, r);
}

int wisp_decline(Wisp *w, const Room *r){
    if (!w || !w->wants) return 0;
    const Insn *in = wisp_insn(w, r);
    const Disasm *d = unit_dis(r, w->unit);
    w->wants = 0; w->asked = 0;
    if (!in || in->cls != IC_CALL || !d) return 0;    /* nowhere else to go */

    /* Unmake the call: the return address it pushed comes back off, and the
       contract of §6 goes on instead -- rax is whatever the callee is
       pretending to have returned, the caller-saved registers are gone. */
    Vm *m = &w->vm;
    if (m->spSlot >= 0){
        int bw = m->nreg > 8 ? 8 : 4;
        vm_slot_set(m, m->spSlot, vm_slot_get(m, m->spSlot) + (uint64_t)bw, PV_DERIVED);
    }
    if (m->ncall > 0) m->ncall--;
    vmx86_call_over(m, in, vm_callee_name(m, in->taddr ? in->taddr : w->lastPort));

    int at = disasm_index_of(d, in->addr + in->len);
    if (at < 0){ stop(w, "the call was stepped over, and the room ends here"); return 1; }
    w->prev = w->cur; w->prevUnit = w->unit; w->t = 0.0f;
    w->cur = at;
    w->kind = ST_CALL_OVER;
    uint16_t *c = visit_cell(w, w->unit, w->cur);
    if (c && *c < 0xffff) (*c)++;
    return 1;
}

void wisp_tick(Wisp *w, const Room *r, float dt){
    if (!w || !w->running) return;
    w->clock += dt;
    {   /* The ceiling panel eases after the body rather than teleporting.
           Until the room has been drawn once the sculptures have no
           coordinates, so there is nothing to ease towards yet -- but that
           is a reason to skip the easing, not to stop running.  A room the
           player is not standing in is never laid out, and this used to
           freeze a wisp the moment it went on ahead.                  */
        const Disasm *d = unit_dis(r, w->unit);
        if (d && d->laid){
            float pos[3]; wisp_pos(w, r, pos);
            if (!w->posed){ w->px = pos[0]; w->pz = pos[2]; w->posed = 1; }
            float k = dt * 3.4f; if (k > 1.0f) k = 1.0f;
            w->px += (pos[0] - w->px) * k;
            w->pz += (pos[2] - w->pz) * k;
        }
    }
    /* the hop itself always animates, even while stepping by hand */
    if (w->t < 1.0f){
        float hop = w->rate > 0.01f ? w->rate : 1.0f;
        w->t += dt * hop;
        if (w->t > 1.0f) w->t = 1.0f;
    }
    if (w->paused || w->done || w->wants) return;
    w->acc += dt * w->rate;
    int guard = 0;
    while (w->acc >= 1.0f && !w->done && !w->wants && !w->paused && guard++ < 64){
        w->acc -= 1.0f;
        wisp_step_once(w, r);
    }
    if (w->acc > 4.0f) w->acc = 0;        /* a long stall must not fast-forward */
}

/* ------------------------------------------------------------------ */
/* where the body is                                                   */
/* ------------------------------------------------------------------ */

void wisp_pos(const Wisp *w, const Room *r, float out[3]){
    out[0] = out[1] = out[2] = 0;
    if (!w) return;
    const Disasm *dc = unit_dis(r, w->unit), *dp = unit_dis(r, w->prevUnit);
    if (!dc || w->cur < 0 || w->cur >= dc->n) return;
    const Insn *c = &dc->ins[w->cur];
    float B[3] = { c->x, c->y + c->h + 0.16f, c->z };
    if (!dp || w->prev < 0 || w->prev >= dp->n || w->t >= 1.0f){
        memcpy(out, B, sizeof B);
        return;
    }
    const Insn *p = &dp->ins[w->prev];
    float A[3] = { p->x, p->y + p->h + 0.16f, p->z };
    /* a taken branch rides the wire that is already drawn for it */
    float lift = (w->kind == ST_TAKEN) ? arc_lift(A, B) : 0.10f;
    arc_point(A, B, lift, w->t, out);
}
