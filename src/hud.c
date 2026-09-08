/* hud.c -- the 2D overlay: readouts, minimap, help and the file browser */
#define _GNU_SOURCE
#include "render.h"
#include "text.h"
#include "disasm.h"
#include "wisp.h"
#include "vm.h"
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
    "TEXT STRESS     F2  (200 strings a frame, none repeated: atlas, then cache)",
    "RUN THE CODE    X  starts a wisp in this room -- a CPU state that walks it",
    "                K run / pause   - = slower, faster   N a different run",
    "                Shift+X the whole state sheet;  Tab follows the wisp",
    "STEP BY STEP    T  step over -- one instruction, and a call is run whole",
    "                I  step into -- make the call and stop inside it",
    "                (F10 / F11 do the same; so do . and > where the layout",
    "                 has them -- on an AZERTY keyboard it does not)",
    "                a call being stepped over runs on its own while you stay",
    "                at the call site; it stops again the moment it is back",
    "AT A CALL       the wisp stops and asks:  E go in with it,  O stay behind",
    "                J  catch up with a wisp that went on without you --",
    "                in step-by-step mode that pauses it again where it stands",
    "                --cpu baseline|v2|v3|host picks the CPU its cpuid answers as,",
    "                which is what a codec dispatches on",
    "                P pilot: start at the entry point and let the wisp lead --",
    "                it walks through the ports and you go with it.  It starts",
    "                paused: step it with T and I, or press K to let it run",
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
    "",
    "A WISP  (X)  -- a CPU state walking this room, NOT the program running",
    "  the panel under the ceiling is its registers and flags; the tether says",
    "  which sculpture they belong to, and the cyan ring is where the machine is",
    "  ~ before a value means it was invented -- nothing here was observed,",
    "  and everything computed from it is arithmetically right and meaningless",
    "  the arithmetic itself is real, and checked against hardware: a branch is",
    "  decided by the flags, not by a coin, wherever the flags are known",
    "  a ? in the flag row means the instruction left that flag undefined",
    "  in pilot mode a call is really made, not stepped over: the return address",
    "  goes on the stack and ret brings you back to the room you called from",
    "  it stops at every door and asks whether you are coming; say no and it goes",
    "  on alone, and the readout tells you which room it reached until it is back",
    "  the cards trailing behind it are the last sixteen steps, newest brightest;",
    "  orange means that branch was guessed rather than worked out from the flags",
    "  Tab lists its neighbours with their addresses and classes",
    NULL
};

/* ------------------------------------------------------------------ */
/* the §13 stress test: 200 strings a frame, none of them ever repeated */
/* ------------------------------------------------------------------ */
/* Through the atlas that is one texture bind and ~4000 quads, and the
   string cache never moves.  Through the string cache it is 200
   rasterizations and 200 texture uploads a frame, and they evict the
   room's own labels -- the treadmill cache_sweep() warns about.  F2
   switches between them so the two numbers can be read side by side. */

#define STRESS_N 200

static unsigned s_rng = 2463534242u;
static unsigned srnd(void){ s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

void hud_stress(App *a){
    static const char *REG[16] = { "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
                                   "r8 ","r9 ","r10","r11","r12","r13","r14","r15" };
    const int cols = 5, rows = (STRESS_N + cols - 1) / cols;
    const float px = 14, lh = 17;
    float colw = (a->winw - 40) / (float)cols;
    float y0 = a->winh * 0.5f - rows * lh * 0.5f + 20;

    panel(20, y0 - 8, a->winw - 40, rows * lh + 16, 0.03f, 0.05f, 0.07f, 0.72f);
    glColor4f(0.62f, 0.92f, 0.72f, 0.95f);
    for (int i = 0; i < STRESS_N; i++){
        char t[64];
        snprintf(t, sizeof t, "%s 0x%08x%08x", REG[i & 15], srnd(), srnd());
        float x = 30 + (i / rows) * colw, y = y0 + (i % rows) * lh;
        if (a->stress == 1) text_mono_2d(x, y, px, t);
        else                text_2d(FNT_MONO, x, y, px, t);
    }

    /* the readout that decides the milestone */
    int live; size_t bytes, abytes; int aw, ah; unsigned long rast;
    text_cache_stats(&live, &bytes, &rast);
    text_mono_atlas(&aw, &ah, &abytes);
    char t[200];
    float ty = y0 - 46;
    panel(20, ty - 6, a->winw - 40, 34, 0.06f, 0.04f, 0.03f, 0.80f);
    snprintf(t, sizeof t, "%s  %d strings/frame   cache %d entries, %.1f MiB, "
                          "%lu rasterized   atlas %dx%d %.2f MiB   %.0f fps",
             a->stress == 1 ? "MONO ATLAS  " : "STRING CACHE",
             STRESS_N, live, bytes / 1048576.0, rast, aw, ah,
             abytes / 1048576.0, a->fps);
    glColor4f(a->stress == 1 ? 0.70f : 1.0f, a->stress == 1 ? 0.95f : 0.70f, 0.60f, 1);
    text_mono_2d(30, ty, 16, t);
}

/* ------------------------------------------------------------------ */
/* the X sheet: the whole state, where there is room to read it        */
/* ------------------------------------------------------------------ */
/* The ceiling panel has to fit between the sculptures and the ceiling and
   truncates everything.  This is §1's list in full: every register with
   its provenance and what it points at, the flags, the stack near rsp,
   and the last few memory references.                              */

static const char *PROVMARK = " ~= *!";    /* none invented file derived call live */

/* What the run is doing, in the three words the two readouts share.  A wisp
   stepping over a call is neither paused nor simply running: it is off in
   the callee on your behalf, and it will stop when it is back.       */
static const char *wisp_state(const Wisp *w){
    if (w->done)     return w->why;
    if (w->overCall) return "stepping over a call -- away until it returns";
    if (w->paused)   return w->stepping ? "paused -- T step over, I step into"
                                        : "paused";
    return "running";
}

static void state_sheet(App *a){
    Wisp *w = a->wisp;
    if (!w) return;
    Room *r = &a->city->bld[a->p.inside].rooms[a->p.room];
    const Vm *m = &w->vm;
    const Insn *in = wisp_insn(w, r);

    /* Everything the run has touched, plus the eight that are always worth
       a line; the rest go on one folded line at the end so the sheet is a
       fixed height whatever the architecture.                        */
    int show[VM_NREG], nshow = 0, fold[VM_NREG], nfold = 0;
    for (int i = 0; i < m->nreg; i++){
        if (i < 8 || i == m->pcSlot || i == m->spSlot || m->r[i].prov != PV_NONE)
            show[nshow++] = i;
        else fold[nfold++] = i;
    }
    /* On the left: Tab's detail sheet already owns the right-hand side, and
       these two are the pair you want open together. */
    int nvec = 0;
    for (int i = 0; i < m->nvreg && i < 16; i++) if (m->v[i].prov != PV_NONE) nvec++;
    float pw = 690, px = 20, py = 96;
    float lh = 15, ph = (nshow + (nfold ? 1 : 0) + (nvec ? nvec + 1 : 0) + 17) * lh + 28;
    if (py + ph > a->winh - 104) py = a->winh - 104 - ph;
    if (py < 6) py = 6;
    panel(px, py, pw, ph, 0.03f, 0.05f, 0.07f, 0.90f);

    char t[220], ann[80];
    float y = py + 10;
    int live = w->src == WS_LIVE;
    if (live){
        glColor4f(0.55f, 0.90f, 1.00f, 1);
        snprintf(t, sizeof t, "LIVE -- every value below was read out of a real"
                              " process stopped under gdb");
    } else {
        glColor4f(1.00f, 0.72f, 0.42f, 1);
        snprintf(t, sizeof t, "SIMULATED -- every value below is invented or computed"
                              " from invented values");
    }
    text_mono_2d(px + 14, y, 14, t); y += lh + 3;
    glColor4f(0.90f, 0.92f, 0.96f, 1);
    if (live)
        snprintf(t, sizeof t, "stop %u   pid %d   %s", m->steps,
                 (int)a->livePid, wisp_state(w));
    else
        snprintf(t, sizeof t, "step %u   fuel %d   %.1f/s   cpu %s   %s",
                 m->steps, m->fuel, w->rate, vm_cpu_name(), wisp_state(w));
    text_mono_2d(px + 14, y, 15, t); y += lh + 6;

    /* §4's accounting.  A wisp that quietly skipped a million instructions
       of libc would be lying by omission, so what it stepped over is on
       the sheet beside what it walked.                               */
    if (live && (w->nover || w->nparked)){
        glColor4f(0.72f, 0.86f, 0.96f, 1);
        snprintf(t, sizeof t, "left this file %d time%s: %d call%s run whole and "
                              "returned, %d with nowhere to return to",
                 w->nover + w->nparked, w->nover + w->nparked == 1 ? "" : "s",
                 w->nover, w->nover == 1 ? "" : "s", w->nparked);
        text_mono_2d(px + 14, y, 14, t); y += lh;
        if (w->wentTo[0]){
            snprintf(t, sizeof t, "   the last one ran %s", w->wentTo);
            text_mono_2d(px + 14, y, 14, t); y += lh;
        }
        y += 4;
    }

    /* the registers, with what each one points at */
    for (int j = 0; j < nshow; j++){
        int i = show[j];
        const VReg *g = &m->r[i];
        char mark = PROVMARK[g->prov <= PV_LIVE ? g->prov : 0];
        if (g->prov == PV_NONE){
            glColor4f(0.42f, 0.45f, 0.50f, 1);
            snprintf(t, sizeof t, "%-4s   (untouched)", m->rname[i]);
        } else {
            vm_annotate(m, g->v, ann, sizeof ann);
            if (g->prov == PV_INVENTED)   glColor4f(0.58f, 0.64f, 0.70f, 1);
            else if (g->prov == PV_FILE)  glColor4f(0.70f, 0.95f, 0.80f, 1);
            else if (g->prov == PV_CALL)  glColor4f(0.92f, 0.78f, 0.50f, 1);
            else if (g->prov == PV_LIVE)  glColor4f(0.62f, 0.90f, 1.00f, 1);
            else                          glColor4f(0.84f, 0.88f, 0.94f, 1);
            snprintf(t, sizeof t, "%-4s %c%016llx  %-8s %s", m->rname[i], mark,
                     (unsigned long long)g->v, vm_prov_name(g->prov), ann);
        }
        text_mono_2d(px + 14, y, 14, t); y += lh;
    }
    if (nfold){
        int wn2 = snprintf(t, sizeof t, "untouched  ");
        for (int j = 0; j < nfold && wn2 < 180; j++)
            wn2 += snprintf(t + wn2, sizeof t - (size_t)wn2, "%s ", m->rname[fold[j]]);
        glColor4f(0.42f, 0.45f, 0.50f, 1);
        text_mono_2d(px + 14, y, 14, t); y += lh;
    }
    y += 4;

    glColor4f(0.72f, 0.78f, 0.86f, 1);
    int wn = snprintf(t, sizeof t, "flags  ");
    static const char *FN[VF_COUNT] = { "CF","PF","AF","ZF","SF","OF" };
    for (int i = 0; i < VF_COUNT; i++)
        wn += snprintf(t + wn, sizeof t - (size_t)wn, "%s%c  ", FN[i],
                       (m->flknown & (1u << i)) ? (char)('0' + m->fl[i]) : '?');
    snprintf(t + wn, sizeof t - (size_t)wn, "   ? = the instruction left it undefined");
    text_mono_2d(px + 14, y, 14, t); y += lh + 6;

    /* every vector register the run has touched, as lanes (§4.4) */
    {
        int any = 0;
        for (int i = 0; i < m->nvreg && i < 16; i++){
            if (m->v[i].prov == PV_NONE) continue;
            if (!any){
                glColor4f(0.86f, 0.90f, 0.72f, 1);
                text_mono_2d(px + 14, y, 14, "vector registers"); y += lh;
                any = 1;
            }
            int ew = (i == m->vecSlot && m->vecEw) ? m->vecEw : 2;
            int wn2 = snprintf(t, sizeof t, "  xmm%-2d %c", i,
                               m->v[i].prov == PV_INVENTED ? '~' : ' ');
            for (int o2 = 0; o2 + ew <= 16 && wn2 < 150; o2 += ew){
                unsigned long long uv2 = 0;
                for (int q = ew - 1; q >= 0; q--) uv2 = (uv2 << 8) | m->v[i].b[o2 + q];
                long long lv = (long long)uv2;
                if (ew < 8){ unsigned long long sb = 1ull << (ew * 8 - 1);
                             lv = (long long)((uv2 ^ sb) - sb); }
                wn2 += snprintf(t + wn2, sizeof t - (size_t)wn2, "%lld ", lv);
            }
            glColor4f(i == m->vecSlot ? 1.0f : 0.72f, i == m->vecSlot ? 0.90f : 0.76f,
                      i == m->vecSlot ? 0.55f : 0.92f, 1);
            text_mono_2d(px + 14, y, 14, t); y += lh;
        }
        if (any) y += 4;
    }

    /* The stack near rsp -- the part people actually want to see, and it
       costs nothing extra: it is only memory (§1).
       Under a live wisp the Vm's memory layers are still the simulated
       ones: shadow, then the file, then invention.  Showing invented
       bytes under a banner that says LIVE would be the exact dishonesty
       the banner exists to prevent, so they are withheld until the live
       memory layer lands (docs/live-wisps.md L7).                    */
    glColor4f(0.86f, 0.90f, 0.72f, 1);
    text_mono_2d(px + 14, y, 14, live ? "stack -- read from the process" : "stack");
    y += lh;
    uint64_t sp = vm_slot_get(m, m->spSlot >= 0 ? m->spSlot : 0);
    for (int i = 0; i < 5; i++){
        uint64_t at = sp + (uint64_t)i * 8, v;
        Prov pv;
        if (!vm_peek(m, at, 8, &v, &pv)){
            glColor4f(0.42f, 0.45f, 0.50f, 1);
            snprintf(t, sizeof t, "  rsp+%-3d  0x%llx   (nothing has touched it)",
                     i * 8, (unsigned long long)at);
        } else {
            vm_annotate(m, v, ann, sizeof ann);
            glColor4f(i ? 0.78f : 1.0f, i ? 0.82f : 0.95f, i ? 0.88f : 0.60f, 1);
            snprintf(t, sizeof t, "  rsp+%-3d  %016llx  %s", i * 8,
                     (unsigned long long)v, ann);
        }
        text_mono_2d(px + 14, y, 14, t); y += lh;
    }
    y += 4;

    /* the last few memory references */
    glColor4f(0.86f, 0.90f, 0.72f, 1);
    text_mono_2d(px + 14, y, 14, "last memory references"); y += lh;
    int first = m->nmem > VM_MEMLOG ? m->nmem - VM_MEMLOG : 0;
    for (int k = m->nmem - 1; k >= first && k >= m->nmem - 4; k--){
        int i = k % VM_MEMLOG;
        vm_annotate(m, m->mem[i].addr, ann, sizeof ann);
        glColor4f(0.70f, 0.78f, 0.86f, 1);
        snprintf(t, sizeof t, "  %s%-2d 0x%-14llx = %-16llx %-8s %s",
                 m->mem[i].wr ? "wr" : "rd", m->mem[i].n * 8,
                 (unsigned long long)m->mem[i].addr, (unsigned long long)m->mem[i].val,
                 vm_prov_name(m->mem[i].prov), ann);
        text_mono_2d(px + 14, y, 14, t); y += lh;
    }
    y += 4;

    if (m->lastCall[0]){
        glColor4f(0.92f, 0.72f, 0.90f, 1);
        snprintf(t, sizeof t, "last call  %s  ->  rax = %llx  (stepped over)",
                 m->lastCall, (unsigned long long)m->lastCallRv);
        text_mono_2d(px + 14, y, 14, t); y += lh;
    }
    if (in){
        glColor4f(0.98f, 0.94f, 0.55f, 1);
        snprintf(t, sizeof t, "next       0x%llx  %s %s",
                 (unsigned long long)in->addr, in->mnem, in->ops);
        text_mono_2d(px + 14, y, 14, t);
    }
}

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
    text_mono_2d(20, 62, 14, t);
    /* a frame this program drew but did not get to show: that is the
       compositor or the driver, and it is worth knowing it happened */
    if (a->stallMs > 100.0f){
        snprintf(t, sizeof t, "worst frame handover %.0f ms   (swap %.1f ms now)",
                 a->stallMs, a->swapMs);
        glColor4f(0.98f, 0.62f, 0.45f, 1);
        text_mono_2d(20, 80, 14, t);
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
                if (a->wisp){
                    Wisp *w = a->wisp;
                    const Insn *wi = wisp_insn(w, r);
                    snprintf(t, sizeof t,
                             "wisp: step %u  %.1f/s  %s%s  %s%s",
                             w->vm.steps, w->rate,
                             w->pilot ? "PILOT " : "",
                             wisp_state(w),
                             wi ? "at 0x" : "", "");
                    if (wi){
                        size_t l = strlen(t);
                        snprintf(t + l, sizeof t - l, "%llx  %s %.20s",
                                 (unsigned long long)wi->addr, wi->mnem, wi->ops);
                    }
                    if (w->pilot && w->nrooms > 1){
                        size_t l = strlen(t);
                        snprintf(t + l, sizeof t - l, "   [%d rooms, %d calls deep]",
                                 w->nrooms, w->vm.ncall);
                    }
                    glColor4f(0.55f, 0.92f, 1.0f, 1);
                    text_mono_2d(20, by + 72, 14, t);
                }
                if (a->nearIns >= 0 && a->nearIns < rd->n){
                    Insn *in = &rd->ins[a->nearIns];
                    snprintf(t, sizeof t, "0x%llx   %s %s",
                             (unsigned long long)in->addr, in->mnem, in->ops);
                    float aw = text_mono_width(20.0f, t);
                    panel(10, by - 34, aw + 22, 30, 0.06f, 0.10f, 0.08f, 0.78f);
                    glColor4f(0.98f, 0.94f, 0.55f, 1);
                    text_mono_2d(20, by - 30, 20, t);
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

    /* The wisp is standing at a door waiting for an answer.  Above the
       centre, in its own colour, so it never fights with the port prompt
       at 0.70h.                                                       */
    if (a->wispAsk[0]){
        float ph2 = 20.0f;
        float aw = text_mono_width(ph2, a->wispAsk);
        float bx = w / 2.0f - aw / 2, by2 = h * 0.30f;
        if (bx < 12) { ph2 *= (w - 40.0f) / (aw > 1 ? aw : 1); aw = w - 40.0f; bx = 20; }
        panel(bx - 16, by2 - 10, aw + 32, ph2 + 20, 0.05f, 0.12f, 0.16f, 0.88f);
        line(bx - 16, by2 - 10, bx + aw + 16, by2 - 10, 0.45f, 0.90f, 1.0f, 0.9f);
        line(bx - 16, by2 + ph2 + 10, bx + aw + 16, by2 + ph2 + 10, 0.45f, 0.90f, 1.0f, 0.9f);
        glColor4f(0.72f, 0.96f, 1.0f, 1);
        text_mono_2d(bx, by2, ph2, a->wispAsk);
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

    /* Where the wisp is, when it is not here.  It carries on without you,
       so the one thing you always need is which room it reached.     */
    if (a->wisp && a->wispAway && a->wispBi >= 0 && a->wispBi < c->nbld){
        Building *wb = &c->bld[a->wispBi];
        Room *wr = (a->wispRi >= 0 && a->wispRi < wb->nrooms) ? &wb->rooms[a->wispRi] : NULL;
        const Insn *wi = wr ? wisp_insn(a->wisp, wr) : NULL;
        char t2[260];
        snprintf(t2, sizeof t2,
                 "the wisp is away in  %.24s / %.44s / floor %d%s%llx %s %.16s"
                 "    [J] catch up",
                 wb->label, wr ? wr->title : "?", wr ? wr->floor + 1 : 0,
                 wi ? "   at 0x" : "", (unsigned long long)(wi ? wi->addr : 0),
                 wi ? wi->mnem : "", wi ? wi->ops : "");
        float ph2 = 16.0f, aw = text_mono_width(ph2, t2);
        panel(16, 100, aw + 24, ph2 + 14, 0.10f, 0.06f, 0.02f, 0.82f);
        glColor4f(1.0f, 0.82f, 0.45f, 1);
        text_mono_2d(28, 107, ph2, t2);
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
        /* With a wisp running, this window follows the *machine* rather than
           where you happen to be standing, and marks what it has already
           executed -- which turns the detail sheet into a trace view.  */
        Wisp *w = a->wisp;
        Disasm *rd = w ? (Disasm *)wisp_dis(w, r) : room_dis(r);
        if (rd && rd->n){
            Insn *ins = rd->ins;
            int c = w ? w->cur : (a->nearIns >= 0 ? a->nearIns : 0);
            int off = w ? w->visitOff[w->unit < 0 ? 0 : w->unit] : 0;
            int first = c - 3; if (first < 0) first = 0;
            if (first > rd->n - 7) first = rd->n - 7;
            if (first < 0) first = 0;
            for (int i = 0; i < 7 && first + i < rd->n; i++){
                Insn *in = &ins[first + i];
                int at = first + i;
                int hits = (w && off + at < w->nvisits) ? w->visits[off + at] : 0;
                char ln2[220];
                if (w)
                    snprintf(ln2, sizeof ln2, "%c%4d 0x%08llx  %-8s %-28.28s  %s",
                             at == c ? '>' : (hits ? '.' : ' '), hits,
                             (unsigned long long)in->addr, in->mnem, in->ops,
                             disasm_class_name(in->cls));
                else
                    snprintf(ln2, sizeof ln2, "%c 0x%08llx  %-8s %-30.30s  %s",
                             at == c ? '>' : ' ',
                             (unsigned long long)in->addr, in->mnem, in->ops,
                             disasm_class_name(in->cls));
                if (at == c)     glColor4f(1.0f, 0.94f, 0.55f, 1);
                else if (hits)   glColor4f(0.55f, 0.92f, 1.00f, 1);
                else             glColor4f(0.62f, 0.88f, 0.68f, 1);
                text_mono_2d(px + 16, py + 194 + i * 17, 14, ln2);
            }
        } else {
            char hx[6][96];
            hexdump_lines(r, hx);
            glColor4f(0.60f, 0.90f, 0.66f, 1);
            for (int i = 0; i < 6 && hx[i][0]; i++)
                text_mono_2d(px + 16, py + 196 + i * 17, 14, hx[i]);
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

    if (a->showState && a->wisp && a->p.inside >= 0 && a->p.room >= 0) state_sheet(a);

    if (a->stress) hud_stress(a);

    text_end_2d();
}
