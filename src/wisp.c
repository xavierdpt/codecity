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

    /* the ghost card for the step just taken: the mnemonic, and the one
       register it moved -- which is the whole of what a trail entry is */
    {
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
