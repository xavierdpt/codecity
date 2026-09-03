/* render.c -- draw the city, the buildings and their interiors */
#define _GNU_SOURCE
#include "render.h"
#include "text.h"
#include <GL/gl.h>
#include <GL/glu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAU 6.28318530718f

static GLuint *g_dl;            /* one display list per building exterior */
static int     g_ndl;
static GLuint  g_dlground;
static City   *g_city;
static GLUquadric *g_q;

static const float SKY_TOP[3] = { 0.30f, 0.46f, 0.68f };
static const float SKY_BOT[3] = { 0.72f, 0.79f, 0.86f };

static float clampf(float v, float a, float b){ return v < a ? a : (v > b ? b : v); }
static unsigned hash2(unsigned a, unsigned b){
    unsigned h = a * 2654435761u ^ (b * 40503u + 0x9e3779b9u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

static void quad3(float ax,float ay,float az, float bx,float by,float bz,
                  float cx,float cy,float cz, float dx,float dy,float dz,
                  float nx,float ny,float nz){
    glNormal3f(nx, ny, nz);
    glVertex3f(ax,ay,az); glVertex3f(bx,by,bz); glVertex3f(cx,cy,cz); glVertex3f(dx,dy,dz);
}

static void draw_box(float x0,float y0,float z0,float x1,float y1,float z1){
    glBegin(GL_QUADS);
    quad3(x0,y0,z1, x1,y0,z1, x1,y1,z1, x0,y1,z1,  0,0,1);
    quad3(x1,y0,z0, x0,y0,z0, x0,y1,z0, x1,y1,z0,  0,0,-1);
    quad3(x1,y0,z1, x1,y0,z0, x1,y1,z0, x1,y1,z1,  1,0,0);
    quad3(x0,y0,z0, x0,y0,z1, x0,y1,z1, x0,y1,z0, -1,0,0);
    quad3(x0,y1,z1, x1,y1,z1, x1,y1,z0, x0,y1,z0,  0,1,0);
    quad3(x0,y0,z0, x1,y0,z0, x1,y0,z1, x0,y0,z1,  0,-1,0);
    glEnd();
}

static void byte_color(unsigned char v, float *r, float *g, float *b){
    if (v == 0){ *r = 0.16f; *g = 0.20f; *b = 0.30f; return; }
    if (v == 0xff){ *r = 0.90f; *g = 0.30f; *b = 0.25f; return; }
    if (v >= 32 && v < 127){ *r = 0.36f; *g = 0.80f; *b = 0.46f; return; }
    float t = v / 255.0f;
    *r = 0.30f + 0.65f * t; *g = 0.42f + 0.30f * (1.0f - t); *b = 0.85f - 0.45f * t;
}

/* ------------------------------------------------------------------ */
/* material palette                                                    */
/* ------------------------------------------------------------------ */

typedef struct { const Building *b; float dim; int interior; } DrawCtx;

static void mat_color(const DrawCtx *d, int mat){
    const float *c = d->b->col;
    switch (mat){
    case M_SHELL:
        if (d->interior) glColor3f(0.74f + c[0]*0.10f, 0.73f + c[1]*0.10f, 0.71f + c[2]*0.10f);
        else glColor3f(c[0]*0.92f, c[1]*0.92f, c[2]*0.92f);
        break;
    case M_ROOF:   glColor3f(c[0]*0.55f, c[1]*0.55f, c[2]*0.58f); break;
    case M_WALL:   glColor3f(0.80f, 0.79f, 0.76f); break;
    case M_PART:   glColor3f(0.72f, 0.71f, 0.69f); break;
    case M_LINTEL: glColor3f(0.62f, 0.61f, 0.60f); break;
    case M_CORE:   glColor3f(c[0]*0.55f + 0.25f, c[1]*0.55f + 0.25f, c[2]*0.55f + 0.25f); break;
    case M_COLUMN: glColor3f(0.52f, 0.50f, 0.55f); break;
    case M_SLAB:   glColor3f(0.33f, 0.33f, 0.36f); break;
    default:       glColor3f(0.7f, 0.7f, 0.7f);
    }
}

static void sink_draw(void *ud, const Box3 *b){
    DrawCtx *d = ud;
    mat_color(d, b->mat);
    draw_box(b->x0, b->y0, b->z0, b->x1, b->y1, b->z1);
}

/* ------------------------------------------------------------------ */
/* building exterior (baked into a display list)                       */
/* ------------------------------------------------------------------ */

static void bake_exterior(const Building *b){
    DrawCtx d = { b, 1.0f, 0 };
    emit_shell(b, sink_draw, &d);

    /* entrance surround */
    float x0 = bld_x0(b);
    glColor3f(0.95f, 0.86f, 0.45f);
    draw_box(x0 - 0.30f, 0, b->bz - DOOR_W*0.5f - 0.35f, x0 + 0.02f, DOOR_H + 0.35f, b->bz - DOOR_W*0.5f);
    draw_box(x0 - 0.30f, 0, b->bz + DOOR_W*0.5f, x0 + 0.02f, DOOR_H + 0.35f, b->bz + DOOR_W*0.5f + 0.35f);
    draw_box(x0 - 0.30f, DOOR_H, b->bz - DOOR_W*0.5f - 0.35f, x0 + 0.02f, DOOR_H + 0.35f, b->bz + DOOR_W*0.5f + 0.35f);
    /* stoop */
    glColor3f(0.55f, 0.54f, 0.52f);
    draw_box(x0 - 2.2f, -0.10f, b->bz - 2.2f, x0, 0.02f, b->bz + 2.2f);

    /* windows: one grid per floor, on all four faces */
    float x1 = bld_x1(b), z0 = bld_z0(b), z1 = bld_z1(b);
    unsigned seed = (unsigned)(b->sec ? b->sec->index : 0) + 7u;
    float wh = 1.9f, sill = 1.1f, pitch = 3.4f;
    glBegin(GL_QUADS);
    for (int f = 0; f < b->nfloors; f++){
        float y = f * FLOOR_H + sill;
        int ncx = (int)((x1 - x0 - 2.0f) / pitch);
        for (int i = 0; i < ncx; i++){
            float wx = x0 + 1.0f + i * pitch, wx2 = wx + pitch * 0.62f;
            for (int side = 0; side < 2; side++){
                unsigned h = hash2(seed * 31u + (unsigned)f, (unsigned)i * 2u + (unsigned)side);
                int lit = (h & 7) < 3;
                float zz = side ? z1 + 0.02f : z0 - 0.02f;
                if (lit) glColor3f(0.99f, 0.90f, 0.62f); else glColor3f(0.10f, 0.16f, 0.22f);
                quad3(wx, y, zz, wx2, y, zz, wx2, y + wh, zz, wx, y + wh, zz, 0, 0, side ? 1 : -1);
            }
        }
        int ncz = (int)((z1 - z0 - 2.0f) / pitch);
        for (int i = 0; i < ncz; i++){
            float wz = z0 + 1.0f + i * pitch, wz2 = wz + pitch * 0.62f;
            for (int side = 0; side < 2; side++){
                unsigned h = hash2(seed * 17u + (unsigned)f, (unsigned)i * 3u + (unsigned)side + 99u);
                int lit = (h & 7) < 3;
                float xx = side ? x1 + 0.02f : x0 - 0.02f;
                if (lit) glColor3f(0.99f, 0.90f, 0.62f); else glColor3f(0.10f, 0.16f, 0.22f);
                quad3(xx, y, wz, xx, y, wz2, xx, y + wh, wz2, xx, y + wh, wz, side ? 1 : -1, 0, 0);
            }
        }
    }
    glEnd();
}

void render_set_city(City *c){
    if (g_dl){ for (int i = 0; i < g_ndl; i++) glDeleteLists(g_dl[i], 1); free(g_dl); g_dl = NULL; }
    if (g_dlground){ glDeleteLists(g_dlground, 1); g_dlground = 0; }
    g_city = c;
    if (!c) return;
    g_ndl = c->nbld;
    g_dl = calloc((size_t)(g_ndl ? g_ndl : 1), sizeof(GLuint));
    for (int i = 0; i < g_ndl; i++){
        g_dl[i] = glGenLists(1);
        glNewList(g_dl[i], GL_COMPILE);
        bake_exterior(&c->bld[i]);
        glEndList();
    }

    /* ground, district plots, plaza */
    g_dlground = glGenLists(1);
    glNewList(g_dlground, GL_COMPILE);
    float m = 400.0f;
    glBegin(GL_QUADS);
    glColor3f(0.20f, 0.24f, 0.20f);
    quad3(c->minx - m, -0.6f, c->minz - m, c->maxx + m, -0.6f, c->minz - m,
          c->maxx + m, -0.6f, c->maxz + m, c->minx - m, -0.6f, c->maxz + m, 0, 1, 0);
    glEnd();
    static const float PLOT[DK_COUNT][3] = {
        {0.20f,0.23f,0.29f},{0.29f,0.26f,0.20f},{0.30f,0.21f,0.20f},
        {0.20f,0.28f,0.23f},{0.24f,0.24f,0.26f},{0.26f,0.26f,0.21f} };
    for (int d = 0; d < DK_COUNT; d++){
        if (!c->dcount[d]) continue;
        float ax = c->dmin[d][0], az = c->dmin[d][1], bx = c->dmax[d][0], bz = c->dmax[d][1];
        glBegin(GL_QUADS);
        glColor3fv(PLOT[d]);
        quad3(ax, -0.5f, az, bx, -0.5f, az, bx, -0.5f, bz, ax, -0.5f, bz, 0, 1, 0);
        glEnd();
        glColor3f(0.85f, 0.82f, 0.55f);            /* kerb */
        draw_box(ax - 0.6f, -0.55f, az - 0.6f, bx + 0.6f, -0.38f, az);
        draw_box(ax - 0.6f, -0.55f, bz, bx + 0.6f, -0.38f, bz + 0.6f);
        draw_box(ax - 0.6f, -0.55f, az, ax, -0.38f, bz);
        draw_box(bx, -0.55f, az, bx + 0.6f, -0.38f, bz);
    }
    /* plaza disc */
    glColor3f(0.42f, 0.40f, 0.38f);
    glPushMatrix(); glTranslatef(0, -0.48f, 0); glRotatef(-90, 1, 0, 0);
    gluDisk(g_q, 0, c->plazaR, 48, 1);
    glPopMatrix();
    glEndList();
}

/* ------------------------------------------------------------------ */
/* spiral stair                                                        */
/* ------------------------------------------------------------------ */

static void draw_stair(const Building *b, int f0, int f1){
    float cx, cz; stair_center(b, &cx, &cz);
    if (f0 < 0) f0 = 0;
    if (f1 > b->nfloors - 1) f1 = b->nfloors - 1;
    float t = 0.16f;
    glBegin(GL_QUADS);
    for (int k = f0; k <= f1; k++){
        for (int j = 0; j < STEPS_PER_FLOOR; j++){
            float y = ((float)k + (float)j / STEPS_PER_FLOOR) * FLOOR_H;
            float a0 = (float)j / STEPS_PER_FLOOR * TAU;
            float a1 = (float)(j + 1) / STEPS_PER_FLOOR * TAU;
            if (j & 1) glColor3f(0.58f, 0.55f, 0.50f); else glColor3f(0.64f, 0.61f, 0.56f);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float ix0 = cx + STAIR_RIN*c0, iz0 = cz + STAIR_RIN*s0;
            float ox0 = cx + STAIR_ROUT*c0, oz0 = cz + STAIR_ROUT*s0;
            float ix1 = cx + STAIR_RIN*c1, iz1 = cz + STAIR_RIN*s1;
            float ox1 = cx + STAIR_ROUT*c1, oz1 = cz + STAIR_ROUT*s1;
            quad3(ix0,y,iz0, ox0,y,oz0, ox1,y,oz1, ix1,y,iz1, 0,1,0);              /* tread */
            glColor3f(0.42f, 0.40f, 0.37f);
            quad3(ix0,y-t,iz0, ix1,y-t,iz1, ox1,y-t,oz1, ox0,y-t,oz0, 0,-1,0);
            quad3(ix0,y-t,iz0, ox0,y-t,oz0, ox0,y,oz0, ix0,y,iz0, s0,0,-c0);        /* riser */
            quad3(ox0,y-t,oz0, ox1,y-t,oz1, ox1,y,oz1, ox0,y,oz0, c0,0,s0);         /* rim */
        }
    }
    glEnd();
    /* handrail posts */
    glColor3f(0.80f, 0.78f, 0.40f);
    for (int k = f0; k <= f1; k++)
        for (int j = 0; j < STEPS_PER_FLOOR; j += 2){
            float y = ((float)k + (float)j / STEPS_PER_FLOOR) * FLOOR_H;
            float a = (float)j / STEPS_PER_FLOOR * TAU;
            float x = cx + (STAIR_ROUT - 0.12f) * cosf(a), z = cz + (STAIR_ROUT - 0.12f) * sinf(a);
            draw_box(x - 0.05f, y, z - 0.05f, x + 0.05f, y + 0.95f, z + 0.05f);
        }
    /* newel column */
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 0.93f, 0.70f);
    for (int k = f0; k <= f1 + 1 && k < b->nfloors; k++)
        draw_box(cx - 0.13f, k * FLOOR_H + FLOOR_H - 0.42f, cz - 0.13f,
                 cx + 0.13f, k * FLOOR_H + FLOOR_H - 0.22f, cz + 0.13f);
    glEnable(GL_LIGHTING);
    glColor3f(0.50f, 0.48f, 0.52f);
    glPushMatrix();
    glTranslatef(cx, 0, cz); glRotatef(-90, 1, 0, 0);
    gluCylinder(g_q, STAIR_RIN * 0.85f, STAIR_RIN * 0.85f, b->nfloors * FLOOR_H, 16, 1);
    glPopMatrix();
}

/* ------------------------------------------------------------------ */
/* room contents                                                       */
/* ------------------------------------------------------------------ */

/* geometry of a room's inner box */
static void room_bounds(const Building *b, const Room *r, float *x0, float *x1,
                        float *zNear, float *zFar, float *nz){
    float cw = CORR_W * 0.5f;
    *x0 = b->bx + r->x0 + WALL_T; *x1 = b->bx + r->x1 - WALL_T;
    if (r->side == 0){ *zFar = bld_z0(b) + WALL_T; *zNear = b->bz - cw - WALL_T; *nz = 1.0f; }
    else             { *zFar = bld_z1(b) - WALL_T; *zNear = b->bz + cw + WALL_T; *nz = -1.0f; }
}

static void wall_text(int font, float x, float y, float z, float nz, float h,
                      int anchor, const char *s){
    float p[3] = { x, y, z };
    float r[3] = { nz > 0 ? 1.0f : -1.0f, 0, 0 };
    float u[3] = { 0, 1, 0 };
    text_3d(font, p, r, u, h, anchor, s);
}


/* ------------------------------------------------------------------ */
/* code rooms: the instructions as sculptures, wired by their branches */
/* ------------------------------------------------------------------ */

static const float ICOL[IC_COUNT][3] = {
    { 0.70f, 0.68f, 0.62f },   /* other    */
    { 0.35f, 0.80f, 0.45f },   /* move     */
    { 0.30f, 0.72f, 0.72f },   /* stack    */
    { 0.36f, 0.56f, 0.92f },   /* arith    */
    { 0.55f, 0.48f, 0.90f },   /* logic    */
    { 0.90f, 0.82f, 0.35f },   /* compare  */
    { 0.96f, 0.55f, 0.20f },   /* jump     */
    { 0.95f, 0.72f, 0.30f },   /* branch   */
    { 0.92f, 0.36f, 0.72f },   /* call     */
    { 0.90f, 0.30f, 0.30f },   /* return   */
    { 0.72f, 0.42f, 0.95f },   /* vector   */
    { 0.42f, 0.44f, 0.48f },   /* padding  */
    { 0.30f, 0.88f, 0.92f },   /* system   */
};

static float class_lift(int c){
    switch (c){
    case IC_CALL:   return 0.55f;
    case IC_JUMP:   return 0.34f;
    case IC_CJUMP:  return 0.26f;
    case IC_SYSCALL:return 0.62f;
    case IC_RET:    return 0.02f;
    case IC_NOP:    return -0.14f;
    default:        return 0.0f;
    }
}

/* serpentine ribbon across the room floor, so consecutive instructions touch */
static void layout_code(Disasm *d, float x0, float x1, float zn, float zf, float base){
    float dir = (zf > zn) ? 1.0f : -1.0f;
    float usableW = x1 - x0 - 0.9f;
    float usableD = fabsf(zf - zn) - 3.0f;
    if (usableW < 1.0f) usableW = 1.0f;
    if (usableD < 1.0f) usableD = 1.0f;
    float pitch = 0.80f;
    int cols = 1, rows = 1;
    for (;;){
        cols = (int)(usableW / pitch); if (cols < 1) cols = 1;
        rows = (int)(usableD / pitch); if (rows < 1) rows = 1;
        if (cols * rows >= d->n || pitch <= 0.235f) break;
        pitch *= 0.93f;
    }
    d->pitch = pitch; d->cols = cols; d->rows = rows;
    float gx = x0 + 0.45f + (usableW - cols * pitch) * 0.5f;
    for (int i = 0; i < d->n; i++){
        Insn *t = &d->ins[i];
        int row = i / cols, col = i % cols;
        if (row & 1) col = cols - 1 - col;               /* boustrophedon */
        if (row >= rows) row = rows - 1;                 /* overflow piles on the last row */
        t->x = gx + (col + 0.5f) * pitch;
        t->z = zn + dir * (2.3f + (row + 0.5f) * pitch);
        t->y = base;
        t->h = clampf(0.22f + t->len * 0.075f + class_lift(t->cls), 0.06f, 1.55f);
    }
    d->laid = 1;
}

/* P(t) on the quadratic arc from a to b, lifted in the middle */
static void arc_point(const float a[3], const float b[3], float lift, float t, float out[3]){
    float c[3] = { (a[0]+b[0])*0.5f, (a[1]+b[1])*0.5f + lift, (a[2]+b[2])*0.5f };
    float u = 1.0f - t;
    for (int i = 0; i < 3; i++)
        out[i] = u*u*a[i] + 2.0f*u*t*c[i] + t*t*b[i];
}

static void draw_arc(const float a[3], const float b[3], float lift, float now, float phase){
    float p[3];
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= 16; i++){
        arc_point(a, b, lift, i / 16.0f, p);
        glVertex3fv(p);
    }
    glEnd();
    /* a pulse running along the wire, so the flow is visible */
    float t = fmodf(now * 0.42f + phase, 1.0f);
    arc_point(a, b, lift, t, p);
    float s = 0.055f;
    draw_box(p[0]-s, p[1]-s, p[2]-s, p[0]+s, p[1]+s, p[2]+s);
}

static void sculpture(const Insn *t, float pitch, int detail){
    float w = pitch * 0.34f;
    float x = t->x, y = t->y, z = t->z, h = t->h;
    if (!detail){
        draw_box(x - w, y, z - w, x + w, y + h, z + w);
        return;
    }
    switch (t->cls){
    case IC_CALL:                                    /* a beacon on a shaft */
        draw_box(x - w*0.45f, y, z - w*0.45f, x + w*0.45f, y + h, z + w*0.45f);
        glPushMatrix(); glTranslatef(x, y + h + w*0.55f, z);
        gluSphere(g_q, w * 0.72f, 10, 8);
        glPopMatrix();
        break;
    case IC_JUMP: case IC_CJUMP:                     /* a spire pointing on */
        glPushMatrix(); glTranslatef(x, y, z); glRotatef(-90, 1, 0, 0);
        gluCylinder(g_q, w, w * 0.10f, h, 8, 1);
        glPopMatrix();
        break;
    case IC_RET:                                     /* a drum, the end of the line */
        glPushMatrix(); glTranslatef(x, y, z); glRotatef(-90, 1, 0, 0);
        gluCylinder(g_q, w * 1.25f, w * 1.25f, h, 10, 1);
        gluDisk(g_q, 0, w * 1.25f, 10, 1);
        glPopMatrix();
        glPushMatrix(); glTranslatef(x, y + h, z); glRotatef(-90, 1, 0, 0);
        gluDisk(g_q, 0, w * 1.25f, 10, 1);
        glPopMatrix();
        break;
    case IC_VECTOR:                                  /* stacked lanes */
        for (int k = 0; k < 3; k++){
            float ww = w * (1.0f - k * 0.22f);
            draw_box(x - ww, y + h * k / 3.0f, z - ww, x + ww, y + h * (k + 1) / 3.0f, z + ww);
        }
        break;
    case IC_SYSCALL:
        glPushMatrix(); glTranslatef(x, y, z); glRotatef(-90, 1, 0, 0);
        gluCylinder(g_q, w * 0.35f, w * 0.35f, h, 8, 1);
        glPopMatrix();
        glPushMatrix(); glTranslatef(x, y + h, z);
        gluSphere(g_q, w * 0.85f, 10, 8);
        glPopMatrix();
        break;
    default:
        draw_box(x - w, y, z - w, x + w, y + h, z + w);
    }
}

static void draw_code_room(const Building *b, const Room *r, App *a,
                           float x0, float x1, float zn, float zf, float nz, float base){
    Disasm *d = r->dis;
    if (!d || !d->n) return;
    if (!d->laid) layout_code(d, x0, x1, zn, zf, base);

    float px = a->p.x, py = a->p.y, pz = a->p.z;
    float now = a->now;

    /* the plinth the whole thing stands on */
    glColor3f(0.26f, 0.26f, 0.29f);
    float pz0 = zn + (nz > 0 ? -2.0f : 2.0f), pz1 = zf;
    if (pz0 > pz1){ float t = pz0; pz0 = pz1; pz1 = t; }
    draw_box(x0 + 0.2f, base, pz0, x1 - 0.2f, base + 0.05f, pz1);

    /* the instructions themselves */
    for (int i = 0; i < d->n; i++){
        const Insn *t = &d->ins[i];
        float dx = t->x - px, dz = t->z - pz;
        float dd = dx*dx + dz*dz;
        if (dd > 900.0f) continue;
        glColor3fv(ICOL[t->cls < IC_COUNT ? t->cls : 0]);
        sculpture(t, d->pitch, dd < 90.0f);
    }

    /* the thread of execution along the floor */
    glDisable(GL_LIGHTING);
    glColor4f(0.55f, 0.58f, 0.62f, 0.9f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < d->n; i++) glVertex3f(d->ins[i].x, base + 0.075f, d->ins[i].z);
    glEnd();

    /* branches that land inside the room: a wire from one sculpture to another */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(2.0f);
    for (int i = 0; i < d->n; i++){
        const Insn *t = &d->ins[i];
        if (t->target < 0) continue;
        const Insn *u = &d->ins[t->target];
        float dx = t->x - px, dz = t->z - pz;
        if (dx*dx + dz*dz > 1000.0f) continue;
        float A[3] = { t->x, t->y + t->h + 0.06f, t->z };
        float B[3] = { u->x, u->y + u->h + 0.06f, u->z };
        float len = fabsf(A[0]-B[0]) + fabsf(A[2]-B[2]);
        float lift = clampf(0.45f + len * 0.14f, 0.35f, 2.4f);
        if (t->cls == IC_CALL)          glColor4f(0.95f, 0.45f, 0.85f, 0.92f);
        else if (t->target > i)         glColor4f(0.35f, 0.88f, 0.98f, 0.85f);   /* onward */
        else                            glColor4f(1.00f, 0.58f, 0.22f, 0.92f);   /* a loop */
        draw_arc(A, B, lift, now, (float)i * 0.137f);
    }

    /* branches that leave the room: a wire out to the lintel over the door */
    int labelled = 0;
    float doorx = (x0 + x1) * 0.5f;
    float doorz = zn + (nz > 0 ? -1.05f : 1.05f);
    float B[3] = { doorx, base + 2.45f, doorz };
    for (int i = 0; i < d->n && labelled < 12; i++){
        const Insn *t = &d->ins[i];
        if (t->target >= 0 || !t->taddr) continue;
        float dx = t->x - px, dz = t->z - pz;
        if (dx*dx + dz*dz > 150.0f) continue;
        labelled++;
        float A[3] = { t->x, t->y + t->h + 0.06f, t->z };
        glColor4f(0.90f, 0.88f, 0.42f, 0.62f);
        draw_arc(A, B, 0.55f, now, (float)i * 0.211f);
    }
    glLineWidth(1.0f);
    if (labelled){                       /* the terminal the wires run to */
        glColor4f(0.62f, 0.60f, 0.30f, 1.0f);
        draw_box(B[0]-0.22f, B[1]-0.10f, B[2]-0.10f, B[0]+0.22f, B[1]+0.10f, B[2]+0.10f);
    }

    /* what the outgoing wires point at */
    if (labelled){
        glEnable(GL_TEXTURE_2D);
        int shown = 0;
        for (int i = 0; i < d->n && shown < 6; i++){
            const Insn *t = &d->ins[i];
            if (t->target >= 0 || !t->taddr) continue;
            float dx = t->x - px, dz = t->z - pz;
            if (dx*dx + dz*dz > 90.0f) continue;
            uint64_t off = 0;
            const char *nm = elf_sym_at(b->elf, t->taddr, &off);
            char lab[160];
            if (nm && nm[0]){
                if (off) snprintf(lab, sizeof lab, "%s %s+%llu", t->mnem, nm, (unsigned long long)off);
                else     snprintf(lab, sizeof lab, "%s %s", t->mnem, nm);
            } else snprintf(lab, sizeof lab, "%s 0x%llx", t->mnem, (unsigned long long)t->taddr);
            glColor3f(0.98f, 0.96f, 0.62f);
            text_billboard(FNT_MONO, doorx, base + 2.72f + shown * 0.24f, doorz + (nz > 0 ? 0.30f : -0.30f),
                           0.21f, lab);
            shown++;
        }
        glDisable(GL_TEXTURE_2D);
    }

    /* label the control flow -- those are the signposts; plus whatever you
       are standing next to, whatever it is */
    glEnable(GL_TEXTURE_2D);
    int nlab = 0;
    for (int i = 0; i < d->n && nlab < 30; i++){
        const Insn *t = &d->ins[i];
        int hot = (i == a->nearIns);
        int flow = (t->cls == IC_JUMP || t->cls == IC_CJUMP || t->cls == IC_CALL ||
                    t->cls == IC_RET  || t->cls == IC_SYSCALL);
        if (!hot && !flow) continue;
        float dx = t->x - px, dz = t->z - pz;
        float dd = dx*dx + dz*dz;
        if (!hot && dd > 64.0f) continue;
        nlab++;
        glColor3f(hot ? 1.0f : 0.92f, hot ? 0.95f : 0.93f, hot ? 0.45f : 0.88f);
        text_billboard(FNT_MONO, t->x, t->y + t->h + 0.17f, t->z, hot ? 0.19f : 0.105f, t->mnem);
        if (hot){                        /* a ring round the one you can read */
            glDisable(GL_TEXTURE_2D);
            glColor4f(1.0f, 0.90f, 0.35f, 0.85f);
            glBegin(GL_LINE_LOOP);
            for (int k = 0; k < 20; k++){
                float ang = k / 20.0f * 6.2831853f;
                glVertex3f(t->x + cosf(ang) * 0.34f, t->y + 0.10f, t->z + sinf(ang) * 0.34f);
            }
            glEnd();
            glEnable(GL_TEXTURE_2D);
        }
    }
    glDisable(GL_TEXTURE_2D);
    (void)py;
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
}

static void door_frame(float x, float z, float base, float dw, int side){
    float t = 0.09f;
    float z0 = z - t, z1 = z + t;
    glColor3f(0.86f, 0.80f, 0.52f);
    draw_box(x - dw*0.5f - 0.14f, base, z0, x - dw*0.5f, base + DOOR_H + 0.14f, z1);
    draw_box(x + dw*0.5f, base, z0, x + dw*0.5f + 0.14f, base + DOOR_H + 0.14f, z1);
    draw_box(x - dw*0.5f - 0.14f, base + DOOR_H, z0, x + dw*0.5f + 0.14f, base + DOOR_H + 0.14f, z1);
    (void)side;
}

static void draw_room(const Building *b, const Room *r, int highlight, App *a){
    float px = a->p.x, pz = a->p.z;
    float x0, x1, zn, zf, nz;
    room_bounds(b, r, &x0, &x1, &zn, &zf, &nz);
    float base = r->floor * FLOOR_H;
    float w = x1 - x0;
    float mid = (x0 + x1) * 0.5f;
    float depth = fabsf(zn - zf);
    float zmid = (zn + zf) * 0.5f;
    float dist = fabsf(px - mid) + fabsf(pz - zmid);

    /* doorway frame + nameplate above it, facing the corridor */
    float plateZ = (r->side == 0) ? b->bz - CORR_W * 0.5f + 0.02f : b->bz + CORR_W * 0.5f - 0.02f;
    float pnz = (r->side == 0) ? 1.0f : -1.0f;
    float dwid = clampf(x1 - x0 + 2*WALL_T - 1.0f, 0.8f, DOOR_W);
    door_frame(mid, plateZ - pnz * 0.16f, base, dwid, r->side);
    glDisable(GL_LIGHTING);
    glColor3f(highlight ? 0.22f : 0.13f, highlight ? 0.26f : 0.14f, highlight ? 0.16f : 0.15f);
    float pw = clampf(w * 0.86f, 1.0f, 9.0f);
    glBegin(GL_QUADS);
    quad3(mid - pw*0.5f, base + 2.82f, plateZ, mid + pw*0.5f, base + 2.82f, plateZ,
          mid + pw*0.5f, base + 3.42f, plateZ, mid - pw*0.5f, base + 3.42f, plateZ, 0, 0, pnz);
    glEnd();
    glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
    glColor3f(highlight ? 1.0f : 0.86f, highlight ? 0.95f : 0.88f, highlight ? 0.55f : 0.80f);
    {
        float ah = text_aspect(FNT_SIGN, r->title);
        float th = clampf(pw / (ah > 0.01f ? ah : 1.0f), 0.10f, 0.30f);
        wall_text(FNT_SIGN, mid, base + 3.04f, plateZ + pnz * 0.01f, pnz, th, 1, r->title);
        glColor3f(0.68f, 0.74f, 0.72f);
        ah = text_aspect(FNT_MONO, r->sub);
        th = clampf(pw / (ah > 0.01f ? ah : 1.0f), 0.07f, 0.16f);
        wall_text(FNT_MONO, mid, base + 2.86f, plateZ + pnz * 0.01f, pnz, th, 1, r->sub);
    }
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);

    if (dist > 46.0f) return;              /* too far to bother with contents */

    if (r->dis){                            /* decoded because we are standing in it */
        draw_code_room(b, r, a, x0, x1, zn, zf, nz, base);
        return;
    }

    switch (r->kind){
    case RT_FUNC: {
        /* the machine code laid out as a floor of byte columns */
        float pitch = 0.46f;
        int cols = (int)((w - 0.6f) / pitch); if (cols > 44) cols = 44; if (cols < 3) cols = 3;
        int rows = (int)((depth - 3.2f) / pitch); if (rows > 8) rows = 8; if (rows < 1) rows = 1;
        uint64_t avail = r->datasz;
        uint64_t want = (uint64_t)cols * rows;
        if (avail && avail < want) want = avail;
        float gx0 = mid - cols * pitch * 0.5f;
        float gz0 = zn + (nz > 0 ? -2.5f : 2.5f);           /* set back from the door */
        glBegin(GL_QUADS);
        for (uint64_t k = 0; k < want; k++){
            int ci = (int)(k % (uint64_t)cols), ri = (int)(k / (uint64_t)cols);
            uint64_t off = avail > want ? (uint64_t)((double)k / want * (double)avail) : k;
            unsigned char v = (r->data && off < avail) ? r->data[off] : 0;
            float cr, cg, cb; byte_color(v, &cr, &cg, &cb);
            glColor3f(cr, cg, cb);
            float cxp = gx0 + (ci + 0.5f) * pitch;
            float z = gz0 + (nz > 0 ? -1.0f : 1.0f) * ri * pitch;
            float hh = 0.12f + (v / 255.0f) * 1.10f;
            float s2 = pitch * 0.36f;
            quad3(cxp-s2,base,z+s2, cxp+s2,base,z+s2, cxp+s2,base+hh,z+s2, cxp-s2,base+hh,z+s2, 0,0,1);
            quad3(cxp+s2,base,z-s2, cxp-s2,base,z-s2, cxp-s2,base+hh,z-s2, cxp+s2,base+hh,z-s2, 0,0,-1);
            quad3(cxp+s2,base,z+s2, cxp+s2,base,z-s2, cxp+s2,base+hh,z-s2, cxp+s2,base+hh,z+s2, 1,0,0);
            quad3(cxp-s2,base,z-s2, cxp-s2,base,z+s2, cxp-s2,base+hh,z+s2, cxp-s2,base+hh,z-s2, -1,0,0);
            quad3(cxp-s2,base+hh,z+s2, cxp+s2,base+hh,z+s2, cxp+s2,base+hh,z-s2, cxp-s2,base+hh,z-s2, 0,1,0);
        }
        glEnd();
        break; }
    case RT_OBJECT: {
        /* the object's bytes as crates on the floor */
        uint64_t n16 = (r->size + 15) / 16; if (!n16) n16 = 1;
        int cols = (int)(w / 0.75f); if (cols < 1) cols = 1;
        int rows = (int)(depth * 0.7f / 0.75f); if (rows < 1) rows = 1;
        if ((uint64_t)(cols * rows) < n16) n16 = (uint64_t)cols * rows;
        glBegin(GL_QUADS);
        for (uint64_t k = 0; k < n16; k++){
            int cxi = (int)(k % (uint64_t)cols), czi = (int)(k / (uint64_t)cols);
            if (czi >= rows) break;
            float bx0 = x0 + 0.15f + cxi * 0.75f, bz0 = zn + (nz > 0 ? -1.0f - czi*0.75f : 1.0f + czi*0.75f);
            unsigned nonzero = 0, sum = 0;
            for (int q = 0; q < 16; q++){
                uint64_t o = k * 16 + (uint64_t)q;
                unsigned char v = (r->data && o < r->datasz) ? r->data[o] : 0;
                if (v) nonzero++;
                sum += v;
            }
            float hh = 0.18f + (nonzero / 16.0f) * 0.55f;
            float cr, cg, cb; byte_color((unsigned char)(sum / 16), &cr, &cg, &cb);
            glColor3f(cr * 0.9f, cg * 0.9f, cb * 0.9f);
            float bx1 = bx0 + 0.62f, bz1 = bz0 + (nz > 0 ? -0.62f : 0.62f);
            float za = bz0 < bz1 ? bz0 : bz1, zb = bz0 < bz1 ? bz1 : bz0;
            quad3(bx0,base,zb, bx1,base,zb, bx1,base+hh,zb, bx0,base+hh,zb, 0,0,1);
            quad3(bx1,base,za, bx0,base,za, bx0,base+hh,za, bx1,base+hh,za, 0,0,-1);
            quad3(bx1,base,zb, bx1,base,za, bx1,base+hh,za, bx1,base+hh,zb, 1,0,0);
            quad3(bx0,base,za, bx0,base,zb, bx0,base+hh,zb, bx0,base+hh,za, -1,0,0);
            quad3(bx0,base+hh,zb, bx1,base+hh,zb, bx1,base+hh,za, bx0,base+hh,za, 0,1,0);
        }
        glEnd();
        break; }
    case RT_BYTES: {
        /* raw bytes as a mosaic on the far wall */
        float bw = clampf(w - 1.0f, 2.0f, 14.0f);
        int cols = (int)(bw / 0.13f); if (cols > 96) cols = 96; if (cols < 4) cols = 4;
        int rows = 20;
        float cw2 = bw / cols, ch = 2.3f / rows;
        float mx0 = mid - bw * 0.5f;
        float z = zf + nz * 0.03f;
        glDisable(GL_LIGHTING);
        glBegin(GL_QUADS);
        for (int ry = 0; ry < rows; ry++)
            for (int cxi = 0; cxi < cols; cxi++){
                uint64_t idx = (uint64_t)ry * cols + cxi;
                if (r->datasz && idx >= r->datasz) { ry = rows; break; }
                unsigned char v = r->data ? r->data[idx] : 0;
                float cr, cg, cb; byte_color(v, &cr, &cg, &cb);
                glColor3f(cr, cg, cb);
                float ax = mx0 + cxi * cw2, ay = base + 3.0f - ry * ch;
                quad3(ax, ay - ch*0.9f, z, ax + cw2*0.9f, ay - ch*0.9f, z,
                      ax + cw2*0.9f, ay, z, ax, ay, z, 0, 0, nz);
            }
        glEnd();
        glEnable(GL_LIGHTING);
        break; }
    case RT_LIST: {
        /* the table's entries, printed on a board on the far wall */
        if (!r->nlines) break;
        float widest = 1.0f;
        for (int i = 0; i < r->nlines; i++){
            float ah = text_aspect(FNT_MONO, r->lines[i]);
            if (ah > widest) widest = ah;
        }
        int shown = r->nlines;
        float budgetW = clampf(w - 1.2f, 2.2f, 6.4f);
        float lh = budgetW / widest;
        float byH = 2.55f / (1.28f * shown);
        if (byH < lh) lh = byH;
        if (lh > 0.14f) lh = 0.14f;
        if (lh < 0.028f) lh = 0.028f;
        float bw = lh * widest + 0.30f;
        float bh = lh * 1.28f * shown + 0.34f;
        float bx0 = mid - bw * 0.5f, bx1 = mid + bw * 0.5f;
        float ytop = base + clampf(0.9f + bh, 1.6f, 3.35f);
        float z = zf + nz * 0.02f;
        glDisable(GL_LIGHTING);
        glColor3f(0.11f, 0.12f, 0.14f);
        glBegin(GL_QUADS);
        quad3(bx0, ytop - bh, z, bx1, ytop - bh, z, bx1, ytop, z, bx0, ytop, z, 0,0,nz);
        glEnd();
        glColor3f(0.42f, 0.45f, 0.40f);
        draw_box(bx0 - 0.08f, ytop - bh - 0.08f, z - 0.05f * nz, bx1 + 0.08f, ytop - bh, z + 0.05f * nz);
        draw_box(bx0 - 0.08f, ytop, z - 0.05f * nz, bx1 + 0.08f, ytop + 0.08f, z + 0.05f * nz);
        glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
        float rv[3] = { nz > 0 ? 1.0f : -1.0f, 0, 0 };
        float uv[3] = { 0, 1, 0 };
        float y = ytop - 0.17f - lh;
        for (int i = 0; i < shown; i++){
            glColor3f(0.62f, 0.92f, 0.68f);
            float p[3] = { nz > 0 ? bx0 + 0.15f : bx1 - 0.15f, y, z + nz * 0.012f };
            text_3d(FNT_MONO, p, rv, uv, lh, 0, r->lines[i]);
            y -= lh * 1.28f;
        }
        glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
        glEnable(GL_LIGHTING);
        break; }
    case RT_EMPTY: {
        glDisable(GL_LIGHTING);
        glColor3f(0.45f, 0.52f, 0.62f);
        float s2 = clampf(w * 0.18f, 0.4f, 1.4f);
        float z = zmid;
        glBegin(GL_LINES);
        float c[3] = { mid, base + 0.9f, z };
        float o[8][3] = {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1},{-1,1,-1},{1,1,-1},{1,1,1},{-1,1,1}};
        int ed[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for (int i = 0; i < 12; i++)
            for (int k = 0; k < 2; k++)
                glVertex3f(c[0]+o[ed[i][k]][0]*s2, c[1]+o[ed[i][k]][1]*s2, c[2]+o[ed[i][k]][2]*s2);
        glEnd();
        glEnable(GL_LIGHTING);
        break; }
    }
}

/* ------------------------------------------------------------------ */
/* interior                                                            */
/* ------------------------------------------------------------------ */

static void draw_interior(const Building *b, int fl, App *ap, int active){
    const Player *p = &ap->p;
    DrawCtx d = { b, 1.0f, 1 };
    emit_floor(b, fl, sink_draw, &d);
    emit_ceiling(b, fl, sink_draw, &d);
    if (fl == 0) draw_stair(b, 0, b->nfloors > 3 ? 2 : b->nfloors - 1);
    else draw_stair(b, fl - 1, fl + 1);

    int a = b->floorStart[fl], e = b->floorStart[fl + 1];
    for (int i = a; i < e; i++)
        draw_room(b, &b->rooms[i], active && p->room == i, ap);

    /* floor number on the core wall */
    char buf[64];
    snprintf(buf, sizeof buf, "FLOOR %d / %d", fl + 1, b->nfloors);
    glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
    glColor3f(0.95f, 0.85f, 0.40f);
    float uv[3] = { 0, 1, 0 };
    float ppE[3] = { b->bx + 0.02f, fl * FLOOR_H + 3.10f, b->bz };   /* seen from the corridor */
    float rvE[3] = { 0, 0, -1 };
    text_3d(FNT_MONO, ppE, rvE, uv, 0.34f, 1, buf);
    float ppW[3] = { b->bx - WALL_T - 0.02f, fl * FLOOR_H + 3.10f, b->bz };  /* from the stair */
    float rvW[3] = { 0, 0, 1 };
    text_3d(FNT_MONO, ppW, rvW, uv, 0.34f, 1, buf);
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND); glEnable(GL_LIGHTING);
}

/* ------------------------------------------------------------------ */
/* plaza furniture                                                     */
/* ------------------------------------------------------------------ */

static void draw_monument(const App *a){
    const Elf *e = a->elf;
    glColor3f(0.72f, 0.70f, 0.66f);
    glPushMatrix();
    glTranslatef(0, -0.45f, 0); glRotatef(-90, 1, 0, 0);
    gluCylinder(g_q, 3.2f, 2.6f, 1.2f, 4, 1);
    glPopMatrix();
    glColor3f(0.55f, 0.58f, 0.62f);
    glPushMatrix();
    glTranslatef(0, 0.75f, 0); glRotatef(-90, 1, 0, 0); glRotatef(45, 0, 0, 1);
    gluCylinder(g_q, 1.9f, 0.35f, 13.0f, 4, 1);
    glPopMatrix();

    char l[6][280];
    snprintf(l[0], 280, "%s", e->base);
    snprintf(l[1], 280, "ELF%d %s  %s", e->is64 ? 64 : 32, e->be ? "MSB" : "LSB", elf_machine_name(e->machine));
    snprintf(l[2], 280, "%s   entry 0x%llx", elf_type_name(e->etype), (unsigned long long)e->entry);
    snprintf(l[3], 280, "%d sections  %d symbols%s", e->nsec, e->nsym, e->stripped ? "  (stripped)" : "");
    snprintf(l[4], 280, "%.1f KiB on disk", e->maplen / 1024.0);
    snprintf(l[5], 280, "%s", e->soname ? e->soname : (e->interp ? e->interp : ""));

    glDisable(GL_LIGHTING);
    for (int face = 0; face < 4; face++){
        glPushMatrix();
        glRotatef(face * 90.0f, 0, 1, 0);
        glColor3f(0.10f, 0.12f, 0.15f);
        glBegin(GL_QUADS);
        quad3(-4.6f, 1.30f, 2.30f, 4.6f, 1.30f, 2.30f,
               4.6f, 6.10f, 2.30f, -4.6f, 6.10f, 2.30f, 0, 0, 1);
        glEnd();
        glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
        glColor3f(0.98f, 0.94f, 0.72f);
        float rv[3] = { 1, 0, 0 }, uv[3] = { 0, 1, 0 };
        float p0[3] = { 0, 5.20f, 2.33f };
        text_3d(FNT_SIGN, p0, rv, uv, 0.62f, 1, l[0]);
        glColor3f(0.85f, 0.90f, 0.95f);
        for (int i = 1; i < 6; i++){
            if (!l[i][0]) continue;
            float pq[3] = { 0, 4.62f - (i - 1) * 0.66f, 2.33f };
            float ah = text_aspect(FNT_MONO, l[i]);
            float th = clampf(8.8f / (ah > 0.01f ? ah : 1.0f), 0.16f, 0.46f);
            text_3d(FNT_MONO, pq, rv, uv, th, 1, l[i]);
        }
        glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
        glPopMatrix();
    }
    glEnable(GL_LIGHTING);
}

static void draw_portals(const App *a){
    City *c = a->city;
    for (int i = 0; i < c->nportal; i++){
        Portal *p = &c->portal[i];
        int ok = p->path[0] != 0;
        glColor3f(ok ? 0.55f : 0.45f, ok ? 0.52f : 0.42f, ok ? 0.58f : 0.42f);
        draw_box(p->x - 2.6f, 0, p->z - 0.35f, p->x - 2.0f, 5.0f, p->z + 0.35f);
        draw_box(p->x + 2.0f, 0, p->z - 0.35f, p->x + 2.6f, 5.0f, p->z + 0.35f);
        draw_box(p->x - 2.6f, 5.0f, p->z - 0.35f, p->x + 2.6f, 5.8f, p->z + 0.35f);
        glDisable(GL_LIGHTING);
        if (ok) glColor4f(0.35f, 0.75f, 0.95f, 0.35f); else glColor4f(0.5f, 0.2f, 0.2f, 0.25f);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        quad3(p->x - 2.0f, 0.1f, p->z, p->x + 2.0f, 0.1f, p->z,
              p->x + 2.0f, 5.0f, p->z, p->x - 2.0f, 5.0f, p->z, 0, 0, 1);
        glEnd();
        glEnable(GL_TEXTURE_2D);
        glColor3f(ok ? 0.95f : 0.75f, ok ? 0.95f : 0.55f, ok ? 0.80f : 0.55f);
        text_billboard(FNT_MONO, p->x, 6.6f, p->z, 0.62f, p->name);
        if (!ok) text_billboard(FNT_MONO, p->x, 6.0f, p->z, 0.36f, "(not found on this system)");
        glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND); glEnable(GL_LIGHTING);
    }
}

/* ------------------------------------------------------------------ */
/* sky                                                                 */
/* ------------------------------------------------------------------ */

static void draw_sky(void){
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0,1,0,1,-1,1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_TEXTURE_2D); glDisable(GL_FOG);
    glBegin(GL_QUADS);
    glColor3fv(SKY_BOT); glVertex2f(0,0); glVertex2f(1,0);
    glColor3fv(SKY_TOP); glVertex2f(1,1); glVertex2f(0,1);
    glEnd();
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

/* ------------------------------------------------------------------ */

void render_init(void){
    g_q = gluNewQuadric();
    gluQuadricNormals(g_q, GLU_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_NORMALIZE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
    float amb[4] = { 0.42f, 0.43f, 0.48f, 1 };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
}

void render_scene(App *a){
    City *c = a->city;
    Player *p = &a->p;
    int w = a->winw, h = a->winh;

    glViewport(0, 0, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    draw_sky();

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluPerspective(70.0, h ? (double)w / h : 1.3, 0.14, 1600.0);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    float ex = p->x, ey = p->y + EYE_H, ez = p->z;
    float fx = cosf(p->yaw) * cosf(p->pitch);
    float fy = sinf(p->pitch);
    float fz = sinf(p->yaw) * cosf(p->pitch);
    gluLookAt(ex, ey, ez, ex + fx, ey + fy, ez + fz, 0, 1, 0);

    /* camera basis for billboards */
    float rgt[3] = { -sinf(p->yaw), 0, cosf(p->yaw) };
    float upv[3] = { -cosf(p->yaw) * sinf(p->pitch), cosf(p->pitch), -sinf(p->yaw) * sinf(p->pitch) };
    text_set_cam(rgt, upv);

    float sun[4] = { 0.45f, 0.82f, 0.35f, 0 };
    float dif[4] = { 0.85f, 0.83f, 0.78f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, sun);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);

    if (p->inside >= 0){
        glDisable(GL_FOG);
        glEnable(GL_LIGHT1);
        float lp[4] = { ex, ey + 0.4f, ez, 1 };
        float ld[4] = { 0.58f, 0.57f, 0.55f, 1 };
        glLightfv(GL_LIGHT1, GL_POSITION, lp);
        glLightfv(GL_LIGHT1, GL_DIFFUSE, ld);
        glLightf(GL_LIGHT1, GL_CONSTANT_ATTENUATION, 1.0f);
        glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.02f);
        glLightf(GL_LIGHT1, GL_QUADRATIC_ATTENUATION, 0.0040f);
    } else {
        glDisable(GL_LIGHT1);
        glEnable(GL_FOG);
        float fc[4] = { SKY_BOT[0], SKY_BOT[1], SKY_BOT[2], 1 };
        glFogfv(GL_FOG_COLOR, fc);
        glFogf(GL_FOG_START, 160.0f);
        glFogf(GL_FOG_END, 900.0f);
    }

    glPolygonMode(GL_FRONT_AND_BACK, a->wire ? GL_LINE : GL_FILL);

    if (g_dlground) glCallList(g_dlground);
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        float dx = p->x - (bld_x0(b) + bld_x1(b)) * 0.5f;
        float dz = p->z - (bld_z0(b) + bld_z1(b)) * 0.5f;
        if (dx*dx + dz*dz > 1200.0f * 1200.0f) continue;
        if (i != p->inside) glCallList(g_dl[i]);
    }

    /* signs above each building */
    glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    {
        typedef struct { float d2; int i; } Near;
        Near near[512]; int nn = 0;
        for (int i = 0; i < c->nbld && nn < 512; i++){
            Building *b = &c->bld[i];
            float mx = (bld_x0(b) + bld_x1(b)) * 0.5f, mz = (bld_z0(b) + bld_z1(b)) * 0.5f;
            float dx = p->x - mx, dz = p->z - mz;
            float d2 = dx*dx + dz*dz;
            if (d2 > 500.0f * 500.0f) continue;
            near[nn].d2 = d2; near[nn].i = i; nn++;
        }
        for (int i = 1; i < nn; i++){          /* insertion sort, nearest first */
            Near t = near[i]; int j = i - 1;
            while (j >= 0 && near[j].d2 > t.d2){ near[j+1] = near[j]; j--; }
            near[j+1] = t;
        }
        int lim = nn < 18 ? nn : 18;
        for (int k = 0; k < lim; k++){
            Building *b = &c->bld[near[k].i];
            float d2 = near[k].d2, dist = sqrtf(d2);
            float mx = (bld_x0(b) + bld_x1(b)) * 0.5f, mz = (bld_z0(b) + bld_z1(b)) * 0.5f;
            float sz = clampf(dist * 0.028f, 1.0f, 7.0f);
            float lift = 3.2f + sz + (float)(near[k].i % 3) * sz * 0.8f;
            glColor3f(1.0f, 0.97f, 0.85f);
            text_billboard(FNT_SIGN, mx, b->nfloors * FLOOR_H + lift, mz, sz, b->label);
            if (d2 < 130.0f * 130.0f && k < 8){
                char sub[200];
                snprintf(sub, sizeof sub, "%s | %s | %d floors | %d rooms",
                         district_name(b->district), elf_sectype_name(b->sec->type),
                         b->nfloors, b->nrooms);
                glColor3f(0.86f, 0.90f, 0.95f);
                text_billboard(FNT_MONO, mx, b->nfloors * FLOOR_H + lift - sz * 0.75f, mz,
                               sz * 0.32f, sub);
            }
        }
    }
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND); glEnable(GL_LIGHTING);

    /* interiors: the one we are in, plus any we can see into from outside */
    if (p->inside >= 0){
        Building *b = &c->bld[p->inside];
        DrawCtx d = { b, 1.0f, 1 };
        emit_shell(b, sink_draw, &d);
        for (int f = p->floor - 1; f <= p->floor + 1; f++)
            if (f >= 0 && f < b->nfloors) draw_interior(b, f, a, f == p->floor);
    } else {
        for (int i = 0; i < c->nbld; i++){
            Building *b = &c->bld[i];
            float dx = p->x - bld_x0(b), dz = p->z - b->bz;
            if (dx*dx + dz*dz < 34.0f * 34.0f) draw_interior(b, 0, a, 0);
        }
    }

    draw_monument(a);
    draw_portals(a);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}
