/* main.c -- Code City: walk around the inside of a binary */
#define _GNU_SOURCE
#include "app.h"
#include "disasm.h"
#include "render.h"
#include "wisp.h"
#include "wisp.h"
#include "text.h"
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static App g_app;

void app_message(App *a, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    vsnprintf(a->msg, sizeof a->msg, fmt, ap);
    va_end(ap);
    a->msgT = 4.0f;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static void spawn_plaza(App *a){
    a->p.x = 0; a->p.y = 0;
    a->p.z = a->city ? -(a->city->plazaR + 26.0f) : -40.0f;
    a->p.yaw = (float)M_PI * 0.5f; a->p.pitch = -0.06f;
    a->p.vy = 0; a->p.inside = -1; a->p.floor = 0; a->p.room = -1;
}

static void wisp_forget(App *a);

static int load_file(App *a, const char *path){
    char err[256] = "";
    Elf *e = elf_open(path, err, sizeof err);
    if (!e){ app_message(a, "%s: %s", path, err[0] ? err : "cannot read"); return 0; }
    City *c = city_build(e);
    wisp_forget(a);                       /* it indexes the old city's rooms */
    if (a->city){ render_set_city(NULL); city_free(a->city); }
    disasm_close();
    if (a->elf) elf_close(a->elf);
    a->elf = e; a->city = c;
    a->lastB = a->lastR = a->lastU = -2; a->nearIns = -1;
    if (!disasm_open(e->machine, e->is64, e->be))
        fprintf(stderr, "note: no disassembler for %s; code rooms show raw bytes\n",
                elf_machine_name(e->machine));
    render_set_city(c);
    spawn_plaza(a);
    long long rooms = 0;
    for (int i = 0; i < c->nbld; i++) rooms += c->bld[i].nrooms;
    app_message(a, "%s -- %d towers, %lld rooms, %d links", e->base, c->nbld, rooms, e->nneeded);
    return 1;
}

/* ------------------------------------------------------------------ */
/* file browser                                                        */
/* ------------------------------------------------------------------ */

static int is_elf(const char *p){
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char m[4];
    int n = (int)read(fd, m, 4);
    close(fd);
    return n == 4 && !memcmp(m, "\177ELF", 4);
}

static int cmp_str(const void *a, const void *b){
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int dx = x[strlen(x)-1] == '/', dy = y[strlen(y)-1] == '/';
    if (dx != dy) return dy - dx;
    return strcmp(x, y);
}

static void browser_filter(App *a){
    a->nbfiltered = 0;
    for (int i = 0; i < a->nbent; i++){
        if (!a->bfilter[0] || strcasestr(a->bent[i], a->bfilter))
            a->bfiltered[a->nbfiltered++] = i;
    }
    if (a->bsel >= a->nbfiltered) a->bsel = a->nbfiltered ? a->nbfiltered - 1 : 0;
    if (a->bsel < 0) a->bsel = 0;
}

static void browser_scan(App *a, const char *dir){
    for (int i = 0; i < a->nbent; i++) free(a->bent[i]);
    a->nbent = 0; a->bsel = 0; a->bfilter[0] = 0;
    snprintf(a->bdir, sizeof a->bdir, "%s", dir);
    DIR *d = opendir(dir);
    if (!d){ app_message(a, "cannot open %s", dir); return; }
    if (strcmp(dir, "/")) a->bent[a->nbent++] = strdup("../");
    struct dirent *de;
    char full[1024];
    while ((de = readdir(d)) && a->nbent < BR_MAX - 1){
        if (de->d_name[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat sb;
        if (stat(full, &sb)) continue;
        if (S_ISDIR(sb.st_mode)){
            char b[512]; snprintf(b, sizeof b, "%s/", de->d_name);
            a->bent[a->nbent++] = strdup(b);
        } else if (S_ISREG(sb.st_mode) && sb.st_size > 64 && is_elf(full)){
            a->bent[a->nbent++] = strdup(de->d_name);
        }
    }
    closedir(d);
    qsort(a->bent, (size_t)a->nbent, sizeof(char *), cmp_str);
    if (!a->bfiltered) a->bfiltered = malloc(BR_MAX * sizeof(int));
    browser_filter(a);
    app_message(a, "%s -- %d entries", dir, a->nbent);
}

static void browser_open(App *a){
    if (!a->nbfiltered) return;
    const char *name = a->bent[a->bfiltered[a->bsel]];
    char path[1200];
    size_t l = strlen(name);
    if (l && name[l-1] == '/'){
        if (!strcmp(name, "../")){
            char up[512]; snprintf(up, sizeof up, "%s", a->bdir);
            char *s = strrchr(up, '/');
            if (s && s != up) *s = 0; else strcpy(up, "/");
            browser_scan(a, up);
        } else {
            char sub[900]; snprintf(sub, sizeof sub, "%.400s/%.400s", a->bdir, name);
            sub[strlen(sub)-1] = 0;
            browser_scan(a, sub);
        }
        return;
    }
    snprintf(path, sizeof path, "%s/%s", a->bdir, name);
    if (load_file(a, path)) a->showBrowser = 0;
}


/* ------------------------------------------------------------------ */
/* decode the room we walk into, throw it away when we walk out        */
/* ------------------------------------------------------------------ */

static void room_lifecycle(App *a){
    int bi = a->p.inside, ri = a->p.room;
    /* The whole room is decoded on the way in -- a chamber included, so all
       seven of its alcoves are lit at once.  The alcove being stood at only
       decides which sculpture is close enough to read.                   */
    if (bi != a->lastB || ri != a->lastR){
        /* Walking out of the room no longer ends the run: if the wisp is in
           the room being left, its decoding is claimed by the second slot
           and it carries on without you (§8 B3).                     */
        if (a->wisp && !a->keepWisp && a->lastB >= 0 && a->lastR >= 0 &&
            a->wispBi == a->lastB && a->wispRi == a->lastR)
            city_enter_wisp_room(a->city, a->wispBi, a->wispRi);
        if (bi >= 0 && ri >= 0) city_enter_room(a->city, bi, ri);
        else                    city_leave_room(a->city);
        a->lastB = bi; a->lastR = ri; a->lastU = -1;
        a->nearIns = -1;
    }
    if (a->wisp)
        a->wispAway = !(a->wispBi == bi && a->wispRi == ri);
    a->nearIns = -1;
    if (bi < 0 || ri < 0) return;
    Room *r = &a->city->bld[bi].rooms[ri];
    if (r->kind == RT_GROUP)
        r->activeUnit = room_unit_at(&a->city->bld[bi], r, a->p.x, a->p.z);
    a->lastU = r->activeUnit;

    /* which sculpture are we standing next to? */
    Disasm *d = room_dis(r);
    if (d && d->laid){
        float best = 2.6f * 2.6f;
        for (int i = 0; i < d->n; i++){
            float dx = d->ins[i].x - a->p.x, dz = d->ins[i].z - a->p.z;
            float d2 = dx*dx + dz*dz;
            if (d2 < best){ best = d2; a->nearIns = i; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* going somewhere                                                     */
/* ------------------------------------------------------------------ */

/* stand in the middle of a room, looking at its far wall */
static void stand_in_room(App *a, int bi, int ri){
    Building *b = &a->city->bld[bi];
    Room *r = &b->rooms[ri];
    floor_realize(b, r->floor);
    float mid = (room_x0(b, r) + room_x1(b, r)) * 0.5f;
    float depth = room_z1(b, r) - room_z0(b, r);
    a->p.x = mid;
    a->p.y = r->floor * FLOOR_H;
    a->p.z = room_z0(b, r) + (depth < 2.4f ? depth * 0.5f : 1.2f);
    a->p.yaw = (float)M_PI * 0.5f;                 /* looking into the room, +Z */
    a->p.pitch = 0.02f;
    a->p.vy = 0;
    a->p.inside = bi; a->p.floor = r->floor; a->p.room = ri;
}

/* step through a port: land in the room that holds the address it names */
static void teleport_addr(App *a, uint64_t addr, const char *what){
    int bi, ri, ui;
    if (!city_find_addr(a->city, addr, &bi, &ri, &ui)){
        app_message(a, "0x%llx is not in this file", (unsigned long long)addr);
        return;
    }
    Building *b = &a->city->bld[bi];
    Room *r = &b->rooms[ri];
    stand_in_room(a, bi, ri);
    if (ui >= 0){                     /* a chamber: stand at that alcove */
        float x0, z0, x1, z1;
        if (room_cell_rect(b, r, ui, &x0, &z0, &x1, &z1)){
            a->p.x = (x0 + x1) * 0.5f;
            a->p.z = z0 - 0.6f > room_z0(b, r) ? z0 - 0.6f : (z0 + z1) * 0.5f;
        }
    }
    a->lastB = a->lastR = -2;         /* force the new room to be decoded */
    room_lifecycle(a);
    app_message(a, "%s  --  %s / %s / floor %d", what ? what : r->title,
                b->label, r->title, r->floor + 1);
}

/* ------------------------------------------------------------------ */
/* the wisp                                                            */
/* ------------------------------------------------------------------ */

/* The room the player is standing in, if it holds code at all.  Not
   room_dis(): that is the alcove being stood at, which goes NULL as soon as
   you walk into the middle of a chamber, and the wisp in the alcove you
   started it in should keep taking its keys.                         */
static Room *code_room_here(App *a){
    if (a->p.inside < 0 || a->p.room < 0) return NULL;
    Room *r = &a->city->bld[a->p.inside].rooms[a->p.room];
    if (r->dis) return r;
    for (int u = 0; r->units && u < r->nunits; u++) if (r->units[u].dis) return r;
    return NULL;
}

static void wisp_toggle(App *a){
    if (a->wisp){
        wisp_forget(a);
        app_message(a, "wisp gone");
        return;
    }
    Room *r = code_room_here(a);
    if (!r){ app_message(a, "no code in this room to run"); return; }
    Building *b = &a->city->bld[a->p.inside];
    a->wisp = wisp_spawn(b, r, a->elf, 0);
    if (!a->wisp){ app_message(a, "nothing here to run"); return; }
    a->wispBi = a->p.inside; a->wispRi = a->p.room; a->wispAway = 0;
    const Insn *in = wisp_insn(a->wisp, r);
    app_message(a, "a wisp starts at 0x%llx  --  k pause, . step, , / speed, n reseed",
                (unsigned long long)(in ? in->addr : 0));
}

/* Pilot mode: the wisp leads and the player goes with it.  Following a port
   is exactly what pressing E on it does, so the single-decoding invariant is
   never in question -- the player really did walk through.           */
static int wisp_move(App *a, int bring);

static void pilot_follow(App *a){ wisp_move(a, 1); }

/* Go and stand where the wisp is */
static void wisp_catchup(App *a);

/* The room the wisp is in, which is not always the room you are in */
static Room *wisp_room(App *a){
    if (!a->wisp) return NULL;
    if (a->wispBi < 0 || a->wispBi >= a->city->nbld) return NULL;
    Building *b = &a->city->bld[a->wispBi];
    if (a->wispRi < 0 || a->wispRi >= b->nrooms) return NULL;
    return &b->rooms[a->wispRi];
}

static void wisp_forget(App *a){
    wisp_free(a->wisp); a->wisp = NULL;
    city_leave_wisp_room(a->city);
    a->wispBi = a->wispRi = -1;
    a->wispAway = 0;
    a->wispDoorBi = a->wispDoorRi = -1; a->wispDoorAddr = 0;
    a->wispAsk[0] = 0; a->wispAskKind = 0;
}

/* Take the wisp through the door it is standing at.  `bring` is whether you
   go too: if you do, this is exactly stepping through a port by hand and
   the city keeps one room decoded; if you do not, the wisp's room is
   decoded beside yours and it walks on alone.                        */
static int wisp_move(App *a, int bring){
    Wisp *w = a->wisp;
    if (!w || !w->wants) return 0;
    uint64_t to = w->wants;
    w->wants = 0; w->asked = 0;

    int bi, ri, ui;
    if (!city_find_addr(a->city, to, &bi, &ri, &ui)){
        char why[128], ann[64];
        vm_annotate(&w->vm, to, ann, sizeof ann);
        if (ann[0]) snprintf(why, sizeof why, "the run left this file, for %s", ann);
        else snprintf(why, sizeof why, "the run left this file, at 0x%llx",
                      (unsigned long long)to);
        wisp_stop(w, why);
        app_message(a, "%s", why);
        return 0;
    }
    /* the fuel is what really bounds a run; this is only a backstop against
       a wisp that spends its whole life crossing thresholds */
    if (w->nrooms > 2000){ wisp_stop(w, "far enough -- 2000 rooms"); return 0; }

    if (bring){
        a->keepWisp = 1;
        teleport_addr(a, to, "the wisp leads");
        a->keepWisp = 0;
        city_leave_wisp_room(a->city);        /* it is your room again */
        if (a->p.inside < 0 || a->p.room < 0){ wisp_stop(w, "nowhere to land"); return 0; }
        a->wispBi = a->p.inside; a->wispRi = a->p.room;
        a->wispAway = 0;
    } else {
        /* remember the door, so the room it left keeps a mark at the call */
        Room *from = wisp_room(a);
        const Insn *at = from ? wisp_insn(w, from) : NULL;
        if (at && a->wispBi == a->p.inside && a->wispRi == a->p.room){
            a->wispDoorBi = a->wispBi; a->wispDoorRi = a->wispRi;
            a->wispDoorAddr = at->addr;
        }
        city_enter_wisp_room(a->city, bi, ri);
        a->wispBi = bi; a->wispRi = ri;
        a->wispAway = !(bi == a->p.inside && ri == a->p.room);
    }
    Room *r = wisp_room(a);
    if (!r || !wisp_rehome(w, r, to)){
        wisp_stop(w, "the address is not an instruction in that room");
        return 0;
    }
    return 1;
}

/* The wisp has reached a door and stopped in front of it.  Going through it
   moves the whole city under you, which is the one thing in this feature
   that is genuinely hard to keep up with -- so it asks, and says where the
   door goes and whether it is even in this tower.                    */
static void wisp_ask(App *a){
    Wisp *w = a->wisp;
    if (!w || !w->wants){ a->wispAsk[0] = 0; a->wispAskKind = 0; return; }

    /* A call being stepped over: staying at the call site is the whole
       point, so it goes through every door on its own and never asks.  By
       the time it returns wisp_step_once has cleared overCall, and the ask
       below is the "the wisp is back" one, which is exactly the offer to
       go where it got to.                                            */
    if (w->overCall){ wisp_move(a, 0); return; }

    int bi = -1, ri = -1, ui;
    int known = city_find_addr(a->city, w->wants, &bi, &ri, &ui);
    int intoYours = known && bi == a->p.inside && ri == a->p.room;

    /* While it is away it moves on its own; it only stops to ask when it is
       about to leave the room you are in, or to walk back into it.     */
    if (a->wispAway && !intoYours){ wisp_move(a, 0); return; }

    w->paused = 1;
    w->asked = 1;

    Room *here = wisp_room(a);
    const Insn *in = here ? wisp_insn(w, here) : NULL;
    int kind = (in && in->cls == IC_CALL) ? 1 : (in && in->cls == IC_RET) ? 2 : 3;
    a->wispAskKind = kind;

    uint64_t to = w->wants;
    const char *nm = vm_callee_name(&w->vm, to);
    char where[128];
    if (known){
        Building *b = &a->city->bld[bi];
        Room *r = &b->rooms[ri];
        snprintf(where, sizeof where, "%.28s / %.40s / floor %d%s",
                 b->label, r->title, r->floor + 1,
                 bi == a->p.inside ? "   (this tower)" : "   -- another tower");
    } else {
        snprintf(where, sizeof where, "somewhere outside this file");
    }

    if (a->wispAway)
        snprintf(a->wispAsk, sizeof a->wispAsk,
                 "the wisp is back, in this room  [E] pick it up again"
                 "   [O] let it carry on");
    else if (kind == 1)
        snprintf(a->wispAsk, sizeof a->wispAsk,
                 "the wisp is calling %.60s  ->  %s"
                 "     [E] go in with it   [O] stay, and let it go alone",
                 nm ? nm : "an unnamed address", where);
    else if (kind == 2)
        snprintf(a->wispAsk, sizeof a->wispAsk,
                 "the wisp is returning to %s"
                 "     [E] go back with it   [O] stay, and let it go alone", where);
    else
        snprintf(a->wispAsk, sizeof a->wispAsk,
                 "the wisp is leaving for %.40s  ->  %s"
                 "     [E] go with it   [O] stay, and let it go alone",
                 nm ? nm : "another room", where);
}

/* [J]: go and stand where the wisp got to. */
static void wisp_catchup(App *a){
    if (!a->wisp){ app_message(a, "no wisp to catch up with"); return; }
    Room *r = wisp_room(a);
    const Insn *in = r ? wisp_insn(a->wisp, r) : NULL;
    if (!in){ app_message(a, "the wisp is nowhere to stand"); return; }
    a->keepWisp = 1;
    teleport_addr(a, in->addr, "caught up with the wisp");
    a->keepWisp = 0;
    city_leave_wisp_room(a->city);          /* it is your room again */
    a->wispBi = a->p.inside; a->wispRi = a->p.room;
    a->wispAway = 0;
    a->wispAsk[0] = 0; a->wispAskKind = 0;
    /* You went to it, so whatever it was doing on its own is over: a call
       being stepped over is abandoned where it stands, and the run is back
       under your hand one instruction at a time.                     */
    if (a->wisp->stepping){
        a->wisp->overCall = 0;
        a->wisp->paused = 1;
        app_message(a, "caught up -- paused, step by step");
    }
}

/* [E] go with it.  [O] do not. */
static void wisp_answer(App *a, int follow){
    Wisp *w = a->wisp;
    if (!w || !w->wants) return;
    a->wispAsk[0] = 0; a->wispAskKind = 0;
    if (follow){
        wisp_move(a, 1);                       /* you go through the door too */
        if (a->wisp){
            a->wisp->overCall = 0;
            a->wisp->paused = a->wisp->stepping;
            app_message(a, a->wisp->stepping ? "with the wisp -- paused, step by step"
                                             : "following the wisp");
        }
        return;
    }
    /* You stay.  The wisp goes on without you, and its room is decoded
       beside yours until it comes back or the run ends.              */
    if (wisp_move(a, 0)){
        w->paused = 0;
        app_message(a, a->wispAway ? "the wisp goes on alone -- [J] to catch up"
                                   : "the wisp carries on");
    }
}

static void pilot_toggle(App *a){
    if (a->wisp){
        a->wisp->pilot = !a->wisp->pilot;
        a->wisp->vm.follow = a->wisp->pilot;
        a->wisp->overCall = 0;
        a->wisp->stepping = a->wisp->pilot;
        a->wisp->paused = a->wisp->pilot;
        app_message(a, a->wisp->pilot
            ? "pilot: paused  --  T step over   I step into   K let it run"
            : "pilot mode off -- the run stays in this room");
        return;
    }
    /* No wisp: start the walk the whole feature exists for, at the entry
       point of the file (§8 B2, and the request this began as). */
    uint64_t e = a->elf->entry;
    int bi, ri, ui;
    if (!e || !city_find_addr(a->city, e, &bi, &ri, &ui)){
        app_message(a, "no entry point to start from -- stand in a code room and press X");
        return;
    }
    teleport_addr(a, e, "the entry point");
    if (a->p.inside < 0 || a->p.room < 0) return;
    Room *r = &a->city->bld[a->p.inside].rooms[a->p.room];
    a->wisp = wisp_spawn(&a->city->bld[a->p.inside], r, a->elf, 0);
    if (!a->wisp){ app_message(a, "nothing to run at the entry point"); return; }
    a->wispBi = a->p.inside; a->wispRi = a->p.room; a->wispAway = 0;
    if (!wisp_rehome(a->wisp, r, e)) app_message(a, "the entry point is not decoded");
    a->wisp->pilot = 1;
    a->wisp->vm.follow = 1;
    a->wisp->nrooms = 1;
    /* It starts paused: the entry point is where you want to be looking
       before anything moves, and from here it goes one instruction at a
       time until you say otherwise.                                  */
    a->wisp->stepping = 1;
    a->wisp->paused = 1;
    app_message(a, "pilot: paused at the entry point 0x%llx  --  "
                "T step over   I step into   K let it run",
                (unsigned long long)e);
}

/* k run/pause, . step over, > step into, , slower, / faster, n reseed */
static void wisp_key(App *a, int k){
    Wisp *w = a->wisp;
    if (!w) return;
    /* The step keys drive the wisp, which is not always in the room you are
       standing in -- a call stepped over puts it somewhere else on purpose.
       Reseeding is the one that really does mean the room you are in.  */
    Room *r = wisp_room(a);
    if (!r) r = code_room_here(a);
    if (!r) return;
    switch (k){
    case 0:
        w->paused = !w->paused;
        if (!w->paused){ w->stepping = 0; w->overCall = 0; }
        app_message(a, w->paused ? "wisp paused  --  T step over   I step into"
                                 : "wisp running"); break;
    case 1: {                                   /* step over */
        wisp_step_over(w, r);
        w->t = 0;
        if (w->overCall)
            app_message(a, "stepping over the call -- it runs on its own,"
                           " and stops when it is back   [J] go with it");
        else if (w->done) app_message(a, "%s", w->why);
        break; }
    case 5: {                                   /* step into */
        wisp_step_into(w, r);
        w->t = 0;
        if (w->wants){                          /* the callee is another room */
            a->wispAsk[0] = 0; a->wispAskKind = 0;
            if (wisp_move(a, 1) && a->wisp){
                a->wisp->paused = 1;
                Room *nr = wisp_room(a);
                const Insn *ni = nr ? wisp_insn(a->wisp, nr) : NULL;
                app_message(a, "stepped in at 0x%llx",
                            (unsigned long long)(ni ? ni->addr : 0));
            }
        } else if (w->done) app_message(a, "%s", w->why);
        break; }
    case 2: w->rate *= 0.6f;
        if (w->rate < WISP_RATE_MIN) w->rate = WISP_RATE_MIN;
        app_message(a, "%.1f instructions/s", w->rate); break;
    case 3: w->rate /= 0.6f;
        if (w->rate > WISP_RATE_MAX) w->rate = WISP_RATE_MAX;
        app_message(a, "%.1f instructions/s", w->rate); break;
    case 4: {
        Room *hr = code_room_here(a);
        if (!hr) break;
        Building *b = &a->city->bld[a->p.inside];
        a->wispSeed++;
        wisp_restart(w, b, hr, (uint64_t)(hr->addr ^ ((uint64_t)a->wispSeed * 0x9E3779B1u)) | 1u);
        app_message(a, "reseeded -- run %d of this room", a->wispSeed + 1);
        break; }
    }
}

static void interact(App *a);
static void goto_floor(App *a, int d);
static void wisp_ask(App *a);
static void wisp_answer(App *a, int follow);

/* Every key that only touches the App.  Split out of the event loop so the
   self-test can press them: the interactive path is otherwise the one part
   of the program nothing exercises.  Escape, F and Ctrl+Q stay in the loop
   because they own the window and the mouse grab.                    */
void app_key(App *a, int k, int shift){
    switch (k){
    case SDLK_F1: a->showHelp = !a->showHelp; break;
    case SDLK_F2:
        a->stress = (a->stress + 1) % 3;
        app_message(a, a->stress == 0 ? "text stress test off"
                    : a->stress == 1 ? "text stress: 200 strings a frame through the mono atlas"
                    : "text stress: the same 200 through the string cache -- watch it churn");
        break;
    case SDLK_e:
        if (a->wispAsk[0]) wisp_answer(a, 1);     /* the wisp asked first */
        else interact(a);
        break;
    case SDLK_o: if (a->wispAsk[0]) wisp_answer(a, 0); break;
    case SDLK_j: wisp_catchup(a); break;
    case SDLK_TAB: a->showDetail = !a->showDetail; break;
    case SDLK_m: a->showMap = !a->showMap; break;
    case SDLK_g: a->wire = !a->wire; break;
    case SDLK_v: a->p.noclip = !a->p.noclip;
        app_message(a, a->p.noclip ? "free-fly on" : "free-fly off"); break;
    case SDLK_r: spawn_plaza(a); app_message(a, "back at the plaza"); break;
    case SDLK_x:
        if (shift){
            a->showState = !a->showState;
            if (a->showState && !a->wisp)
                app_message(a, "no wisp to show -- press X to start one");
        } else wisp_toggle(a);
        break;
    case SDLK_k: wisp_key(a, 0); break;          /* §12 asks for Space, which
                                                    is jump here */
    /* Step by step.  Letters, because a keycode for a letter is the same key
       on every layout, and the punctuation this started out with is not:
       `.` on a French keyboard is Shift+`;`, which arrives as SDLK_SEMICOLON
       and never reached these cases at all.  F10 / F11 are the debugger
       habit, and the old punctuation still works where it exists.     */
    case SDLK_t: case SDLK_F10: case SDLK_PERIOD:
        wisp_key(a, 1); break;                   /* step over */
    case SDLK_i: case SDLK_F11: case SDLK_GREATER:
        wisp_key(a, 5); break;                   /* step into */
    /* Same trap: `/` is Shift+`:` on a French keyboard.  `-` and `=` are
       unshifted on both, so they are the ones that are documented.    */
    case SDLK_COMMA:  case SDLK_MINUS:  case SDLK_KP_MINUS:
        wisp_key(a, 2); break;
    case SDLK_SLASH:  case SDLK_EQUALS: case SDLK_KP_PLUS:
        wisp_key(a, 3); break;
    case SDLK_n:      wisp_key(a, 4); break;
    case SDLK_p:      pilot_toggle(a); break;
    case SDLK_LEFTBRACKET:  goto_floor(a, -1); break;
    case SDLK_RIGHTBRACKET: goto_floor(a, +1); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* interaction                                                         */
/* ------------------------------------------------------------------ */

static void update_prompt(App *a){
    a->prompt[0] = 0; a->promptKind = 0; a->promptIdx = -1; a->promptAddr = 0;
    City *c = a->city;
    Player *p = &a->p;
    if (p->inside >= 0){
        uint64_t addr = 0; const char *label = NULL;
        if (render_pick_port(a, &addr, &label)){
            snprintf(a->prompt, sizeof a->prompt, "[E / click]  go to %s",
                     label && label[0] ? label : "there");
            a->promptKind = 3; a->promptAddr = addr;
            snprintf(a->promptName, sizeof a->promptName, "%s", label ? label : "");
        }
        return;
    }
    float best = 7.0f * 7.0f; int hit = -1;
    for (int i = 0; i < c->nportal; i++){
        float dx = p->x - c->portal[i].x, dz = p->z - c->portal[i].z;
        float d2 = dx*dx + dz*dz;
        if (d2 < best){ best = d2; hit = i; }
    }
    if (hit >= 0){
        Portal *po = &c->portal[hit];
        if (po->path[0]) snprintf(a->prompt, sizeof a->prompt, "[E]  travel to %s", po->name);
        else             snprintf(a->prompt, sizeof a->prompt, "%s is not installed here", po->name);
        a->promptKind = 1; a->promptIdx = hit;
        return;
    }
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        float dx = p->x - (bld_x0(b) - 1.4f), dz = p->z - b->bz;
        if (dx*dx + dz*dz < 5.5f * 5.5f){
            snprintf(a->prompt, sizeof a->prompt, "%s  --  walk in", b->label);
            a->promptKind = 2; a->promptIdx = i;
            return;
        }
    }
}

static void interact(App *a){
    if (a->promptKind == 3 && a->promptAddr){
        teleport_addr(a, a->promptAddr, a->promptName);
        return;
    }
    if (a->promptKind == 1 && a->promptIdx >= 0){
        Portal *po = &a->city->portal[a->promptIdx];
        if (po->path[0]) load_file(a, po->path);
        else app_message(a, "%s was not found in the library search path", po->name);
    }
}

static void goto_floor(App *a, int delta){
    Player *p = &a->p;
    if (p->inside < 0){ app_message(a, "you are not in a tower"); return; }
    Building *b = &a->city->bld[p->inside];
    int f = p->floor + delta;
    if (f < 0 || f >= b->nfloors){ app_message(a, "no floor %d", f + 1); return; }
    float cx, cz; stair_center(b, &cx, &cz);
    p->x = b->bx - 1.2f; p->z = b->bz;
    p->y = f * FLOOR_H; p->vy = 0; p->floor = f;
    floor_realize(b, f);
    app_message(a, "%s -- floor %d of %d", b->label, f + 1, b->nfloors);
}



/* ------------------------------------------------------------------ */
/* headless self test: can we actually walk in and climb?              */
/* ------------------------------------------------------------------ */

static void step_towards(App *a, float tx, float tz, int steps){
    /* budget from the distance, not a constant: plates are tens of metres
       across now, and 4.5 m/s at 60 Hz is 7.5 cm a step                  */
    float d0 = hypotf(tx - a->p.x, tz - a->p.z);
    int need = (int)(d0 / (4.5f / 60.0f) * 1.8f) + 90;
    if (need > steps) steps = need;
    for (int i = 0; i < steps; i++){
        float dx = tx - a->p.x, dz = tz - a->p.z;
        if (dx*dx + dz*dz < 0.09f) break;            /* arrived */
        a->p.yaw = atan2f(dz, dx);
        player_update(a->city, &a->p, 4.5f / 60.0f, 0, 0, 1.0f / 60.0f);
    }
}

/* Every port must be aimable and must lead somewhere in the city.  Stands
   at the door of a code room, looks at each port in turn and steps through
   it, then checks the address really is in the room it landed in.        */
static int test_ports(App *a, int bi, const char **why, int *nok){
    City *c = a->city;
    Building *b = &c->bld[bi];
    int found = 0, checked = 0;
    for (int k = b->floorStart[0]; k < b->floorStart[1] && !found; k++){
        Room *r = &b->rooms[k];
        if (r->kind != RT_FUNC) continue;
        city_enter_room(c, bi, k);
        if (!r->dis || !r->dis->nports){ city_leave_room(c); continue; }
        found = 1;
        code_layout(r->dis, b, room_x0(b, r), room_x1(b, r),
                    room_z0(b, r), room_z1(b, r), r->floor * FLOOR_H);
        for (int i = 0; i < r->dis->nports && i < 6; i++){
            /* stand just inside the door and look straight at the port */
            Port want = r->dis->ports[i];
            a->p.inside = bi; a->p.floor = r->floor; a->p.room = k;
            a->p.x = (room_x0(b, r) + room_x1(b, r)) * 0.5f;
            a->p.z = room_z0(b, r) + 0.9f;
            a->p.y = r->floor * FLOOR_H;
            float dx = want.x - a->p.x, dy = want.y - (a->p.y + EYE_H), dz = want.z - a->p.z;
            a->p.yaw = atan2f(dz, dx);
            a->p.pitch = atan2f(dy, sqrtf(dx*dx + dz*dz));
            uint64_t got = 0; const char *lab = NULL;
            if (!render_pick_port(a, &got, &lab)){ *why = "a port could not be aimed at"; return 1; }
            if (got != want.addr){ *why = "aiming at one port picked another"; return 1; }
            int tb, tr, tu;
            if (!city_find_addr(c, got, &tb, &tr, &tu)) continue;   /* not in this file */
            Room *dst = &c->bld[tb].rooms[tr];
            uint64_t at = (tu >= 0) ? dst->units[tu].addr : dst->addr;
            if (got < at){ *why = "a port led to a room that starts after it"; return 1; }
            checked++;
        }
        city_leave_room(c);
    }
    *nok = checked;
    if (!found) return 0;
    if (!checked){ *why = "no port led anywhere in the file"; return 1; }
    return 0;
}

/* §8 B2's test: start at the entry point and let the wisp lead.  At every
   hop the room the player lands in must really cover the address it asked
   for, and exactly one room's worth of decoding must be alive -- the
   invariant pilot mode exists to not break.                          */
static int pilot_selftest(App *a, int hops){
    uint64_t e = a->elf->entry;
    int bi, ri, ui;
    if (!e || !city_find_addr(a->city, e, &bi, &ri, &ui)){
        printf("ok   pilot            no entry point in this file, nothing to walk\n");
        return 0;
    }
    teleport_addr(a, e, NULL);
    if (a->p.inside < 0 || a->p.room < 0){
        printf("FAIL pilot            the entry point is not in a room we can stand in\n");
        return 1;
    }
    Room *r = &a->city->bld[a->p.inside].rooms[a->p.room];
    wisp_free(a->wisp);
    a->wisp = wisp_spawn(&a->city->bld[a->p.inside], r, a->elf, 0);
    if (!a->wisp || !wisp_rehome(a->wisp, r, e)){
        printf("FAIL pilot            could not start a run at 0x%llx\n",
               (unsigned long long)e);
        wisp_free(a->wisp); a->wisp = NULL;
        return 1;
    }
    a->wisp->pilot = 1;
    a->wisp->vm.follow = 1;

    int fail = 0, hop = 0, deepest = 0;
    long steps = 0;
    while (hop < hops && !a->wisp->done){
        for (int i = 0; i < VM_FUEL + 8 && !a->wisp->done && !a->wisp->wants; i++){
            wisp_step_once(a->wisp, &a->city->bld[a->p.inside].rooms[a->p.room]);
            steps++;
        }
        if (!a->wisp->wants) break;                 /* ended in this room */
        uint64_t want = a->wisp->wants;
        int wb, wr, wu;
        if (!city_find_addr(a->city, want, &wb, &wr, &wu)){
            pilot_follow(a);                        /* left the file: not a failure */
            break;
        }
        pilot_follow(a);
        if (a->wisp->done){
            printf("FAIL pilot            could not follow 0x%llx: %s\n",
                   (unsigned long long)want, a->wisp->why);
            fail++; break;
        }
        hop++;
        if (a->wisp->vm.ncall > deepest) deepest = a->wisp->vm.ncall;
        Room *now = &a->city->bld[a->p.inside].rooms[a->p.room];
        const Insn *in = wisp_insn(a->wisp, now);
        if (!in || in->addr != want){
            printf("FAIL pilot            asked for 0x%llx, landed on 0x%llx\n",
                   (unsigned long long)want,
                   (unsigned long long)(in ? in->addr : 0));
            fail++; break;
        }
        int budget = (now->kind == RT_GROUP) ? now->nunits : 1;
        if (disasm_live() > budget){
            printf("FAIL pilot            %d decodings alive after hop %d, room holds %d\n",
                   disasm_live(), hop, budget);
            fail++; break;
        }
    }
    if (!fail)
        printf("ok   pilot            %d hops from the entry point, %ld steps,"
               " %d rooms, %d calls deep  (%s)\n", hop, steps, a->wisp->nrooms,
               deepest, a->wisp->why[0] ? a->wisp->why : "still running");
    wisp_forget(a);
    /* the walk left us standing inside a room; the caller's budget check
       expects nothing decoded */
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return fail != 0;
}

/* ------------------------------------------------------------------ */
/* §4.2: the same call site, two CPUs, two different rooms             */
/* ------------------------------------------------------------------ */
/* A codec's dispatch asks the CPU directly and branches on the answer, so
   switching --cpu changes where the run goes.  Find a function that
   executes a `cpuid`, run a wisp through it under each profile, and see
   whether they part company.  Ordinary application code has no `cpuid` at
   all (§4.2's table), so on `git` or `bash` this reports that and stops. */

/* the first `cpuid` in the file that really decodes as one */
static uint64_t find_cpuid(const Elf *e){
    for (int i = 0; i < e->nsec; i++){
        const Sec *s = &e->sec[i];
        if (!(s->flags & 0x4) || !s->data || !s->addr) continue;
        for (uint64_t k = 0; k + 1 < s->datasz; k++)
            if (s->data[k] == 0x0f && s->data[k+1] == 0xa2){
                InsnInfo info;
                if (disasm_info(s->data + k, 2, s->addr + k, &info) &&
                    !strcmp(info.mnem, "cpuid"))
                    return s->addr + k;
            }
    }
    return 0;
}

#define CPU_TRACE 3000

/* walk from `at` under the current profile, recording where it went */
static int cpu_walk(App *a, uint64_t at, int hops, uint64_t *trace, int *nrooms){
    int bi, ri, ui;
    if (!city_find_addr(a->city, at, &bi, &ri, &ui)) return 0;
    /* Start at the top of the function, not at the cpuid: the instructions
       before it are what put the leaf in eax, and a run that begins in the
       middle asks the CPU an invented question.  The nearest `endbr64`
       behind it is the function entry in any CET build, and a much tighter
       start than the room, which for a stripped library can be a whole
       chunk of .text.                                                */
    {
        const Room *r0 = &a->city->bld[bi].rooms[ri];
        uint64_t top = (ui >= 0 && r0->units) ? r0->units[ui].addr : r0->addr;
        for (int i = 0; i < a->elf->nsec; i++){
            const Sec *sc = &a->elf->sec[i];
            if (!(sc->flags & 0x4) || !sc->data || !sc->addr) continue;
            if (at < sc->addr || at >= sc->addr + sc->datasz) continue;
            uint64_t lo = at - sc->addr, floor2 = lo > 4096 ? lo - 4096 : 0;
            for (uint64_t k = lo; k-- > floor2; )
                if (sc->data[k] == 0xf3 && sc->data[k+1] == 0x0f &&
                    sc->data[k+2] == 0x1e && sc->data[k+3] == 0xfa){
                    top = sc->addr + k;
                    break;
                }
            break;
        }
        if (top && top <= at) at = top;
    }
    teleport_addr(a, at, NULL);
    if (a->p.inside < 0 || a->p.room < 0) return 0;
    Room *r = &a->city->bld[a->p.inside].rooms[a->p.room];
    wisp_free(a->wisp);
    a->wisp = wisp_spawn(&a->city->bld[a->p.inside], r, a->elf, 0);
    if (!a->wisp || !wisp_rehome(a->wisp, r, at)){
        wisp_free(a->wisp); a->wisp = NULL;
        return 0;
    }
    a->wisp->pilot = 1;
    a->wisp->vm.follow = 1;

    int n = 0;
    for (int h = 0; h <= hops && !a->wisp->done && n < CPU_TRACE; h++){
        Room *now = &a->city->bld[a->p.inside].rooms[a->p.room];
        while (!a->wisp->done && !a->wisp->wants && n < CPU_TRACE){
            const Insn *in = wisp_insn(a->wisp, now);
            trace[n++] = in ? in->addr : 0;
            wisp_step_once(a->wisp, now);
        }
        if (!a->wisp->wants) break;
        pilot_follow(a);
    }
    if (nrooms) *nrooms = a->wisp->nrooms;
    wisp_forget(a);
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return n;
}

/* The interactive path, headless: the exact sequence the event loop runs,
   with a pilot wisp leading and both HUD sheets open.  Nothing else in the
   tests drives room_lifecycle(), wisp_tick() and pilot_follow() in the
   order the real frame does.                                        */
void app_key(App *a, int k, int shift);

static int pilot_frames_selftest(App *a, int frames){
    pilot_toggle(a);                              /* P, from the entry point */
    if (!a->wisp){
        printf("ok   pilot frames   nothing to pilot in this file\n");
        return 0;
    }
    a->wisp->rate = 40.0f;
    a->showDetail = 1; a->showState = 1; a->showMap = 1;
    int restarts = 0, hand = 0, overs = 0;
    for (int f = 0; f < frames; f++){
        /* P leaves it paused, so the first quarter is driven by hand: `T`
           steps over and `I` steps into, and a call stepped over sends it
           off on its own for as long as the callee lasts.  After that K
           lets it run, which is the path this test has always taken.  */
        if (a->wisp && f < frames / 4){
            if ((f & 7) == 0 && !a->wispAsk[0]){
                int before = a->wisp->overCall;
                app_key(a, (f & 24) ? SDLK_t : SDLK_i, 0);
                hand++;
                if (!before && a->wisp && a->wisp->overCall) overs++;
            }
        } else if (a->wisp && a->wisp->stepping && !a->wisp->overCall){
            app_key(a, SDLK_k, 0);                     /* K: let it run */
        }
        a->now += 0.016f;
        if (a->p.inside >= 0) floor_realize(&a->city->bld[a->p.inside], a->p.floor);
        room_lifecycle(a);
        if (a->wisp){
            Room *wr = wisp_room(a);          /* not always the room you are in */
            if (wr) wisp_tick(a->wisp, wr, 0.016f);
            if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        }
        if (a->wispAsk[0]) wisp_answer(a, (f & 3) != 0);   /* mostly follow */
        update_prompt(a);
        render_scene(a);
        hud_draw(a);
        if (a->wisp && a->wisp->done && restarts < 6){ /* keep it moving */
            wisp_free(a->wisp); a->wisp = NULL;
            pilot_toggle(a);
            if (a->wisp) a->wisp->rate = 40.0f;
            restarts++;
        }
    }
    printf("ok   pilot frames   %d frames of the real loop, %d rooms, %d restarts,"
           " %d by hand, %d calls stepped over\n",
           frames, a->wisp ? a->wisp->nrooms : 0, restarts, hand, overs);
    wisp_forget(a);
    a->showDetail = a->showState = 0;
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return 0;
}

/* Stay behind at the first call and check the wisp really carries on: it
   has to keep stepping, keep following calls, and come back on its own.
   A room the player is not in is never laid out, and the wisp used to
   stop dead the moment it went on ahead because of it.              */
static int away_selftest(App *a){
    wisp_forget(a);
    pilot_toggle(a);
    if (!a->wisp){ printf("ok   away          nothing to pilot here\n"); return 0; }
    /* run until it offers to go into a call, then stay behind */
    int guard = 0;
    while (a->wisp && !a->wisp->done && guard++ < 20000){
        Room *wr = wisp_room(a);
        if (!wr) break;
        /* step directly to get there: the point of the test is what
           wisp_tick() does *after* it has gone on ahead */
        wisp_step_once(a->wisp, wr);
        if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        if (a->wispAsk[0]){
            if (a->wispAskKind == 1){ wisp_answer(a, 0); break; }   /* stay */
            wisp_answer(a, 1);
        }
    }
    if (!a->wisp || !a->wispAway){
        printf("ok   away          the run never offered a call to stay behind at\n");
        wisp_forget(a);
        return 0;
    }
    unsigned at0 = a->wisp->vm.steps;
    int rooms0 = a->wisp->nrooms, back = 0;
    for (int f = 0; f < 4000 && a->wisp && !a->wisp->done; f++){
        Room *wr = wisp_room(a);
        if (!wr) break;
        wisp_tick(a->wisp, wr, 0.05f);
        if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        if (a->wispAsk[0]){ back = 1; break; }        /* it is back at your door */
    }
    unsigned at1 = a->wisp ? a->wisp->vm.steps : at0;
    int rooms1 = a->wisp ? a->wisp->nrooms : rooms0;
    int fail = 0;
    if (at1 <= at0){
        printf("FAIL away          it stopped dead: %u steps before, %u after\n", at0, at1);
        fail = 1;
    } else {
        printf("ok   away          %u steps and %d more rooms while away%s\n",
               at1 - at0, rooms1 - rooms0,
               back ? ", then knocked to come back" : "");
    }
    wisp_forget(a);
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return fail;
}

/* Step by step: the two contracts the `T` and `I` keys promise.
 *
 *  `T`  over a call that is really made -- the wisp goes into the callee on
 *       its own while the player stays at the call site, and it stops again
 *       on the instruction *after* the call, one address further on.
 *  `I`  into the same call -- the wisp stops on the callee's first
 *       instruction and the player is brought along with it.
 *
 * Both leave it paused: the whole point of step mode is that nothing moves
 * until you say so, which is the part a frame loop can get wrong quietly.
 */
static int stepping_selftest(App *a){
    int fail = 0, did = 0;

    /* --- `I` : into the callee ------------------------------------- */
    wisp_forget(a);
    pilot_toggle(a);                            /* P: paused, step by step */
    if (!a->wisp){ printf("ok   stepping      nothing to pilot here\n"); return 0; }
    if (!a->wisp->paused || !a->wisp->stepping){
        printf("FAIL stepping      P did not leave the wisp paused in step mode\n");
        fail = 1;
    }
    for (int f = 0; f < 20000 && a->wisp && !a->wisp->done; f++){
        Room *wr = wisp_room(a);
        if (!wr) break;
        if (a->wisp->paused && !a->wisp->overCall && !a->wispAsk[0]){
            const Insn *in = wisp_insn(a->wisp, wr);
            int bi, ri, ui;
            if (in && in->cls == IC_CALL && in->taddr && !vm_is_stub(&a->wisp->vm, in->taddr)
                && city_find_addr(a->city, in->taddr, &bi, &ri, &ui)){
                uint64_t want = in->taddr, over = in->addr + in->len;
                int deep = a->wisp->vm.ncall;
                app_key(a, SDLK_i, 0);                   /* I: step into */
                Room *nr = a->wisp ? wisp_room(a) : NULL;
                const Insn *ni = nr ? wisp_insn(a->wisp, nr) : NULL;
                uint64_t at = ni ? ni->addr : 0;
                /* the VM declines a call it cannot follow (the depth limit,
                   no stack pointer); then the honest landing is the step
                   over, and that is what is checked instead. */
                int made = a->wisp && a->wisp->vm.ncall > deep;
                if (at != (made ? want : over)){
                    printf("FAIL stepping      I at a call landed on 0x%llx, wanted 0x%llx\n",
                           (unsigned long long)at,
                           (unsigned long long)(made ? want : over));
                    fail = 1;
                } else if (a->wisp && !a->wisp->paused){
                    printf("FAIL stepping      I left the wisp running\n");
                    fail = 1;
                } else did |= 1;
                break;
            }
            app_key(a, SDLK_t, 0);                       /* T: step over */
        }
        wisp_tick(a->wisp, wr, 0.05f);
        if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        if (a->wispAsk[0]) wisp_answer(a, 1);
    }

    /* --- `T` : over the whole call --------------------------------- */
    wisp_forget(a);
    pilot_toggle(a);
    uint64_t after = 0;
    int pb = -1, pr = -1, stayed = 1, rooms = 0;
    for (int f = 0; f < 20000 && a->wisp && !a->wisp->done; f++){
        Room *wr = wisp_room(a);
        if (!wr) break;
        if (!after){                                     /* still looking */
            if (a->wisp->paused && !a->wisp->overCall && !a->wispAsk[0]){
                const Insn *in = wisp_insn(a->wisp, wr);
                uint64_t at = in ? in->addr + in->len : 0;
                app_key(a, SDLK_t, 0);                   /* T: step over */
                if (a->wisp && a->wisp->overCall){
                    after = at;
                    pb = a->p.inside; pr = a->p.room;
                    rooms = a->wisp->nrooms;
                }
                else if (a->wisp && !a->wisp->paused && !a->wisp->done){
                    printf("FAIL stepping      a plain T left the wisp running\n");
                    fail = 1; break;
                }
            }
        } else if (!a->wisp->overCall){
            /* it is back: it may be knocking at the door of the room it
               called from, which is the offer to go where it got to */
            if (a->wispAsk[0]) wisp_answer(a, 1);
            const Insn *in = wisp_insn(a->wisp, wisp_room(a));
            uint64_t at = in ? in->addr : 0;
            if (at != after){
                printf("FAIL stepping      T over a call came back to 0x%llx,"
                       " wanted 0x%llx\n", (unsigned long long)at,
                       (unsigned long long)after);
                fail = 1;
            } else if (!a->wisp->paused || !a->wisp->stepping){
                printf("FAIL stepping      T over a call did not stop\n");
                fail = 1;
            } else did |= 2;
            rooms = a->wisp->nrooms - rooms;
            break;
        }
        wisp_tick(a->wisp, wr, 0.05f);
        if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        /* while it is stepping over, wisp_ask sends it on alone and never
           asks -- so an ask here is a jump it wants the player to take */
        if (a->wispAsk[0] && !after) wisp_answer(a, 1);
        if (after && a->wisp->overCall &&
            (a->p.inside != pb || a->p.room != pr)) stayed = 0;
    }
    if (after && !stayed){
        printf("FAIL stepping      the player was moved while a call was stepped over\n");
        fail = 1;
    }
    if (!fail)
        printf("ok   stepping      %s%s%s\n",
               (did & 1) ? "I lands inside the callee" : "",
               (did == 3) ? ";  " : "",
               (did & 2) ? "T runs the call whole and comes back one on" : "");
    if (!did && !fail)
        printf("ok   stepping      no call in reach to step over in this file\n");
    wisp_forget(a);
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return fail;
}

/* The same loop with a hand on the keyboard: every key the app answers,
   pressed at random, while the player wanders.  This is the only test that
   drives the interactive path the way a person does.               */
static int keyfuzz_selftest(App *a, int frames){
    static const int KEYS[] = {
        SDLK_x, SDLK_x, SDLK_p, SDLK_k, SDLK_t, SDLK_t, SDLK_i, SDLK_i,
        SDLK_PERIOD, SDLK_F10, SDLK_F11, SDLK_COMMA, SDLK_SLASH, SDLK_MINUS,
        SDLK_EQUALS,
        SDLK_n, SDLK_TAB, SDLK_m, SDLK_g, SDLK_v, SDLK_r, SDLK_e, SDLK_F1,
        SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET, SDLK_F2, SDLK_o, SDLK_j, SDLK_j,
    };
    const int NK = (int)(sizeof KEYS / sizeof *KEYS);
    unsigned rng = 0x5eed1234u;
    spawn_plaza(a);
    a->lastB = a->lastR = -2;
    int pressed = 0;
    for (int f = 0; f < frames; f++){
        rng = rng * 1664525u + 1013904223u;
        if ((rng >> 16) % 7 == 0){
            int k = KEYS[(rng >> 8) % (unsigned)NK];
            app_key(a, k, ((rng >> 4) & 3) == 0);      /* sometimes with shift */
            pressed++;
        }
        /* wander, so rooms are entered and left under a running wisp */
        float fwd = (float)((int)((rng >> 20) % 3) - 1);
        float str = (float)((int)((rng >> 24) % 3) - 1);
        player_update(a->city, &a->p, fwd * 0.9f, str * 0.9f, ((rng >> 12) & 15) == 0, 0.016f);
        a->p.yaw += ((float)((rng >> 6) % 100) - 50.0f) * 0.004f;
        a->now += 0.016f;
        if (a->p.inside >= 0) floor_realize(&a->city->bld[a->p.inside], a->p.floor);
        room_lifecycle(a);
        if (a->wisp && a->p.inside >= 0 && a->p.room >= 0)
            wisp_tick(a->wisp, &a->city->bld[a->p.inside].rooms[a->p.room], 0.016f);
        if (a->wisp && a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        if (a->wispAsk[0]) wisp_answer(a, ((rng >> 18) & 3) != 0);
        if (!a->wisp && a->wispAsk[0]){ a->wispAsk[0] = 0; a->wispAskKind = 0; }
        update_prompt(a);
        render_scene(a);
        hud_draw(a);
    }
    printf("ok   key fuzz       %d frames, %d keys pressed while walking\n",
           frames, pressed);
    wisp_forget(a);
    a->showDetail = a->showState = a->showHelp = a->stress = 0;
    city_leave_room(a->city);
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->lastU = -1;
    return 0;
}

static int cpu_selftest(App *a){
    uint64_t at = find_cpuid(a->elf);
    if (!at){
        printf("  cpu profiles  no cpuid in this file -- for ordinary application code\n"
               "                it is a loader phenomenon (§4.2), so there is nothing\n"
               "                here for --cpu to change\n");
        return 0;
    }
    int was = vm_cpu_profile();
    static uint64_t trace[4][CPU_TRACE];
    const char *nm[4];
    int len[4], rooms[4], n = 0;
    {
        uint64_t off = 0;
        const char *sym = elf_sym_at(a->elf, at, &off);
        printf("  cpu profiles  a cpuid at 0x%llx%s%s; walking the function it is in:\n",
               (unsigned long long)at, sym ? ", in " : "", sym ? sym : "");
    }
    for (int i = 0; n < 4; i++){
        const char *what, *p = vm_cpu_nth(i, &what);
        if (!p) break;
        if (!strcmp(p, "host")) continue;          /* not reproducible */
        vm_cpu_set(p);
        nm[n] = p;
        rooms[n] = 0;
        uint32_t o1[4], o7[4];
        vm_cpuid(1, 0, o1);
        vm_cpuid(7, 0, o7);
        len[n] = cpu_walk(a, at, 24, trace[n], &rooms[n]);
        printf("                %-9s cpuid.1 ecx=%08x  7.0 ebx=%08x  xcr0=%llx"
               "   %d instructions, %d rooms\n",
               p, o1[2], o7[1], (unsigned long long)vm_xcr0(), len[n], rooms[n]);
        n++;
    }
    vm_cpu_set(vm_cpu_nth(was, NULL));
    if (n < 2) return 0;

    /* Where the profiles part company.  Comparing only the final address is
       not enough: a function with one exit returns from the same `ret`
       whichever kernel it chose on the way.                          */
    int split = -1;
    for (int k = 0; k < len[0] && split < 0; k++)
        for (int i = 1; i < n; i++)
            if (k >= len[i] || trace[i][k] != trace[0][k]){ split = k; break; }
    if (split < 0){
        int reached = 0;
        for (int k = 0; k < len[0]; k++) if (trace[0][k] == at){ reached = 1; break; }
        if (!reached)
            printf("                the run never reached that cpuid -- it is not on the\n"
                   "                path from the entry of the function it sits in\n");
        else
            printf("                every profile took the same path: this cpuid answers a\n"
                   "                question, and something else does the dispatching\n");
        return 0;
    }
    printf("                they part company at step %d, address 0x%llx:\n",
           split, (unsigned long long)(split ? trace[0][split - 1] : 0));
    for (int i = 0; i < n; i++)
        printf("                  %-9s goes to 0x%llx\n", nm[i],
               (unsigned long long)(split < len[i] ? trace[i][split] : 0));
    printf("                the same call site reaches different code, which is the\n"
           "                whole of what --cpu is for\n");
    return 0;
}

static int selftest(App *a){
    City *c = a->city;
    int fail = 0, tested = 0;
    for (int i = 0; i < c->nbld && tested < 8; i++){
        Building *b = &c->bld[i];
        if (b->nfloors < 2) continue;
        tested++;
        /* start outside the front door */
        a->p.x = bld_x0(b) - 6.0f; a->p.z = b->bz; a->p.y = 0;
        a->p.vy = 0; a->p.inside = -1; a->p.noclip = 0;
        step_towards(a, bld_x0(b) + 2.0f, b->bz, 200);
        int in = world_locate(c, a->p.x, a->p.z, a->p.y);
        if (in != i){ printf("FAIL %-16s could not walk in the front door (x=%.1f)\n", b->label, a->p.x); fail++; continue; }

        /* walk round to the foot of the spiral, then climb it */
        float cx, cz; stair_center(b, &cx, &cz);
        float r = 2.4f;
        step_towards(a, cx + r, cz, 260);          /* the foot of the flight */
        float ang = atan2f(a->p.z - cz, a->p.x - cx);
        /* climb to this building's top, or three floors, whichever is less */
        int ftarget = b->nfloors - 1;
        if (ftarget > 3) ftarget = 3;
        float ytarget = (float)ftarget * FLOOR_H;
        int guard = 0;
        while (a->p.y < ytarget - 0.3f && guard++ < 4000){
            ang += 0.012f;
            step_towards(a, cx + cosf(ang) * r, cz + sinf(ang) * r, 2);
        }
        if (a->p.y < ytarget - 0.6f){
            printf("FAIL %-16s stuck on the stair at y=%.2f (floor %d, %d iters)\n",
                   b->label, a->p.y, a->p.floor, guard); fail++; continue;
        }
        /* and back down one floor, to prove descent works too */
        float ytop = a->p.y;
        for (int q = 0; q < 900 && a->p.y > ytop - FLOOR_H; q++){
            ang -= 0.012f;
            step_towards(a, cx + cosf(ang) * r, cz + sinf(ang) * r, 2);
        }
        if (a->p.y > ytop - FLOOR_H + 0.6f){
            printf("FAIL %-16s could not walk back down (y=%.2f from %.2f)\n", b->label, a->p.y, ytop);
            fail++; continue;
        }
        /* walk out of the stairwell along the corridor and into a room */
        int f = a->p.floor;
        a->p.x = b->bx + CORR_W * 0.5f; a->p.z = b->bz; a->p.y = f * FLOOR_H; a->p.vy = 0;
        int target = -1;
        for (int k = b->floorStart[f]; k < b->floorStart[f+1]; k++)
            if (b->rooms[k].x1 - b->rooms[k].x0 > 3.0f){ target = k; break; }
        if (target < 0) target = b->floorStart[f];
        Room *rm = &b->rooms[target];
        float doorx = (room_x0(b, rm) + room_x1(b, rm)) * 0.5f;
        float corrz = room_z0(b, rm) - CORR_W * 0.5f;
        step_towards(a, b->bx + CORR_W * 0.5f, corrz, 400);   /* along the spine */
        step_towards(a, doorx, corrz, 400);                   /* along the row   */
        float depth = room_z1(b, rm) - room_z0(b, rm);
        step_towards(a, doorx, room_z0(b, rm) + (depth < 3.0f ? depth * 0.5f : 1.6f), 300);
        int got = room_at(c, &a->p);
        /* entering decodes the room; leaving must free it */
        int in_room = got >= 0 ? got : target;
        city_enter_room(c, i, in_room);
        Room *ir = &b->rooms[in_room];
        int decoded = ir->dis ? ir->dis->n : 0;
        int budget = 1;                        /* a chamber lights every alcove */
        if (ir->kind == RT_GROUP){
            budget = ir->nunits;
            for (int u = 0; u < ir->nunits; u++)
                if (ir->units[u].dis) decoded += ir->units[u].dis->n;
        }
        if (disasm_live() > budget){
            printf("FAIL %-16s %d decodings alive at once, room holds %d\n",
                   b->label, disasm_live(), budget);
            fail++; continue;
        }
        city_leave_room(c);
        if (disasm_live() != 0){
            printf("FAIL %-16s decoding leaked on leaving (%d alive)\n", b->label, disasm_live());
            fail++; continue;
        }
        if (got != target){
            printf("FAIL %-16s could not get through the door of '%s' (in room %d, wanted %d)\n",
                   b->label, rm->title, got, target); fail++; continue;
        }
        const char *why = NULL; int nport = 0;
        if (test_ports(a, i, &why, &nport)){
            printf("FAIL %-16s %s\n", b->label, why); fail++; continue;
        }
        if (decoded) printf("ok   %-16s door -> spiral -> floor %d -> room '%s'  (%d instructions, %d ports followed, freed on exit)\n",
                            b->label, f + 1, rm->title, decoded, nport);
        else         printf("ok   %-16s door -> spiral -> floor %d -> room '%s'  (%d ports followed)\n",
                            b->label, f + 1, rm->title, nport);
    }
    fail += pilot_selftest(a, 40);
    fail += cpu_selftest(a);
    fail += pilot_frames_selftest(a, 1200);
    fail += away_selftest(a);
    fail += stepping_selftest(a);
    fail += keyfuzz_selftest(a, 3000);
    if (disasm_live() != 0){ printf("FAIL %d decodings still alive at the end\n", disasm_live()); fail++; }
    printf("%s [%s]: %d towers tested, %d failed\n",
           a->elf->base, disasm_arch_name(), tested, fail);
    return fail != 0;
}

/* ------------------------------------------------------------------ */
/* --vmtest: run a wisp in every code room and see what happens        */
/* ------------------------------------------------------------------ */
/* The assertions are the ones that matter for a walker with no semantics:
   every run terminates, nothing faults, the shadow memory stays inside its
   page cap, a wisp's whole lifecycle leaves disasm_live() where it found
   it, and the same room walked twice takes exactly the same path.  The
   table it prints is the honest measure of how far the walk gets.    */

typedef struct {
    int rooms, ran;
    int endRet, endPort, endFuel, endStop;
    long steps, conds, decided, guessed, tier0, tier1, tier2, tier3, noaccess;
    long calls, canary, relocs;
    long callsNamed, callsModelled;
    int endChk, hot[4];
    char vecShown[160];
    int stopIndirect, stopOff, stopOther;
    int maxpg, pgfull, maxsteps;
    int failTerm, failLive, failPage, failDet;
} VmStats;

/* walk one wisp to a standstill; returns the number of steps it took */
static int run_to_end(Wisp *w, Room *r, int cap){
    int n = 0;
    while (!w->done && n < cap){ wisp_step_once(w, r); n++; }
    return n;
}

/* the addresses a run visits, so two runs of the same room can be compared */
static int trace_run(App *a, Building *b, Room *r, uint64_t *out, int max){
    Wisp *w = wisp_spawn(b, r, a->elf, 0);
    if (!w) return -1;
    int n = 0;
    while (!w->done && n < max){
        const Insn *in = wisp_insn(w, r);
        out[n++] = in ? in->addr : 0;
        wisp_step_once(w, r);
    }
    wisp_free(w);
    return n;
}

static void vmtest_room(App *a, int bi, int ri, VmStats *st, int show){
    City *c = a->city;
    Building *b = &c->bld[bi];
    Room *r = &b->rooms[ri];
    if (!room_is_code(b, r) && r->kind != RT_GROUP) return;

    int live0 = disasm_live();
    city_enter_room(c, bi, ri);
    int liveIn = disasm_live();          /* what this room costs, wisp aside */
    if (!room_dis(r) && !(r->kind == RT_GROUP && r->units)){ city_leave_room(c); return; }
    st->rooms++;

    Wisp *w = wisp_spawn(b, r, a->elf, 0);
    if (!w){ city_leave_room(c); return; }
    st->ran++;

    /* the cap is deliberately above VM_FUEL: if a run gets here it did not
       terminate on its own, which is a failure and not a slow room */
    int n = run_to_end(w, r, VM_FUEL + 64);
    if (!w->done){
        printf("FAIL %-14s '%s': did not terminate in %d steps\n", b->label, r->title, n);
        st->failTerm++;
    }
    /* where a run that ran out of fuel actually spent it */
    if (show && vm_spent(&w->vm) && st->endFuel < 6){
        const Disasm *d = wisp_dis(w, r);
        printf("  FUEL  %-22s ", r->title);
        for (int pass = 0; pass < 3 && d; pass++){
            int best = -1, bn = 0;
            for (int i = 0; i < d->n && i < w->nvisits; i++){
                int skip = 0;
                for (int q = 0; q < pass; q++) if (i == st->hot[q]) skip = 1;
                if (!skip && w->visits[i] > bn){ bn = w->visits[i]; best = i; }
            }
            if (best < 0) break;
            st->hot[pass] = best;
            printf("%dx %s %.18s | ", bn, d->ins[best].mnem, d->ins[best].ops);
        }
        /* the hottest conditional, and what set its flags */
        int hb = -1, hn = 0;
        for (int i = 0; d && i < d->n && i < w->nvisits; i++)
            if (d->ins[i].cls == IC_CJUMP && w->visits[i] > hn){ hn = w->visits[i]; hb = i; }
        if (hb > 0)
            printf("loop on: %s %.24s / %s %.20s", d->ins[hb-1].mnem, d->ins[hb-1].ops,
                   d->ins[hb].mnem, d->ins[hb].ops);
        printf("\n");
    }
    if (w->vm.npg > VM_MAXPG){
        printf("FAIL %-14s '%s': %d shadow pages, cap is %d\n",
               b->label, r->title, w->vm.npg, VM_MAXPG);
        st->failPage++;
    }
    if (w->vm.pgFull) st->pgfull++;
    if (w->vm.npg > st->maxpg) st->maxpg = w->vm.npg;
    if (w->nsteps > st->maxsteps) st->maxsteps = w->nsteps;

    st->steps    += w->nsteps;
    st->conds    += w->vm.nCond;
    st->decided  += w->vm.nDecided;
    st->guessed  += w->vm.nGuessed;
    st->tier0    += w->vm.nTier0;
    st->tier1    += w->vm.nTier1;
    st->tier2    += w->vm.nTier2;
    st->tier3    += w->vm.nTier3;
    /* the first vector state a run produced, as the panel formats it (§4.4) */
    if (show && !st->vecShown[0] && w->vm.vecSlot >= 0 && w->vm.vecEw){
        const Vm *vm = &w->vm;
        int ew = vm->vecEw, nb = vm->vecBytes ? vm->vecBytes : 16;
        int wn = snprintf(st->vecShown, sizeof st->vecShown, "xmm%d as %d x int%d: ",
                          vm->vecSlot, nb / ew, ew * 8);
        for (int o = 0; o + ew <= nb && wn < 120; o += ew){
            unsigned long long uv2 = 0;
            for (int q = ew - 1; q >= 0; q--)
                uv2 = (uv2 << 8) | vm->v[vm->vecSlot].b[o + q];
            long long lv = (long long)uv2;
            if (ew < 8){ unsigned long long sb = 1ull << (ew * 8 - 1);
                         lv = (long long)((uv2 ^ sb) - sb); }
            wn += snprintf(st->vecShown + wn, sizeof st->vecShown - (size_t)wn, "%lld ", lv);
        }
    }
    st->calls    += w->vm.nCalls;
    st->canary   += w->vm.nCanary;
    st->relocs   += w->vm.nRelocs;
    st->callsNamed    += w->vm.nCallsNamed;
    st->callsModelled += w->vm.nCallsModelled;
    st->noaccess += w->vm.nNoAccess;
    if (w->nports && w->lastPort){
        const Disasm *d = wisp_dis(w, r);
        for (int i = 0; d && i < d->nports; i++)
            if (d->ports[i].addr == w->lastPort &&
                strstr(d->ports[i].label, "stack_chk_fail")) st->endChk++;
    }
    if (w->nret) st->endRet++;
    else if (w->nports || w->nleft) st->endPort++;
    else if (vm_spent(&w->vm)) st->endFuel++;
    else {
        st->endStop++;
        if (strstr(w->why, "indirect"))          st->stopIndirect++;
        else if (strstr(w->why, "off the end"))  st->stopOff++;
        else                                     st->stopOther++;
    }

    /* a wisp must not perturb the decoding it walks over */
    if (disasm_live() != liveIn){
        printf("FAIL %-14s '%s': disasm_live() moved from %d to %d under a wisp\n",
               b->label, r->title, liveIn, disasm_live());
        st->failLive++;
    }
    wisp_free(w);

    /* the same room twice must take the same path (§5, determinism) */
    enum { TR = 512 };
    static uint64_t t1[TR], t2[TR];
    int n1 = trace_run(a, b, r, t1, TR);
    int n2 = trace_run(a, b, r, t2, TR);
    if (n1 != n2 || (n1 > 0 && memcmp(t1, t2, (size_t)n1 * sizeof *t1))){
        printf("FAIL %-14s '%s': two runs diverged (%d vs %d steps)\n",
               b->label, r->title, n1, n2);
        st->failDet++;
    }
    if (disasm_live() != liveIn){
        printf("FAIL %-14s '%s': disasm_live() is %d after three wisps, was %d\n",
               b->label, r->title, disasm_live(), liveIn);
        st->failLive++;
    }

    city_leave_room(c);
    if (disasm_live() != live0){
        printf("FAIL %-14s '%s': %d decodings alive after the wisp, not %d\n",
               b->label, r->title, disasm_live(), live0);
        st->failLive++;
    }
}

/* The memory layers are not driven by tier 0 -- it executes no operands --
   so check them here directly, or the page-cap assertion below has nothing
   to assert against.  Tier 1 will exercise them for real.            */
static int vm_memcheck(App *a){
    Vm m;
    vm_init(&m, a->elf, a->elf->entry, 0x1234567u);
    int fail = 0;
    Prov pr;

    /* layer 2: an address inside a real section must give the file's bytes */
    const Sec *ro = NULL;
    for (int i = 0; i < a->elf->nsec; i++){
        const Sec *sc = &a->elf->sec[i];
        if ((sc->flags & 0x2) && sc->addr && sc->data && sc->datasz >= 8){ ro = sc; break; }
    }
    if (ro){
        uint64_t got = vm_read(&m, ro->addr, 8, &pr);
        uint64_t want = elf_rd64(a->elf, ro->data);
        if (got != want || pr != PV_FILE){
            printf("FAIL memory: %s+0 read 0x%llx (%s), the file says 0x%llx\n",
                   ro->name, (unsigned long long)got, vm_prov_name(pr),
                   (unsigned long long)want);
            fail++;
        }
    }

    /* layer 3: an unmapped address is invented once and then remembered */
    uint64_t a1 = 0x0000710000000000ull;
    uint64_t v1 = vm_read(&m, a1, 8, &pr);
    if (pr != PV_INVENTED){ printf("FAIL memory: unmapped read is not invented\n"); fail++; }
    if (vm_read(&m, a1, 8, &pr) != v1){
        printf("FAIL memory: an invented value changed on the second read\n"); fail++;
    }

    /* layer 1: a write wins over both, and never touches the mapped file */
    if (ro){
        vm_write(&m, ro->addr, 8, 0xdeadbeefcafef00dull);
        if (vm_read(&m, ro->addr, 8, &pr) != 0xdeadbeefcafef00dull){
            printf("FAIL memory: the shadow did not shadow the file\n"); fail++;
        }
        if (elf_rd64(a->elf, ro->data) == 0xdeadbeefcafef00dull){
            printf("FAIL memory: the mapped file was written to\n"); fail++;
        }
    }

    /* the cap: past it a run stops recording and keeps inventing, which is
       what keeps a runaway loop from eating the machine */
    for (int i = 0; i < VM_MAXPG + 40; i++) vm_read(&m, 0x0000720000000000ull + (uint64_t)i * 4096, 1, &pr);
    if (m.npg > VM_MAXPG){
        printf("FAIL memory: %d pages allocated, the cap is %d\n", m.npg, VM_MAXPG); fail++;
    }
    if (!m.pgFull){ printf("FAIL memory: the cap was passed without being noticed\n"); fail++; }
    printf("  memory layers %s: file, shadow, invention; %d pages at the cap\n",
           fail ? "FAILED" : "ok", m.npg);
    vm_free(&m);
    return fail;
}

static int vmtest(App *a, int nbld, int show){
    City *c = a->city;
    /* the semantics corpus first: it is the only thing here that says
       whether the numbers on the panel are right, rather than merely
       plausible.  It needs an x86-64 handle, whatever file we opened. */
    int fail0 = 0;
    {
        int mach = disasm_machine(), is64 = a->elf->is64, be = a->elf->be;
        disasm_open(62, 1, 0);
        fail0 = vm_corpus_check("tests/vm-corpus.txt", a->elf, show);
        disasm_open(mach, is64, be);
    }
    VmStats st;
    memset(&st, 0, sizeof st);
    int towers = 0;
    for (int i = 0; i < c->nbld && towers < nbld; i++){
        Building *b = &c->bld[i];
        if (b->district != DK_CODE) continue;
        towers++;
        /* a big .text is thousands of rooms and each one is three runs, so
           cap the sample rather than the truth of it */
        for (int k = 0; k < b->nrooms && st.rooms < 600; k++) vmtest_room(a, i, k, &st, show);
    }
    int fail = st.failTerm + st.failLive + st.failPage + st.failDet + fail0;
    printf("\n%s [%s]  %d code rooms, %d wisps run, %ld steps\n",
           a->elf->base, disasm_arch_name(), st.rooms, st.ran, st.steps);
    if (st.ran){
        printf("  ended at      ret %d (%.0f%%),  left the room %d (%.0f%%),"
               "  out of fuel %d (%.0f%%),  stuck %d\n",
               st.endRet, 100.0 * st.endRet / st.ran,
               st.endPort, 100.0 * st.endPort / st.ran,
               st.endFuel, 100.0 * st.endFuel / st.ran, st.endStop);
        printf("  steps         %.1f mean, %d worst (fuel is %d)\n",
               (double)st.steps / st.ran, st.maxsteps, VM_FUEL);
        double tot = st.steps ? (double)st.steps : 1.0;
        printf("  semantics     tier 1 %ld (%.1f%%),  tier 2 %ld (%.1f%%),"
               "  tier 3 %ld (%.1f%%)\n"
               "                tier 0 %ld (%.1f%%) -- modelled %.1f%% of all steps\n",
               st.tier1, 100.0 * st.tier1 / tot, st.tier2, 100.0 * st.tier2 / tot,
               st.tier3, 100.0 * st.tier3 / tot,
               st.tier0, 100.0 * st.tier0 / tot,
               100.0 * (st.steps - st.tier0) / tot);
        printf("  calls         %ld stepped over, %ld of them to a name (%.0f%%),"
               " %ld to a callee we model\n", st.calls, st.callsNamed,
               st.calls ? 100.0 * st.callsNamed / st.calls : 0.0, st.callsModelled);
        printf("  canary        %ld loads at fs:0x28;  %d runs dived into"
               " __stack_chk_fail\n", st.canary, st.endChk);
        if (st.endStop)
            printf("  stuck because  %d an indirect branch we could not resolve,"
                   " %d stepped outside the room, %d other\n",
                   st.stopIndirect, st.stopOff, st.stopOther);
        if (st.vecShown[0]) printf("  a vector row   %s\n", st.vecShown);
        printf("  relocations   %ld applied into the shadow before each run\n",
               st.ran ? st.relocs / st.ran : 0);
        printf("  conditionals  %ld seen, %ld decided (%.1f%%), %ld guessed\n",
               st.conds, st.decided, st.conds ? 100.0 * st.decided / st.conds : 0.0,
               st.guessed);
        printf("  shadow memory %d pages at worst (cap %d), %d runs hit the cap\n",
               st.maxpg, VM_MAXPG, st.pgfull);
    }
    vm_tier_report(a->elf);
    vm_string_report(a->elf);
    fail += vm_memcheck(a);
    printf("  %s\n", fail ? "FAILED" : "every run terminated, nothing leaked, both runs of every room agreed");
    return fail != 0;
}

/* ------------------------------------------------------------------ */
/* screenshot mode: render a few canned viewpoints and exit            */
/* ------------------------------------------------------------------ */

static void save_ppm(const char *path, int w, int h){
    unsigned char *px = malloc((size_t)w * h * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f){
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        fprintf(stderr, "wrote %s\n", path);
    }
    free(px);
}

static int find_bld(App *a, const char *name){
    for (int i = 0; i < a->city->nbld; i++)
        if (!strcmp(a->city->bld[i].label, name)) return i;
    return -1;
}

static void shot(App *a, const char *dir, const char *name){
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    a->p.room = room_at(a->city, &a->p);
    room_lifecycle(a);
    render_scene(a);
    hud_draw(a);
    glFinish();
    save_ppm(path, a->winw, a->winh);
}

static int shot_mode(App *a, const char *dir){
    City *c = a->city;
    spawn_plaza(a);
    a->p.pitch = 0.05f;
    shot(a, dir, "01-plaza");

    a->p.x = 0; a->p.y = 130; a->p.z = c->minz - 150;
    a->p.yaw = (float)M_PI * 0.5f; a->p.pitch = -0.44f;
    a->p.inside = -1;
    shot(a, dir, "02-skyline");

    int bi = find_bld(a, ".text");
    if (bi < 0) bi = 0;
    Building *b = &c->bld[bi];
    a->p.x = bld_x0(b) - 13.0f; a->p.z = b->bz + 5.0f; a->p.y = 0;
    a->p.yaw = 0.16f; a->p.pitch = 0.10f;
    a->p.inside = -1; a->p.floor = 0;
    shot(a, dir, "03-entrance");

    float cx, cz; stair_center(b, &cx, &cz);
    a->p.x = b->bx + 1.6f; a->p.z = b->bz; a->p.y = 0;
    a->p.yaw = (float)M_PI; a->p.pitch = 0.22f;
    (void)cx; (void)cz;
    a->p.inside = bi; a->p.floor = 0;
    floor_realize(b, 0);
    shot(a, dir, "04-stairwell");

    /* stand in the first row's corridor and look along it */
    {
        Room *r0 = &b->rooms[b->floorStart[0]];
        a->p.x = b->bx + CORR_W * 0.5f + 1.0f;
        a->p.z = room_z0(b, r0) - CORR_W * 0.5f;
        a->p.y = 0;
        a->p.yaw = 0.02f; a->p.pitch = 0.02f;
        a->p.inside = bi; a->p.floor = 0;
    }
    shot(a, dir, "05-corridor");

    int ri = -1;
    for (int i = b->floorStart[0]; i < b->floorStart[1]; i++)
        if (b->rooms[i].kind == RT_FUNC){ ri = i; break; }
    if (ri < 0) ri = b->floorStart[0];
    stand_in_room(a, bi, ri);
    shot(a, dir, "06-function-room");

    int si = find_bld(a, ".dynstr");
    if (si < 0) si = find_bld(a, ".strtab");
    if (si < 0) si = find_bld(a, ".shstrtab");
    if (si >= 0){
        Building *sb = &c->bld[si];
        stand_in_room(a, si, sb->floorStart[0]);
        shot(a, dir, "07-strings-room");
    }
    int di = find_bld(a, ".dynamic");
    if (di >= 0){
        Building *db = &c->bld[di];
        stand_in_room(a, di, db->floorStart[0]);
        shot(a, dir, "08-dynamic-room");
    }
    int ri2 = -1;
    for (int i = 0; i < c->nbld && ri2 < 0; i++)
        if (c->bld[i].district == DK_DATA)
            for (int k = c->bld[i].floorStart[0]; k < c->bld[i].floorStart[1]; k++)
                if (c->bld[i].rooms[k].kind == RT_OBJECT){ bi = i; ri2 = k; break; }
    if (ri2 >= 0){ stand_in_room(a, bi, ri2); shot(a, dir, "09-data-room"); }

    /* a code room, decoded: the sculptures and their branch wiring */
    int cbi = find_bld(a, ".text"); if (cbi < 0) cbi = 0;
    Building *cb = &c->bld[cbi];
    int cri = -1; uint64_t bestsz = 0;
    for (int f = 0; f < cb->nfloors && f < 4; f++)
        for (int k = cb->floorStart[f]; k < cb->floorStart[f+1]; k++){
            Room *rr = &cb->rooms[k];
            if (rr->size > 260 && rr->size < 2600 && rr->size > bestsz){ bestsz = rr->size; cri = k; }
        }
    if (cri < 0) cri = cb->floorStart[0];
    a->now = 3.7f;
    stand_in_room(a, cbi, cri);
    shot(a, dir, "11-code-room");

    {   /* and again from among the sculptures */
        Room *rr = &cb->rooms[cri];
        float rd = room_z1(cb, rr) - room_z0(cb, rr);
        a->p.z = room_z0(cb, rr) + (rd < 4.0f ? rd * 0.5f : 2.6f);
        a->p.pitch = -0.16f;
        shot(a, dir, "12-code-close");
    }

    /* a chamber: seven small units around an open middle, the one you are
       standing at decoded into sculpture */
    {
        int gb = -1, gr = -1;
        for (int i = 0; i < c->nbld && gb < 0; i++){
            Building *bb2 = &c->bld[i];
            if (!bb2->sec || !(bb2->sec->flags & 0x4)) continue;
            for (int k = 0; k < bb2->nrooms; k++)
                if (bb2->rooms[k].kind == RT_GROUP && bb2->rooms[k].nunits >= 6){
                    gb = i; gr = k; break;
                }
        }
        if (gb >= 0){
            Building *bb2 = &c->bld[gb];
            Room *rr = &bb2->rooms[gr];
            stand_in_room(a, gb, gr);
            float x0, z0, x1, z1;
            if (room_cell_rect(bb2, rr, 0, &x0, &z0, &x1, &z1)){
                a->p.x = (x0 + x1) * 0.5f + 1.4f;
                a->p.z = (z0 + z1) * 0.5f + 1.4f;
            }
            a->p.pitch = -0.32f;
            a->p.inside = gb; a->p.floor = rr->floor; a->p.room = gr;
            room_lifecycle(a);
            shot(a, dir, "14-chamber");
            int lit = 0;
            for (int u = 0; u < rr->nunits; u++) if (rr->units[u].dis) lit++;
            printf("  chamber '%s': %d units, %d decoded, standing at alcove %d\n",
                   rr->title, rr->nunits, lit, rr->activeUnit);
            city_leave_room(c);
        }
    }

    {   /* a room small enough to see its far wall, so the ports read */
        int pb = -1, pr = -1, bestn = 0;
        for (int f = 0; f < cb->nfloors && f < 12 && bestn < 9; f++)
            for (int k = cb->floorStart[f]; k < cb->floorStart[f+1]; k++){
                Room *rr = &cb->rooms[k];
                if (rr->kind != RT_FUNC || rr->size < 250 || rr->size > 1400) continue;
                city_enter_room(c, cbi, k);
                int np = rr->dis ? rr->dis->nports : 0;
                city_leave_room(c);
                if (np > bestn){ bestn = np; pb = cbi; pr = k; }
            }
        if (pr >= 0){
            stand_in_room(a, pb, pr);
            a->p.pitch = 0.06f;
            shot(a, dir, "15-ports");
            Room *rr = &cb->rooms[pr];
            printf("  ports: room '%s' has %d exits by %d ports\n", rr->title,
                   rr->dis ? rr->dis->nexits : 0, rr->dis ? rr->dis->nports : 0);
            city_leave_room(c);
        }
    }

    {   /* a wisp running: pick a small room whose run lasts long enough to
           be caught mid-walk, then stand under the panel and look up */
        int wri = -1, want = 26;
        for (int f = 0; f < cb->nfloors && f < 6 && wri < 0; f++)
            for (int k = cb->floorStart[f]; k < cb->floorStart[f+1]; k++){
                Room *q = &cb->rooms[k];
                if (q->kind != RT_FUNC || q->size < 90 || q->size > 400) continue;
                city_enter_room(c, cbi, k);
                Wisp *probe = wisp_spawn(cb, q, a->elf, 0);
                int ok = 0;
                if (probe){
                    for (int i = 0; i < want && !probe->done; i++) wisp_step_once(probe, q);
                    ok = !probe->done;
                    wisp_free(probe);
                }
                city_leave_room(c);
                if (ok){ wri = k; break; }
            }
        if (wri < 0) wri = cri;
        stand_in_room(a, cbi, wri);
        a->lastB = a->lastR = -2;
        room_lifecycle(a);
        Room *rr = &cb->rooms[wri];
        wisp_free(a->wisp);
        a->wisp = wisp_spawn(cb, rr, a->elf, 0);
        if (a->wisp){
            /* run it with the clock going, so §7's trail has entries with
               ages rather than all of them at zero */
            a->wisp->rate = 8.0f;
            for (int k = 0; k < 90 && !a->wisp->done; k++){
                render_scene(a); hud_draw(a);
                wisp_tick(a->wisp, rr, 0.04f);
            }
            a->wisp->paused = 1;
            /* the sculptures get their coordinates when the room is first
               drawn, so draw it once before asking where the wisp is */
            render_scene(a); hud_draw(a);
            for (int k = 0; k < 40; k++) wisp_tick(a->wisp, rr, 0.05f);   /* settle */

            float pos[3]; wisp_pos(a->wisp, rr, pos);
            float top = rr->floor * FLOOR_H + 3.00f;   /* mid-plate */
            float loz = room_z0(cb, rr) + 0.7f, hiz = room_z1(cb, rr) - 0.7f;
            float lox = room_x0(cb, rr) + 0.7f, hix = room_x1(cb, rr) - 0.7f;
            for (int pass = 0; pass < 2; pass++){
                /* pass 0 looks at the ceiling panel, pass 1 down the trail
                   of ghost cards over the sculptures it came along */
                float back = pass ? 4.2f : 5.2f;
                float cx = pos[0], cz = pos[2] - back;
                if (cz < loz) cz = loz;
                if (cz > hiz) cz = hiz;
                if (cx < lox) cx = lox;
                if (cx > hix) cx = hix;
                a->p.x = cx; a->p.z = cz; a->p.y = rr->floor * FLOOR_H;
                float hz = sqrtf((pos[0]-cx)*(pos[0]-cx) + (pos[2]-cz)*(pos[2]-cz));
                if (hz < 0.4f) hz = 0.4f;
                float aimy = pass ? pos[1] + 0.5f : top + 0.3f;
                a->p.yaw = atan2f(pos[2] - cz, pos[0] - cx);
                a->p.pitch = atan2f(aimy - (a->p.y + EYE_H), hz);
                shot(a, dir, pass ? "19-wisp-trail" : "18-wisp");
            }
            /* the state sheet and the trace view, with a wisp to fill them */
            a->showState = 1; a->showDetail = 1;
            shot(a, dir, "20-wisp-sheet");
            a->showState = 0; a->showDetail = 0;

            /* §8 B2: from the entry point, letting the wisp lead */
            {
                wisp_free(a->wisp); a->wisp = NULL;
                pilot_toggle(a);
                for (int hop = 0; a->wisp && hop < 40 && !a->wisp->done; hop++){
                    for (int i = 0; i < 400 && a->wisp && !a->wisp->done
                                     && !a->wisp->wants; i++)
                        wisp_step_once(a->wisp, &c->bld[a->p.inside].rooms[a->p.room]);
                    if (!a->wisp || !a->wisp->wants) break;
                    pilot_follow(a);
                }
                if (a->wisp){
                    a->wisp->paused = 1;
                    render_scene(a); hud_draw(a);
                    for (int k = 0; k < 40; k++)
                        wisp_tick(a->wisp, &c->bld[a->p.inside].rooms[a->p.room], 0.04f);
                    a->p.pitch = 0.28f;
                    shot(a, dir, "22-pilot");
                }
                wisp_free(a->wisp); a->wisp = NULL;
                a->lastB = a->lastR = -2;
            }

            /* the wisp gone on without you: its token still standing on the
               call it went in by, its panel still ticking */
            {
                wisp_forget(a);
                pilot_toggle(a);
                for (int guard = 0; a->wisp && guard < 4000; guard++){
                    Room *wr2 = wisp_room(a);
                    if (!wr2) break;
                    wisp_step_once(a->wisp, wr2);
                    if (a->wisp->wants){
                        wisp_ask(a);
                        if (!a->wispAsk[0]) continue;      /* it moved itself */
                        if (a->wispAskKind == 1){ wisp_answer(a, 0); break; }
                        wisp_answer(a, 1);
                    }
                    if (a->wisp->done) break;
                }
                if (a->wisp && a->wispAway){
                    for (int k = 0; k < 300 && !a->wisp->done; k++){
                        Room *wr2 = wisp_room(a);
                        if (!wr2) break;
                        wisp_step_once(a->wisp, wr2);
                        if (a->wisp->wants) wisp_ask(a);
                        if (a->wispAsk[0]) break;          /* it is coming back */
                    }
                    render_scene(a); hud_draw(a);
                    Room *hr = &c->bld[a->p.inside].rooms[a->p.room];
                    /* the door may be in any alcove of a chamber */
                    Disasm *hd = hr->dis;
                    int at = hd ? disasm_index_of(hd, a->wispDoorAddr) : -1;
                    for (int u = 0; at < 0 && hr->units && u < hr->nunits; u++){
                        hd = hr->units[u].dis;
                        at = hd ? disasm_index_of(hd, a->wispDoorAddr) : -1;
                    }
                    if (hd && at >= 0){
                        const Insn *di = &hd->ins[at];
                        float cx = di->x, cz = di->z - 7.0f;
                        a->p.noclip = 1;            /* a canned viewpoint may
                                                       stand outside the room */
                        a->p.x = cx; a->p.z = cz;
                        a->p.y = hr->floor * FLOOR_H;
                        float hz = sqrtf((di->x-cx)*(di->x-cx) + (di->z-cz)*(di->z-cz));
                        if (hz < 1.0f) hz = 1.0f;
                        a->p.yaw = atan2f(di->z - cz, di->x - cx);
                        a->p.pitch = atan2f(a->p.y + 2.2f - (a->p.y + EYE_H), hz);
                        a->p.noclip = 0;
                    }
                    shot(a, dir, "23-wisp-away");
                }
                wisp_forget(a);
                a->lastB = a->lastR = -2;
            }

            /* and from across the room: §7's middle level of detail, where
               the panel collapses to the token and its mnemonic */
            {
                float cz = pos[2] - 14.0f;
                if (cz < loz) cz = loz;
                a->p.z = cz; a->p.x = pos[0];
                float hz = pos[2] - cz; if (hz < 1.0f) hz = 1.0f;
                a->p.yaw = (float)M_PI * 0.5f;
                a->p.pitch = atan2f(pos[1] + 0.4f - (a->p.y + EYE_H), hz);
                shot(a, dir, "21-wisp-wide");
            }
        }
        wisp_free(a->wisp); a->wisp = NULL;
        a->lastB = a->lastR = -2;
    }

    a->showMap = 1; a->showDetail = 1; a->showHelp = 1;
    shot(a, dir, "13-overlays");

    /* §13's test, both ways: the same 200 never-repeated strings drawn out
       of the atlas and drawn through the string cache.  --textbench has the
       numbers; these two are so the glyphs can be compared by eye.      */
    a->showHelp = 0;
    a->stress = 1; shot(a, dir, "16-text-atlas");
    a->stress = 2; shot(a, dir, "17-text-cache");
    a->stress = 0;
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv){
    App *a = &g_app;
    a->winw = 1440; a->winh = 900;
    a->showMap = 1;
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = a->lastU = -2; a->nearIns = -1;

    const char *start = NULL, *shotdir = NULL;
    int dotest = 0, dobench = 0, dostats = 0, gpudebug = 0, dotext = 0, dovm = 0;
    int verbose = 0;
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--shot") && i + 1 < argc){ shotdir = argv[++i]; continue; }
        if (!strcmp(argv[i], "--selftest")){ dotest = 1; continue; }
        if (!strcmp(argv[i], "--bench")){ dobench = 1; continue; }
        if (!strcmp(argv[i], "--textbench")){ dotext = 1; continue; }
        if (!strcmp(argv[i], "--vmtest")){ dovm = 1; continue; }
        if (!strcmp(argv[i], "-v")){ verbose = 1; continue; }
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc){
            if (!vm_cpu_set(argv[++i])){
                fprintf(stderr, "unknown cpu profile '%s'; try:\n", argv[i]);
                for (int k = 0; ; k++){
                    const char *what, *nm = vm_cpu_nth(k, &what);
                    if (!nm) break;
                    fprintf(stderr, "  %-9s %s\n", nm, what);
                }
                return 1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--stats")){ dostats = 1; continue; }
        if (!strcmp(argv[i], "--gpudebug")){ gpudebug = 1; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")){
            printf("usage: %s [options] [binary-or-library]\n"
                   "\n"
                   "  Walk around an ELF file as if it were a city.  Defaults to /bin/ls;\n"
                   "  press F inside the app to browse for another file.\n"
                   "\n"
                   "  --shot DIR    render a set of canned viewpoints into DIR as .ppm and exit\n"
                   "  --selftest    headless check that every tower can be entered and climbed\n"
                   "  --vmtest      run a wisp in every code room of the first code towers and\n"
                   "                report where the runs got to\n"
                   "  --cpu NAME    the CPU a wisp's `cpuid` answers as: baseline (the\n"
                   "                default, SSE2 only), v2, v3 (AVX2), or host.  A codec\n"
                   "                dispatches on this, so it decides which kernel the wisp\n"
                   "                walks into -- the same call site, two different rooms\n"
                   "  --stats       print the layout the packer produced, and exit\n"
                   "  --textbench   draw 200 strings a frame that are never repeated, once\n"
                   "                through the monospace atlas and once through the string\n"
                   "                cache, and print what each costs\n"
                   "  --gpudebug    MESA_DEBUG=1 and GALLIUM_HUD=fps,VRAM-usage, for telling a\n"
                   "                stall in this program from one in the driver.  An existing\n"
                   "                value in the environment is left alone, which is how to get\n"
                   "                a useful HUD on Intel: VRAM-usage is a radeonsi/nouveau\n"
                   "                query, so try GALLIUM_HUD=fps,frametime,cpu instead.\n"
                   "  -h, --help    this text\n", argv[0]);
            return 0;
        }
        start = argv[i];
    }
    if (!start){
        static const char *cand[] = { "/bin/ls", "/usr/bin/ls", "/bin/bash", "/usr/bin/cat", NULL };
        for (int i = 0; cand[i] && !start; i++) if (!access(cand[i], R_OK)) start = cand[i];
    }
    if (!start){ fprintf(stderr, "give me an ELF file to explore\n"); return 1; }

    /* Mesa reads both of these when the driver comes up, so they have to be
       in the environment before SDL touches GL.  The HUD draws over the top
       left of the window; VRAM-usage is the one that shows the glyph cache
       and the exterior display lists growing.                            */
    if (gpudebug){
        static const struct { const char *k, *v; } ENV[] = {
            { "MESA_DEBUG",  "1" },
            { "GALLIUM_HUD", "fps,VRAM-usage" },
        };
        for (size_t i = 0; i < sizeof ENV / sizeof *ENV; i++){
            const char *had = getenv(ENV[i].k);
            if (had && had[0]) fprintf(stderr, "gpu debug: %s=%s (from the environment)\n",
                                       ENV[i].k, had);
            else { setenv(ENV[i].k, ENV[i].v, 1);
                   fprintf(stderr, "gpu debug: %s=%s\n", ENV[i].k, ENV[i].v); }
        }
    }

    if (dostats){
        char err[256];
        Elf *e = elf_open(start, err, sizeof err);
        if (!e){ fprintf(stderr, "%s: %s\n", start, err); return 1; }
        disasm_open(e->machine, e->is64, e->be);
        City *c = city_build(e);
        long rooms = 0, chambers = 0, units = 0, recovered = 0;
        int maxfl = 0; float maxh = 0;
        printf("%-20s %7s %7s %6s %6s %14s %9s\n",
               "section","rooms","tiles","floors","height","plate (m)","tile(m)");
        for (int i = 0; i < c->nbld; i++){
            Building *b = &c->bld[i];
            long ch = 0, un = 0, tl = 0;
            for (int k = 0; k < b->nrooms; k++){
                tl += b->rooms[k].ntiles;
                if (b->rooms[k].kind == RT_GROUP){ ch++; un += b->rooms[k].nunits; }
                if (!strncmp(b->rooms[k].title, "sub_", 4)) recovered++;
            }
            rooms += b->nrooms; chambers += ch; units += un;
            if (b->nfloors > maxfl) maxfl = b->nfloors;
            if (b->h > maxh) maxh = b->h;
            if (b->nrooms >= 8)
                printf("%-20s %7d %7ld %6d %5.0fm %6.1f x %-5.1f %9.3f\n",
                       b->label, b->nrooms, tl, b->nfloors, b->h, b->plateW, b->plateD, b->tile);
        }
        printf("\n%s: %d buildings, %ld rooms (%ld chambers holding %ld units, %ld recovered)\n",
               e->base, c->nbld, rooms, chambers, units, recovered);
        printf("tallest %.0f m / %d floors   city %.0f x %.0f m   far plane %.0f m\n",
               maxh, maxfl, c->maxx - c->minx, c->maxz - c->minz, c->farPlane);
        {   int hist[12] = {0}, tot = 0, bad = 0;
            for (int i = 0; i < c->nbld; i++){
                Building *b = &c->bld[i];
                for (int f = 0; f < b->nfloors; f++){
                    int k = b->floorStart[f+1] - b->floorStart[f];
                    if (k < 1 || k > MAXPF) bad++;
                    hist[k < 11 ? k : 11]++; tot++;
                }
            }
            {   /* how big are the rooms actually coming out, and how
                   slender is the tower */
                for (int i = 0; i < c->nbld; i++){
                    Building *b = &c->bld[i];
                    if (b->nrooms < 40) continue;
                    float sw = 0, mx = 0, mn = 1e9f;
                    for (int k = 0; k < b->nrooms; k++){
                        float w = room_x1(b,&b->rooms[k]) - room_x0(b,&b->rooms[k]);
                        sw += w; if (w > mx) mx = w; if (w < mn) mn = w;
                    }
                    printf("  %-16s room width min %.1f mean %.1f max %.1f m   "
                           "plate %.0f  height %.0f  aspect 1:%.0f  %.1f rooms/floor\n",
                           b->label, mn, sw / b->nrooms, mx, b->plateW, b->h,
                           b->h / b->plateW, (float)b->nrooms / b->nfloors);
                }
            }
            {   /* what the sculpture cap does to the biggest code rooms */
            int bb = -1;
            for (int i = 0; i < c->nbld; i++){
                Building *b = &c->bld[i];
                if (!b->sec || !(b->sec->flags & 0x4)) continue;
                if (bb < 0 || b->nrooms > c->bld[bb].nrooms) bb = i;
            }
            if (bb >= 0){
                Building *b = &c->bld[bb];
                int *ord = malloc((size_t)b->nrooms * sizeof(int)), no = 0;
                for (int k = 0; k < b->nrooms; k++)
                    if (room_is_code(b, &b->rooms[k])) ord[no++] = k;
                for (int x = 0; x < no; x++)          /* partial sort: top 4 */
                    for (int y = x + 1; y < no && x < 4; y++)
                        if (b->rooms[ord[y]].size > b->rooms[ord[x]].size){
                            int t = ord[x]; ord[x] = ord[y]; ord[y] = t; }
                printf("\nbiggest %s rooms (grid is what the room was sized for):\n", b->label);
                for (int x = 0; x < no && x < 4; x++){
                    city_enter_room(c, bb, ord[x]);
                    Room *r = &b->rooms[ord[x]];
                    int n = r->dis ? r->dis->n : 0;
                    printf("  %-14s %8llu B  grid %3dx%-3d = %6d tiles  decoded %4d  %-9s  covered %3.0f%%\n",
                           r->title, (unsigned long long)r->size, r->tw, r->th,
                           r->tw * r->th, n,
                           (r->dis && r->dis->truncated) ? "TRUNCATED" : "complete",
                           100.0 * n / (r->tw * r->th));
                    city_leave_room(c);
                }
                /* and the other end: do small rooms overflow their grid? */
                int over = 0, tot = 0, okind[6] = {0}, ovkind[6] = {0}, decoded[6] = {0};
                for (int x = no - 1; x >= 0 && tot < 400; x--, tot++){
                    city_enter_room(c, bb, ord[x]);
                    Room *r = &b->rooms[ord[x]];
                    okind[r->kind]++;
                    if (r->dis) decoded[r->kind]++;
                    if (r->dis && r->dis->n > r->tw * r->th){ over++; ovkind[r->kind]++; }
                    city_leave_room(c);
                }
                static const char *KN[6] = {"FUNC","OBJECT","LIST","BYTES","EMPTY","GROUP"};
                printf("  of %d code rooms sampled, %d hold more instructions than tiles\n", tot, over);
                for (int k = 0; k < 6; k++)
                    if (okind[k]) printf("      %-7s sampled %4d, decoded %4d, overflowing grid %4d\n",
                                         KN[k], okind[k], decoded[k], ovkind[k]);
                {   /* per-room mean instruction length: the low tail is what
                       decides how many tiles a room needs                    */
                    float *bpi = malloc((size_t)no * sizeof(float)); int nb = 0;
                    for (int x = 0; x < no && nb < 1500; x++){
                        city_enter_room(c, bb, ord[x]);
                        Room *r = &b->rooms[ord[x]];
                        if (r->dis && !r->dis->truncated && r->dis->n > 3)
                            bpi[nb++] = (float)r->dis->covered / r->dis->n;
                        city_leave_room(c);
                    }
                    for (int x = 1; x < nb; x++){          /* insertion sort */
                        float t = bpi[x]; int y = x - 1;
                        while (y >= 0 && bpi[y] > t){ bpi[y+1] = bpi[y]; y--; }
                        bpi[y+1] = t;
                    }
                    if (nb) printf("  per-room bytes/instruction over %d rooms:"
                        " p1 %.2f  p5 %.2f  p10 %.2f  p25 %.2f  median %.2f\n", nb,
                        bpi[nb/100], bpi[nb/20], bpi[nb/10], bpi[nb/4], bpi[nb/2]);
                    free(bpi);
                }
                {   /* the real mean instruction length, from rooms that fit */
                    uint64_t bytes = 0; long insns = 0; int sample = 0;
                    for (int x = 0; x < no && sample < 600; x++){
                        city_enter_room(c, bb, ord[x]);
                        Room *r = &b->rooms[ord[x]];
                        if (r->dis && !r->dis->truncated && r->dis->n){
                            bytes += r->dis->covered; insns += r->dis->n; sample++;
                        }
                        city_leave_room(c);
                    }
                    if (insns) printf("  mean instruction length over %d complete rooms: %.2f bytes"
                                      " (room sizing assumes 4.00)\n", sample, (double)bytes / insns);
                }
                free(ord);
            }
        }
        printf("rooms per floor:");
            for (int k = 1; k <= 10; k++) if (hist[k]) printf("  %d:%d", k, hist[k]);
            printf("   (%d floors, %d outside 1..%d)\n", tot, bad, MAXPF);
        }
        city_free(c); disasm_close(); elf_close(e);
        return 0;
    }

    /* This is a window, not a fullscreen game.  Asking the compositor to
       unredirect it makes Mutter redirect and unredirect on every focus
       change, which is one of the ways an alt-tab turns into a stall.   */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0){
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_Window *win = SDL_CreateWindow("Code City -- a 3D file explorer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, a->winw, a->winh,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | ((shotdir||dotest||dobench||dotext||dovm) ? SDL_WINDOW_HIDDEN : 0));
    if (!win){
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        win = SDL_CreateWindow("Code City -- a 3D file explorer",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, a->winw, a->winh,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (shotdir ? SDL_WINDOW_HIDDEN : 0));
    }
    if (!win){ fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx){ fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    glEnable(GL_MULTISAMPLE);

    if (!text_init()){ fprintf(stderr, "no fonts available\n"); return 1; }
    render_init();

    if (!load_file(a, start)){
        fprintf(stderr, "%s\n", a->msg);
        return 1;
    }
    a->bfiltered = malloc(BR_MAX * sizeof(int));
    snprintf(a->bdir, sizeof a->bdir, "/usr/bin");

    if (dobench){
        City *c = a->city;
        int bi = find_bld(a, ".text"); if (bi < 0) bi = 0;
        Building *bb = &c->bld[bi];
        int ri = bb->floorStart[0]; uint64_t best = 0;
        for (int k = bb->floorStart[0]; k < bb->floorStart[1]; k++)
            if (bb->rooms[k].size > best){ best = bb->rooms[k].size; ri = k; }
        stand_in_room(a, bi, ri);
        {   Room *q = &bb->rooms[ri];
            float qd = room_z1(bb, q) - room_z0(bb, q);
            if (qd > 5.0f) a->p.z += 2.0f;
        }
        room_lifecycle(a);
        Room *rr = &bb->rooms[ri];
        Uint64 t0 = SDL_GetPerformanceCounter();
        int N = 240;
        for (int k = 0; k < N; k++){ a->now = k * 0.016f; render_scene(a); hud_draw(a); }
        glFinish();
        double el = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
        printf("%s  room '%s'  %d instructions, %d wires  ->  %.1f fps (%.2f ms/frame)\n",
               a->elf->base, rr->title, rr->dis ? rr->dis->n : 0,
               rr->dis ? rr->dis->nlinks + rr->dis->nexits : 0, N / el, el / N * 1000.0);

        /* and the same room with a wisp running in it: the panel, the
           tether and the ring, plus one instruction stepped per frame */
        a->wisp = wisp_spawn(bb, rr, a->elf, 0);
        if (a->wisp){
            a->wisp->rate = 60.0f;
            t0 = SDL_GetPerformanceCounter();
            for (int k = 0; k < N; k++){
                a->now = k * 0.016f;
                if (a->wisp->done) wisp_restart(a->wisp, bb, rr, 0);
                wisp_tick(a->wisp, rr, 0.016f);
                render_scene(a); hud_draw(a);
            }
            glFinish();
            double e2 = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
            printf("%s  the same room with a wisp running   ->  %.1f fps (%.2f ms/frame, %+.2f)\n",
                   a->elf->base, N / e2, e2 / N * 1000.0, (e2 - el) / N * 1000.0);
            wisp_free(a->wisp); a->wisp = NULL;
        }
        city_leave_room(c);
        render_set_city(NULL); city_free(a->city); elf_close(a->elf); disasm_close();
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
    }
    /* §13: text that may change every frame.  The string cache keys on
       content, so a readout that is new every frame is a rasterization and
       an upload every frame, and it evicts the room's own labels.  Draw the
       same 200 never-repeated strings both ways and print what each cost. */
    if (dotext){
        const int WARM = 40, N = 200;
        struct { const char *name; int mode; } RUN[] = {
            { "no stress panel", 0 }, { "mono atlas", 1 }, { "string cache", 2 } };
        printf("%-16s %8s %10s %9s %9s %8s %11s\n",
               "path", "fps", "ms/frame", "vs base", "entries", "MiB", "rasterized");
        double base = 0, basems = 0;
        for (size_t m = 0; m < sizeof RUN / sizeof *RUN; m++){
            a->stress = RUN[m].mode;
            for (int k = 0; k < WARM; k++){ a->now = k * 0.016f; render_scene(a); hud_draw(a); }
            glFinish();
            int live0; size_t bytes0; unsigned long rast0;
            text_cache_stats(&live0, &bytes0, &rast0);
            Uint64 t0 = SDL_GetPerformanceCounter();
            for (int k = 0; k < N; k++){ a->now = k * 0.016f; render_scene(a); hud_draw(a); }
            glFinish();
            double el = (double)(SDL_GetPerformanceCounter() - t0)
                      / (double)SDL_GetPerformanceFrequency();
            int live; size_t bytes; unsigned long rast;
            text_cache_stats(&live, &bytes, &rast);
            double ms = el / N * 1000.0;
            if (!m){ base = N / el; basems = ms; }
            printf("%-16s %8.1f %10.2f %+9.2f %9d %8.1f %11lu%s\n",
                   RUN[m].name, N / el, ms, ms - basems, live, bytes / 1048576.0,
                   rast - rast0,
                   rast == rast0 && live == live0 ? "   nothing rasterized, cache flat"
                                                  : "   <-- treadmill");
        }
        printf("\n%d strings a frame, none of them ever repeated; the first row is\n"
               "the same frame without them, at %.0f fps.\n", N, base);
        a->stress = 0;
        render_set_city(NULL); city_free(a->city); elf_close(a->elf); disasm_close();
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
    }
    if (dovm){
        int rc = vmtest(a, 8, verbose);
        wisp_free(a->wisp); a->wisp = NULL;
        render_set_city(NULL); city_free(a->city); elf_close(a->elf); disasm_close();
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (dotest){
        int rc = selftest(a);
        render_set_city(NULL); city_free(a->city); elf_close(a->elf);
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (shotdir){
        shot_mode(a, shotdir);
        render_set_city(NULL); city_free(a->city); elf_close(a->elf);
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
    }

    SDL_SetRelativeMouseMode(SDL_TRUE);
    /* mouseLook is what the visitor asked for; the pointer grab is only
       actually held while we have focus.  Relative mode is an XGrabPointer
       under X11 and XWayland, and a grab still held by a window the
       compositor is switching away from is how alt-tab locks up a desktop. */
    int mouseLook = 1, running = 1, focused = 1, visible = 1, swallowMotion = 0;
    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    float fpsAcc = 0; int fpsN = 0;

    while (running){
        SDL_Event ev;
        while (SDL_PollEvent(&ev)){
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_WINDOWEVENT){
                switch (ev.window.event){
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    a->winw = ev.window.data1; a->winh = ev.window.data2;
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    focused = 0;
                    SDL_SetRelativeMouseMode(SDL_FALSE);   /* let go of the pointer */
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    focused = 1;
                    swallowMotion = 1;                     /* drop the catch-up jump */
                    if (mouseLook && !a->showBrowser) SDL_SetRelativeMouseMode(SDL_TRUE);
                    break;
                case SDL_WINDOWEVENT_MINIMIZED: case SDL_WINDOWEVENT_HIDDEN:
                    visible = 0;
                    break;
                case SDL_WINDOWEVENT_RESTORED: case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_EXPOSED:
                    visible = 1;
                    break;
                }
            }
            else if (ev.type == SDL_MOUSEMOTION && swallowMotion){
                swallowMotion = 0;
            }
            else if (ev.type == SDL_MOUSEMOTION && mouseLook && !a->showBrowser){
                a->p.yaw   += ev.motion.xrel * 0.0026f;
                a->p.pitch -= ev.motion.yrel * 0.0026f;
                if (a->p.pitch >  1.52f) a->p.pitch =  1.52f;
                if (a->p.pitch < -1.52f) a->p.pitch = -1.52f;
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && !a->showBrowser){
                if (!mouseLook){ mouseLook = 1; SDL_SetRelativeMouseMode(SDL_TRUE); }
                else if (ev.button.button == SDL_BUTTON_LEFT) interact(a);
            }
            else if (ev.type == SDL_TEXTINPUT && a->showBrowser){
                size_t l = strlen(a->bfilter);
                if (l + strlen(ev.text.text) < sizeof a->bfilter - 1){
                    strcat(a->bfilter, ev.text.text);
                    browser_filter(a);
                }
            }
            else if (ev.type == SDL_KEYDOWN){
                SDL_Keycode k = ev.key.keysym.sym;
                int ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
                int shift = ctrl || (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (ctrl && k == SDLK_q){ running = 0; continue; }
                if (a->showBrowser){
                    switch (k){
                    case SDLK_ESCAPE: a->showBrowser = 0; SDL_StopTextInput();
                        SDL_SetRelativeMouseMode(SDL_TRUE); mouseLook = 1; break;
                    case SDLK_UP:   if (a->bsel > 0) a->bsel--; break;
                    case SDLK_DOWN: if (a->bsel + 1 < a->nbfiltered) a->bsel++; break;
                    case SDLK_PAGEUP:   a->bsel -= 12; if (a->bsel < 0) a->bsel = 0; break;
                    case SDLK_PAGEDOWN: a->bsel += 12;
                        if (a->bsel >= a->nbfiltered) a->bsel = a->nbfiltered ? a->nbfiltered-1 : 0;
                        break;
                    case SDLK_HOME: a->bsel = 0; break;
                    case SDLK_END:  a->bsel = a->nbfiltered ? a->nbfiltered - 1 : 0; break;
                    case SDLK_BACKSPACE: {
                        size_t l = strlen(a->bfilter);
                        if (l) a->bfilter[l-1] = 0;
                        browser_filter(a); break; }
                    case SDLK_RETURN: case SDLK_KP_ENTER:
                        browser_open(a);
                        if (!a->showBrowser){ SDL_StopTextInput(); SDL_SetRelativeMouseMode(SDL_TRUE); mouseLook = 1; }
                        break;
                    }
                    continue;
                }
                switch (k){
                case SDLK_ESCAPE:
                    if (a->showHelp) a->showHelp = 0;
                    else if (mouseLook){ mouseLook = 0; SDL_SetRelativeMouseMode(SDL_FALSE); }
                    else running = 0;
                    break;
                case SDLK_f:
                    a->showBrowser = 1; a->showHelp = 0;
                    SDL_SetRelativeMouseMode(SDL_FALSE); mouseLook = 0;
                    SDL_StartTextInput();
                    browser_scan(a, a->bdir[0] ? a->bdir : "/usr/bin");
                    break;
                default: app_key(a, (int)k, shift); break;
                }
            }
        }

        /* A minimised window gets no frame callbacks, so swapping into it can
           block for as long as it stays hidden.  Wait on the event queue
           instead -- that is a wait we control and can be woken from.     */
        if (!visible){
            SDL_WaitEventTimeout(NULL, 120);
            prev = SDL_GetPerformanceCounter();
            continue;
        }

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - prev) / freq);
        prev = now;
        if (dt > 0.1f) dt = 0.1f;
        fpsAcc += dt; fpsN++;
        if (fpsAcc > 0.4f){ a->fps = fpsN / fpsAcc; fpsAcc = 0; fpsN = 0; }

        if (!a->showBrowser){
            const Uint8 *ks = SDL_GetKeyboardState(NULL);
            float speed = (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]) ? 17.0f : 5.4f;
            if (a->p.noclip) speed *= 2.4f;
            float fwd = 0, str = 0;
            if (ks[SDL_SCANCODE_W]) fwd += 1;
            if (ks[SDL_SCANCODE_S]) fwd -= 1;
            if (ks[SDL_SCANCODE_D]) str += 1;
            if (ks[SDL_SCANCODE_A]) str -= 1;
            float n = sqrtf(fwd*fwd + str*str);
            if (n > 0.001f){ fwd /= n; str /= n; }
            player_update(a->city, &a->p, fwd * speed * dt, str * speed * dt,
                          ks[SDL_SCANCODE_SPACE], dt);
        }

        a->now += dt;
        if (a->p.inside >= 0) floor_realize(&a->city->bld[a->p.inside], a->p.floor);
        room_lifecycle(a);
        if (a->wisp){
            Room *wr = wisp_room(a);
            if (wr) wisp_tick(a->wisp, wr, dt);
            if (a->wisp->wants && !a->wisp->asked) wisp_ask(a);
        }
        if (!a->wisp && a->wispAsk[0]){ a->wispAsk[0] = 0; a->wispAskKind = 0; }
        update_prompt(a);
        if (a->msgT > 0) a->msgT -= dt;

        render_scene(a);
        hud_draw(a);
        Uint64 sw0 = SDL_GetPerformanceCounter();
        SDL_GL_SwapWindow(win);
        a->swapMs = (float)((SDL_GetPerformanceCounter() - sw0) / freq * 1000.0);
        if (a->swapMs > a->stallMs) a->stallMs = a->swapMs;
        if (a->swapMs > 250.0f)
            fprintf(stderr, "stall: %.0f ms inside SDL_GL_SwapWindow -- the frame was "
                            "drawn, the compositor or driver held it\n", a->swapMs);

        /* behind another window there is nothing to animate for */
        if (!focused) SDL_Delay(24);
    }

    for (int i = 0; i < a->nbent; i++) free(a->bent[i]);
    free(a->bfiltered);
    wisp_free(a->wisp); a->wisp = NULL;
    render_set_city(NULL);
    city_free(a->city);
    elf_close(a->elf);
    disasm_close();
    text_shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
