/* main.c -- ELF City: walk around the inside of a binary */
#define _GNU_SOURCE
#include "app.h"
#include "disasm.h"
#include "render.h"
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

static int load_file(App *a, const char *path){
    char err[256] = "";
    Elf *e = elf_open(path, err, sizeof err);
    if (!e){ app_message(a, "%s: %s", path, err[0] ? err : "cannot read"); return 0; }
    City *c = city_build(e);
    if (a->city){ render_set_city(NULL); city_free(a->city); }
    disasm_close();
    if (a->elf) elf_close(a->elf);
    a->elf = e; a->city = c;
    a->lastB = a->lastR = -2; a->nearIns = -1;
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
    if (bi != a->lastB || ri != a->lastR){
        if (bi >= 0 && ri >= 0) city_enter_room(a->city, bi, ri);
        else                    city_leave_room(a->city);
        a->lastB = bi; a->lastR = ri;
        a->nearIns = -1;
    }
    /* which sculpture are we standing next to? */
    a->nearIns = -1;
    if (bi >= 0 && ri >= 0){
        Room *r = &a->city->bld[bi].rooms[ri];
        if (r->dis && r->dis->laid){
            float best = 2.6f * 2.6f;
            for (int i = 0; i < r->dis->n; i++){
                float dx = r->dis->ins[i].x - a->p.x, dz = r->dis->ins[i].z - a->p.z;
                float d2 = dx*dx + dz*dz;
                if (d2 < best){ best = d2; a->nearIns = i; }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* interaction                                                         */
/* ------------------------------------------------------------------ */

static void update_prompt(App *a){
    a->prompt[0] = 0; a->promptKind = 0; a->promptIdx = -1;
    City *c = a->city;
    Player *p = &a->p;
    if (p->inside >= 0) return;
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
        city_enter_room(c, i, got >= 0 ? got : target);
        int decoded = (got >= 0 && b->rooms[got].dis) ? b->rooms[got].dis->n : 0;
        if (disasm_live() > 1){
            printf("FAIL %-16s %d decodings alive at once\n", b->label, disasm_live());
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
        if (decoded) printf("ok   %-16s door -> spiral -> floor %d -> room '%s'  (%d instructions, freed on exit)\n",
                            b->label, f + 1, rm->title, decoded);
        else         printf("ok   %-16s door -> spiral -> floor %d -> room '%s'\n", b->label, f + 1, rm->title);
    }
    if (disasm_live() != 0){ printf("FAIL %d decodings still alive at the end\n", disasm_live()); fail++; }
    printf("%s [%s]: %d towers tested, %d failed\n",
           a->elf->base, disasm_arch_name(), tested, fail);
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
    a->p.inside = bi; a->p.floor = r->floor; a->p.room = ri;
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

    a->showMap = 1; a->showDetail = 1; a->showHelp = 1;
    shot(a, dir, "13-overlays");
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv){
    App *a = &g_app;
    a->winw = 1440; a->winh = 900;
    a->showMap = 1;
    a->p.inside = -1; a->p.room = -1;
    a->lastB = a->lastR = -2; a->nearIns = -1;

    const char *start = NULL, *shotdir = NULL;
    int dotest = 0, dobench = 0, dostats = 0;
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--shot") && i + 1 < argc){ shotdir = argv[++i]; continue; }
        if (!strcmp(argv[i], "--selftest")){ dotest = 1; continue; }
        if (!strcmp(argv[i], "--bench")){ dobench = 1; continue; }
        if (!strcmp(argv[i], "--stats")){ dostats = 1; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")){
            printf("usage: %s [options] [binary-or-library]\n"
                   "\n"
                   "  Walk around an ELF file as if it were a city.  Defaults to /bin/ls;\n"
                   "  press F inside the app to browse for another file.\n"
                   "\n"
                   "  --shot DIR    render a set of canned viewpoints into DIR as .ppm and exit\n"
                   "  --selftest    headless check that every tower can be entered and climbed\n"
                   "  --stats       print the layout the packer produced, and exit\n"
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

    if (SDL_Init(SDL_INIT_VIDEO) != 0){
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_Window *win = SDL_CreateWindow("ELF City -- a 3D file explorer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, a->winw, a->winh,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | ((shotdir||dotest||dobench) ? SDL_WINDOW_HIDDEN : 0));
    if (!win){
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        win = SDL_CreateWindow("ELF City -- a 3D file explorer",
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
        city_leave_room(c);
        render_set_city(NULL); city_free(a->city); elf_close(a->elf); disasm_close();
        text_shutdown(); SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
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
    int mouseLook = 1, running = 1;
    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    float fpsAcc = 0; int fpsN = 0;

    while (running){
        SDL_Event ev;
        while (SDL_PollEvent(&ev)){
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED){
                a->winw = ev.window.data1; a->winh = ev.window.data2;
            }
            else if (ev.type == SDL_MOUSEMOTION && mouseLook && !a->showBrowser){
                a->p.yaw   += ev.motion.xrel * 0.0026f;
                a->p.pitch -= ev.motion.yrel * 0.0026f;
                if (a->p.pitch >  1.52f) a->p.pitch =  1.52f;
                if (a->p.pitch < -1.52f) a->p.pitch = -1.52f;
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && !mouseLook && !a->showBrowser){
                mouseLook = 1; SDL_SetRelativeMouseMode(SDL_TRUE);
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
                case SDLK_F1: a->showHelp = !a->showHelp; break;
                case SDLK_f:
                    a->showBrowser = 1; a->showHelp = 0;
                    SDL_SetRelativeMouseMode(SDL_FALSE); mouseLook = 0;
                    SDL_StartTextInput();
                    browser_scan(a, a->bdir[0] ? a->bdir : "/usr/bin");
                    break;
                case SDLK_e: interact(a); break;
                case SDLK_TAB: a->showDetail = !a->showDetail; break;
                case SDLK_m: a->showMap = !a->showMap; break;
                case SDLK_g: a->wire = !a->wire; break;
                case SDLK_v: a->p.noclip = !a->p.noclip;
                    app_message(a, a->p.noclip ? "free-fly on" : "free-fly off"); break;
                case SDLK_r: spawn_plaza(a); app_message(a, "back at the plaza"); break;
                case SDLK_LEFTBRACKET:  goto_floor(a, -1); break;
                case SDLK_RIGHTBRACKET: goto_floor(a, +1); break;
                }
            }
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
        update_prompt(a);
        if (a->msgT > 0) a->msgT -= dt;

        render_scene(a);
        hud_draw(a);
        SDL_GL_SwapWindow(win);
    }

    for (int i = 0; i < a->nbent; i++) free(a->bent[i]);
    free(a->bfiltered);
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
