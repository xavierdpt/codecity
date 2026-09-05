/* hud.c -- the 2D overlay: readouts, minimap, help and the file browser */
#define _GNU_SOURCE
#include "render.h"
#include "text.h"
#include "disasm.h"
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void panel(float x, float y, float w, float h, float r, float g, float b, float a){
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

static void line(float x0, float y0, float x1, float y1, float r, float g, float b, float a){
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_LINES); glVertex2f(x0, y0); glVertex2f(x1, y1); glEnd();
    glEnable(GL_TEXTURE_2D);
}

static const float DCOL[DK_COUNT][3] = {
    {0.42f,0.62f,0.86f},{0.78f,0.62f,0.36f},{0.80f,0.44f,0.40f},
    {0.55f,0.78f,0.62f},{0.60f,0.58f,0.66f},{0.70f,0.70f,0.50f} };

static void minimap(App *a, float mx, float my, float ms){
    City *c = a->city;
    Player *p = &a->p;
    panel(mx, my, ms, ms, 0.04f, 0.06f, 0.08f, 0.72f);
    float wx0 = c->minx, wx1 = c->maxx, wz0 = c->minz, wz1 = c->maxz;
    float sw = wx1 - wx0, sh = wz1 - wz0;
    float sc = ms / (sw > sh ? sw : sh);
    float ox = mx + (ms - sw * sc) * 0.5f, oy = my + (ms - sh * sc) * 0.5f;
    glDisable(GL_TEXTURE_2D);
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        float x0 = ox + (bld_x0(b) - wx0) * sc, x1 = ox + (bld_x1(b) - wx0) * sc;
        float z0 = oy + (bld_z0(b) - wz0) * sc, z1 = oy + (bld_z1(b) - wz0) * sc;
        const float *cc = DCOL[b->district];
        float k = (i == p->inside) ? 1.6f : 0.85f;
        glColor4f(cc[0]*k, cc[1]*k, cc[2]*k, 0.95f);
        glBegin(GL_QUADS);
        glVertex2f(x0,z0); glVertex2f(x1,z0); glVertex2f(x1,z1); glVertex2f(x0,z1);
        glEnd();
    }
    float px = ox + (p->x - wx0) * sc, py = oy + (p->z - wz0) * sc;
    glColor4f(1.0f, 0.95f, 0.3f, 1.0f);
    glBegin(GL_TRIANGLES);
    float ca = cosf(p->yaw), sa = sinf(p->yaw);
    glVertex2f(px + ca*7, py + sa*7);
    glVertex2f(px - sa*4 - ca*3, py + ca*4 - sa*3);
    glVertex2f(px + sa*4 - ca*3, py - ca*4 - sa*3);
    glEnd();
    glEnable(GL_TEXTURE_2D);
    /* district key */
    float ky = my + ms + 6;
    for (int d = 0; d < DK_COUNT; d++){
        if (!c->dcount[d]) continue;
        panel(mx, ky + 2, 10, 10, DCOL[d][0], DCOL[d][1], DCOL[d][2], 0.95f);
        char t[64]; snprintf(t, sizeof t, "%s (%d)", district_name(d), c->dcount[d]);
        glColor4f(0.85f, 0.87f, 0.9f, 0.95f);
        text_2d(FNT_MONO, mx + 14, ky, 13, t);
        ky += 16;
    }
}

static void hexdump_lines(const Room *r, char out[6][96]){
    for (int i = 0; i < 6; i++) out[i][0] = 0;
    if (!r->data) return;
    for (int i = 0; i < 6; i++){
        uint64_t off = (uint64_t)i * 16;
        if (off >= r->datasz) break;
        char hx[64] = {0}, as[20] = {0};
        int w = 0, n = 0;
        for (int k = 0; k < 16; k++){
            if (off + (uint64_t)k >= r->datasz){ w += snprintf(hx+w, sizeof hx - (size_t)w, "   "); continue; }
            unsigned char v = r->data[off + k];
            w += snprintf(hx + w, sizeof hx - (size_t)w, "%02x ", v);
            as[n++] = (v >= 32 && v < 127) ? (char)v : '.';
        }
        as[n] = 0;
        snprintf(out[i], 96, "%08llx  %s |%s|", (unsigned long long)(r->fileoff + off), hx, as);
    }
}

static const char *HELP[] = {
    "MOVE            W A S D          SPRINT  Shift        JUMP  Space",
    "LOOK            mouse            RELEASE MOUSE  Esc",
    "ENTER A TOWER   walk in the lit door on its west face",
    "CHANGE FLOOR    climb the spiral stair, or  [  and  ]",
    "INSPECT ROOM    Tab  (full detail for the room you are standing in)",
    "FOLLOW A CALL   look at a port on a code room's far wall, then E or click",
    "FOLLOW A LINK   E at a dependency portal on the plaza",
    "OPEN A FILE     F  (browse /usr/bin, /usr/lib, ...)   Enter to load",
    "MINIMAP  M      WIREFRAME  G      FREE-FLY  V      PLAZA  R",
    "QUIT            Ctrl+Q",
    "",
    "WHAT YOU ARE LOOKING AT",
    "  city      = one ELF file          district = section class",
    "  tower     = one ELF section       floor    = a slice of it",
    "  room      = one symbol, or a slice of a table / of raw bytes",
    "  nameplate = the symbol's name, binding, type, address, size",
    "  door in a party wall = the two symbols are contiguous in the file",
    "  crates    = the bytes of a data object",
    "  green text on the far wall = the actual table entries or strings",
    "",
    "INSIDE A CODE ROOM  (decoded when you walk in, freed when you walk out)",
    "  every sculpture is one machine instruction; its height is its length",
    "  spire = jump   beacon = call   drum = ret   tiers = vector   slab = padding",
    "  cyan wire = a branch forward     orange wire = a branch back, i.e. a loop",
    "  magenta wire = a call within the room    green wire = on to the next alcove",
    "  yellow wire = a branch that leaves, running out to its own port on the far wall",
    "  one port per destination: look at it and press E, or click, to be taken there",
    "  the pulse running along a wire shows which way control flows",
    "  walk up to a sculpture: it is ringed, and the readout below spells it out",
    "  Tab lists its neighbours with their addresses and classes",
    NULL
};

void hud_draw(App *a){
    int w = a->winw, h = a->winh;
    Player *p = &a->p;
    City *c = a->city;
    Elf *e = a->elf;

    text_begin_2d(w, h);

    /* crosshair */
    if (!a->showBrowser){
        line(w/2.0f - 9, h/2.0f, w/2.0f - 3, h/2.0f, 1,1,1,0.75f);
        line(w/2.0f + 3, h/2.0f, w/2.0f + 9, h/2.0f, 1,1,1,0.75f);
        line(w/2.0f, h/2.0f - 9, w/2.0f, h/2.0f - 3, 1,1,1,0.75f);
        line(w/2.0f, h/2.0f + 3, w/2.0f, h/2.0f + 9, 1,1,1,0.75f);
    }

    /* top-left: the file */
    panel(10, 10, 470, a->stallMs > 100.0f ? 94 : 76, 0.03f, 0.05f, 0.07f, 0.66f);
    glColor4f(1.0f, 0.92f, 0.62f, 1);
    text_2d(FNT_SIGN, 20, 15, 24, e->base);
    char t[320];
    snprintf(t, sizeof t, "ELF%d %s  %s  %s  entry 0x%llx", e->is64 ? 64 : 32,
             e->be ? "MSB" : "LSB", elf_machine_name(e->machine),
             elf_type_name(e->etype), (unsigned long long)e->entry);
    glColor4f(0.80f, 0.86f, 0.92f, 1);
    text_2d(FNT_MONO, 20, 43, 15, t);
    snprintf(t, sizeof t, "%d sections  %d symbols  %.0f KiB  %s   %.0f fps",
             e->nsec, e->nsym, e->maplen / 1024.0,
             e->stripped ? "stripped" : "with symtab", a->fps);
    glColor4f(0.62f, 0.68f, 0.74f, 1);
    text_2d(FNT_MONO, 20, 62, 14, t);
    /* a frame this program drew but did not get to show: that is the
       compositor or the driver, and it is worth knowing it happened */
    if (a->stallMs > 100.0f){
        snprintf(t, sizeof t, "worst frame handover %.0f ms   (swap %.1f ms now)",
                 a->stallMs, a->swapMs);
        glColor4f(0.98f, 0.62f, 0.45f, 1);
        text_2d(FNT_MONO, 20, 80, 14, t);
    }

    /* bottom-left: where you are */
    float by = h - 96.0f;
    panel(10, by, 700, 86, 0.03f, 0.05f, 0.07f, 0.66f);
    if (p->inside >= 0){
        Building *b = &c->bld[p->inside];
        snprintf(t, sizeof t, "%s / %s / floor %d of %d",
                 district_name(b->district), b->label, p->floor + 1, b->nfloors);
        glColor4f(0.95f, 0.90f, 0.55f, 1);
        text_2d(FNT_MONO, 20, by + 6, 19, t);
        if (p->room >= 0){
            Room *r = &b->rooms[p->room];
            glColor4f(0.98f, 0.98f, 0.98f, 1);
            text_2d(FNT_MONO, 20, by + 30, 20, r->title);
            Disasm *rd = room_dis(r);
            if (rd){
                snprintf(t, sizeof t, "%d instructions  %d branches inside  %d exits by %d port%s%s",
                         rd->n, rd->nlinks, rd->nexits, rd->nports,
                         rd->nports == 1 ? "" : "s",
                         rd->truncated ? "  (room is deeper than shown)" : "");
                glColor4f(0.70f, 0.85f, 0.75f, 1);
                text_2d(FNT_MONO, 20, by + 54, 15, t);
                if (a->nearIns >= 0 && a->nearIns < rd->n){
                    Insn *in = &rd->ins[a->nearIns];
                    snprintf(t, sizeof t, "0x%llx   %s %s",
                             (unsigned long long)in->addr, in->mnem, in->ops);
                    float aw = text_aspect(FNT_MONO, t) * 20.0f;
                    panel(10, by - 34, aw + 22, 30, 0.06f, 0.10f, 0.08f, 0.78f);
                    glColor4f(0.98f, 0.94f, 0.55f, 1);
                    text_2d(FNT_MONO, 20, by - 30, 20, t);
                }
            } else {
                glColor4f(0.70f, 0.85f, 0.75f, 1);
                text_2d(FNT_MONO, 20, by + 54, 15, r->sub);
            }
        } else {
            glColor4f(0.65f, 0.70f, 0.75f, 1);
            text_2d(FNT_MONO, 20, by + 34, 15,
                    p->x < b->bx ? "stairwell -- the spiral joins every floor"
                                 : "corridor -- each door is a symbol");
        }
    } else {
        float d2best = 1e30f; int nearest = -1;
        for (int i = 0; i < c->nbld; i++){
            Building *b = &c->bld[i];
            float dx = p->x - (bld_x0(b)+bld_x1(b))*0.5f, dz = p->z - (bld_z0(b)+bld_z1(b))*0.5f;
            float d2 = dx*dx + dz*dz;
            if (d2 < d2best){ d2best = d2; nearest = i; }
        }
        glColor4f(0.95f, 0.90f, 0.55f, 1);
        text_2d(FNT_MONO, 20, by + 6, 19, "street level");
        if (nearest >= 0){
            Building *b = &c->bld[nearest];
            snprintf(t, sizeof t, "nearest: %s  (%s, %s)  %.0f m",
                     b->label, district_name(b->district),
                     elf_sectype_name(b->sec->type), sqrtf(d2best));
            glColor4f(0.80f, 0.86f, 0.92f, 1);
            text_2d(FNT_MONO, 20, by + 32, 16, t);
            snprintf(t, sizeof t, "%d floors, %d rooms, %llu bytes at 0x%llx",
                     b->nfloors, b->nrooms, (unsigned long long)b->sec->size,
                     (unsigned long long)b->sec->addr);
            glColor4f(0.62f, 0.68f, 0.74f, 1);
            text_2d(FNT_MONO, 20, by + 56, 15, t);
        }
    }

    /* interaction prompt */
    if (a->prompt[0]){
        float aw = text_aspect(FNT_MONO, a->prompt) * 22.0f;
        panel(w/2.0f - aw/2 - 14, h*0.70f - 6, aw + 28, 36, 0.05f, 0.08f, 0.10f, 0.80f);
        glColor4f(1.0f, 0.95f, 0.55f, 1);
        text_2d(FNT_MONO, w/2.0f - aw/2, h*0.70f, 22, a->prompt);
    }

    /* toast */
    if (a->msgT > 0){
        float al = a->msgT > 1.0f ? 1.0f : a->msgT;
        float aw = text_aspect(FNT_MONO, a->msg) * 18.0f;
        panel(w/2.0f - aw/2 - 12, 96, aw + 24, 30, 0.05f, 0.07f, 0.10f, 0.75f * al);
        glColor4f(0.95f, 0.95f, 0.98f, al);
        text_2d(FNT_MONO, w/2.0f - aw/2, 100, 18, a->msg);
    }

    if (a->showMap) minimap(a, w - 240.0f, 10, 230.0f);

    /* room detail sheet */
    if (a->showDetail && p->inside >= 0 && p->room >= 0){
        Building *b = &c->bld[p->inside];
        Room *r = &b->rooms[p->room];
        float pw = 660, ph = 300, px = w - pw - 20, py = h * 0.5f - ph * 0.5f;
        if (a->showMap) py = 300;
        panel(px, py, pw, ph, 0.04f, 0.05f, 0.07f, 0.86f);
        glColor4f(1.0f, 0.92f, 0.60f, 1);
        text_2d(FNT_SIGN, px + 16, py + 12, 22, r->title);
        glColor4f(0.72f, 0.88f, 0.78f, 1);
        text_2d(FNT_MONO, px + 16, py + 42, 16, r->sub);
        char ln[8][160]; int n = 0;
        snprintf(ln[n++], 160, "section    %s  (%s)", b->sec->name, elf_sectype_name(b->sec->type));
        snprintf(ln[n++], 160, "vaddr      0x%llx", (unsigned long long)r->addr);
        snprintf(ln[n++], 160, "file off   0x%llx", (unsigned long long)r->fileoff);
        snprintf(ln[n++], 160, "size       %llu bytes", (unsigned long long)r->size);
        snprintf(ln[n++], 160, "floor      %d of %d,  row %d",
                 r->floor + 1, b->nfloors, r->row + 1);
        snprintf(ln[n++], 160, "doors      corridor%s%s", r->linkPrev >= 0 ? ", west neighbour" : "",
                 r->linkNext >= 0 ? ", east neighbour" : "");
        glColor4f(0.82f, 0.86f, 0.90f, 1);
        for (int i = 0; i < n; i++) text_2d(FNT_MONO, px + 16, py + 72 + i * 20, 15, ln[i]);
        Disasm *rd = room_dis(r);
        if (rd && rd->n){
            Insn *ins = rd->ins;
            int c = a->nearIns >= 0 ? a->nearIns : 0;
            int first = c - 3; if (first < 0) first = 0;
            if (first > rd->n - 7) first = rd->n - 7;
            if (first < 0) first = 0;
            for (int i = 0; i < 7 && first + i < rd->n; i++){
                Insn *in = &ins[first + i];
                char ln2[220];
                snprintf(ln2, sizeof ln2, "%c 0x%08llx  %-8s %-30.30s  %s",
                         (first + i == c) ? '>' : ' ',
                         (unsigned long long)in->addr, in->mnem, in->ops,
                         disasm_class_name(in->cls));
                if (first + i == c) glColor4f(1.0f, 0.94f, 0.55f, 1);
                else                glColor4f(0.62f, 0.88f, 0.68f, 1);
                text_2d(FNT_MONO, px + 16, py + 194 + i * 17, 14, ln2);
            }
        } else {
            char hx[6][96];
            hexdump_lines(r, hx);
            glColor4f(0.60f, 0.90f, 0.66f, 1);
            for (int i = 0; i < 6 && hx[i][0]; i++)
                text_2d(FNT_MONO, px + 16, py + 196 + i * 17, 14, hx[i]);
        }
    }

    /* help */
    if (a->showHelp){
        float pw = 860, ph = 430, px = w/2.0f - pw/2, py = h/2.0f - ph/2;
        panel(px, py, pw, ph, 0.03f, 0.04f, 0.06f, 0.92f);
        glColor4f(1.0f, 0.92f, 0.60f, 1);
        text_2d(FNT_SIGN, px + 24, py + 16, 26, "CODE CITY -- controls");
        float y = py + 60;
        for (int i = 0; HELP[i]; i++){
            if (!HELP[i][0]){ y += 10; continue; }
            int head = (HELP[i][0] != ' ' && !strchr(HELP[i], '=') && strstr(HELP[i], "WHAT YOU"));
            glColor4f(head ? 1.0f : 0.84f, head ? 0.90f : 0.88f, head ? 0.55f : 0.92f, 1);
            text_2d(FNT_MONO, px + 24, y, 16, HELP[i]);
            y += 21;
        }
        glColor4f(0.6f, 0.65f, 0.7f, 1);
        text_2d(FNT_MONO, px + 24, py + ph - 26, 15, "F1 closes this");
    }

    /* file browser */
    if (a->showBrowser){
        float pw = w * 0.72f, ph = h * 0.78f, px = w/2.0f - pw/2, py = h/2.0f - ph/2;
        panel(px, py, pw, ph, 0.02f, 0.03f, 0.05f, 0.95f);
        glColor4f(1.0f, 0.92f, 0.60f, 1);
        text_2d(FNT_SIGN, px + 20, py + 12, 22, a->bdir);
        char f[128];
        snprintf(f, sizeof f, "filter: %s_", a->bfilter);
        glColor4f(0.70f, 0.90f, 0.75f, 1);
        text_2d(FNT_MONO, px + 20, py + 44, 17, f);
        snprintf(f, sizeof f, "%d entries", a->nbfiltered);
        glColor4f(0.55f, 0.60f, 0.66f, 1);
        text_2d(FNT_MONO, px + pw - 180, py + 44, 17, f);

        int rows = (int)((ph - 110) / 21);
        int first = a->bsel - rows / 2;
        if (first > a->nbfiltered - rows) first = a->nbfiltered - rows;
        if (first < 0) first = 0;
        for (int i = 0; i < rows && first + i < a->nbfiltered; i++){
            int idx = a->bfiltered[first + i];
            float y = py + 74 + i * 21;
            if (first + i == a->bsel){
                panel(px + 14, y - 2, pw - 28, 21, 0.18f, 0.30f, 0.22f, 0.9f);
                glColor4f(1, 1, 0.8f, 1);
            } else glColor4f(0.80f, 0.84f, 0.88f, 1);
            text_2d(FNT_MONO, px + 20, y, 16, a->bent[idx]);
        }
        glColor4f(0.6f, 0.65f, 0.7f, 1);
        text_2d(FNT_MONO, px + 20, py + ph - 28, 15,
                "Up/Down select   Enter open   Backspace edit filter   Esc cancel");
    }

    text_end_2d();
}
