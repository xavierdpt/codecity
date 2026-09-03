/* world.c -- building geometry, collision and player movement */
#define _GNU_SOURCE
#include "world.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define TAU 6.28318530718f

static float clampf(float v, float a, float b){ return v < a ? a : (v > b ? b : v); }

void stair_center(const Building *b, float *cx, float *cz){
    *cx = b->bx - CORE_W * 0.5f;
    *cz = b->bz;
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

static void box(BoxSink f, void *ud, float x0, float y0, float z0,
                float x1, float y1, float z1, int mat){
    Box3 b = { x0, y0, z0, x1, y1, z1, mat };
    if (b.x1 - b.x0 < 0.01f || b.z1 - b.z0 < 0.01f || b.y1 - b.y0 < 0.01f) return;
    f(ud, &b);
}

void emit_shell(const Building *b, BoxSink f, void *ud){
    float x0 = bld_x0(b), x1 = bld_x1(b), z0 = bld_z0(b), z1 = bld_z1(b);
    float h = b->nfloors * FLOOR_H;
    float T = WALL_T;
    /* west face carries the front door, at ground level */
    float dl = b->bz - DOOR_W * 0.5f, dr = b->bz + DOOR_W * 0.5f;
    box(f, ud, x0, 0, z0, x0 + T, h, dl, M_SHELL);
    box(f, ud, x0, 0, dr, x0 + T, h, z1, M_SHELL);
    box(f, ud, x0, DOOR_H, dl, x0 + T, h, dr, M_SHELL);
    box(f, ud, x1 - T, 0, z0, x1, h, z1, M_SHELL);
    box(f, ud, x0, 0, z0, x1, h, z0 + T, M_SHELL);
    box(f, ud, x0, 0, z1 - T, x1, h, z1, M_SHELL);
    /* roof + parapet */
    box(f, ud, x0, h, z0, x1, h + 0.25f, z1, M_ROOF);
    box(f, ud, x0, h + 0.25f, z0, x1, h + 1.3f, z0 + 0.35f, M_ROOF);
    box(f, ud, x0, h + 0.25f, z1 - 0.35f, x1, h + 1.3f, z1, M_ROOF);
    box(f, ud, x0, h + 0.25f, z0, x0 + 0.35f, h + 1.3f, z1, M_ROOF);
    box(f, ud, x1 - 0.35f, h + 0.25f, z0, x1, h + 1.3f, z1, M_ROOF);
}

/* walls, partitions and the slab of one floor */
void emit_floor(const Building *b, int fl, BoxSink f, void *ud){
    if (fl < 0 || fl >= b->nfloors) return;
    float base = fl * FLOOR_H, top = base + FLOOR_H;
    float x0 = bld_x0(b), x1 = bld_x1(b), z0 = bld_z0(b), z1 = bld_z1(b);
    float cw = CORR_W * 0.5f;
    float T = WALL_T;

    /* slab under the corridor + rooms */
    box(f, ud, b->bx, base - 0.18f, z0, x1, base, z1, M_SLAB);
    /* landing in front of the stair, on every floor */
    box(f, ud, b->bx - 2.4f, base - 0.18f, b->bz - cw, b->bx, base, b->bz + cw, M_SLAB);
    if (fl == 0) box(f, ud, x0, -0.18f, z0, b->bx, 0, z1, M_SLAB);   /* core ground */

    /* core / corridor divider */
    box(f, ud, b->bx - T, base, z0, b->bx, top, b->bz - cw, M_CORE);
    box(f, ud, b->bx - T, base, b->bz + cw, b->bx, top, z1, M_CORE);

    /* central column of the spiral stair */
    float sx, sz; stair_center(b, &sx, &sz);
    box(f, ud, sx - STAIR_RIN, base, sz - STAIR_RIN, sx + STAIR_RIN,
        b->nfloors * FLOOR_H, sz + STAIR_RIN, M_COLUMN);

    int a = b->floorStart[fl], e = b->floorStart[fl + 1];
    float endx[2] = { 0, 0 };
    for (int i = a; i < e; i++){
        const Room *r = &b->rooms[i];
        if (r->x1 > endx[r->side]) endx[r->side] = r->x1;
        float rx0 = b->bx + r->x0, rx1 = b->bx + r->x1;
        float wz0, wz1, bz0, bz1;
        if (r->side == 0){ wz0 = b->bz - cw - T; wz1 = b->bz - cw; bz0 = z0 + T; bz1 = wz0; }
        else             { wz0 = b->bz + cw;     wz1 = b->bz + cw + T; bz0 = wz1; bz1 = z1 - T; }

        /* corridor wall with a doorway */
        float dw = clampf(rx1 - rx0 - 1.0f, 0.8f, DOOR_W);
        float mid = (rx0 + rx1) * 0.5f, dl = mid - dw * 0.5f, dr = mid + dw * 0.5f;
        box(f, ud, rx0, base, wz0, dl, top, wz1, M_WALL);
        box(f, ud, dr, base, wz0, rx1, top, wz1, M_WALL);
        box(f, ud, dl, base + DOOR_H, wz0, dr, top, wz1, M_LINTEL);

        /* partition at the far end of the room (shared with the next room) */
        if (r->x1 < b->len - 0.05f){
            int door = (r->linkNext >= 0);
            float pz = (bz0 + bz1) * 0.5f;
            if (door){
                box(f, ud, rx1, base, bz0, rx1 + T, top, pz - DOOR_W * 0.5f, M_PART);
                box(f, ud, rx1, base, pz + DOOR_W * 0.5f, rx1 + T, top, bz1, M_PART);
                box(f, ud, rx1, base + DOOR_H, pz - DOOR_W * 0.5f, rx1 + T, top, pz + DOOR_W * 0.5f, M_LINTEL);
            } else {
                box(f, ud, rx1, base, bz0, rx1 + T, top, bz1, M_PART);
            }
        }
    }
    /* blank wall along the stretch of corridor with no rooms behind it */
    for (int side = 0; side < 2; side++){
        if (endx[side] >= b->len - 0.2f) continue;
        float wz0 = side ? b->bz + cw : b->bz - cw - T;
        float wz1 = side ? b->bz + cw + T : b->bz - cw;
        box(f, ud, b->bx + endx[side], base, wz0, x1, top, wz1, M_PART);
    }
}

void emit_ceiling(const Building *b, int fl, BoxSink f, void *ud){
    if (fl < 0 || fl >= b->nfloors) return;
    float y = (fl + 1) * FLOOR_H;
    float x1 = bld_x1(b), z0 = bld_z0(b), z1 = bld_z1(b);
    box(f, ud, b->bx, y - 0.18f, z0, x1, y, z1, M_SLAB);
}

/* ------------------------------------------------------------------ */
/* spiral stair                                                        */
/* ------------------------------------------------------------------ */

float stair_height(const Building *b, float wx, float wz, float curY){
    float cx, cz; stair_center(b, &cx, &cz);
    float dx = wx - cx, dz = wz - cz;
    float r = sqrtf(dx * dx + dz * dz);
    if (r < STAIR_RIN - 0.05f || r > STAIR_ROUT) return -1e9f;
    float a = atan2f(dz, dx);
    if (a < 0) a += TAU;
    int step = (int)(a / TAU * STEPS_PER_FLOOR);
    float frac = (float)step / (float)STEPS_PER_FLOOR;
    float best = -1e9f;
    for (int k = 0; k < b->nfloors; k++){
        float h = ((float)k + frac) * FLOOR_H;
        if (h <= curY + STEP_UP && h > best) best = h;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* collision                                                           */
/* ------------------------------------------------------------------ */

#define MAX_SOLIDS 4096
typedef struct { Box3 b[MAX_SOLIDS]; int n; } Solids;

static void sink_solid(void *ud, const Box3 *b){
    Solids *s = ud;
    if (b->mat == M_SLAB || b->mat == M_ROOF) return;
    if (s->n < MAX_SOLIDS) s->b[s->n++] = *b;
}

int world_locate(City *c, float wx, float wz, float wy){
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        if (wx >= bld_x0(b) && wx <= bld_x1(b) && wz >= bld_z0(b) && wz <= bld_z1(b)
            && wy < b->nfloors * FLOOR_H + 0.5f)
            return i;
    }
    return -1;
}

float ground_at(City *c, int bi, float wx, float wz, float curY){
    if (bi < 0) return 0.0f;
    Building *b = &c->bld[bi];
    if (wx < bld_x0(b) || wx > bld_x1(b) || wz < bld_z0(b) || wz > bld_z1(b)) return 0.0f;
    float cw = CORR_W * 0.5f;
    if (wx < b->bx - 2.4f){                       /* open stair shaft */
        float h = stair_height(b, wx, wz, curY);
        return h > -1e8f ? h : 0.0f;
    }
    if (wx < b->bx && (wz < b->bz - cw || wz > b->bz + cw)){
        float h = stair_height(b, wx, wz, curY);
        return h > -1e8f ? h : 0.0f;
    }
    float best = 0.0f;                             /* landings + floor slabs */
    for (int k = 0; k < b->nfloors; k++){
        float h = k * FLOOR_H;
        if (h <= curY + STEP_UP && h > best) best = h;
    }
    if (wx < b->bx){                               /* landing only exists at floors */
        float h = stair_height(b, wx, wz, curY);
        if (h > best) best = h;
    }
    return best;
}

static void gather(City *c, Player *p, Solids *s){
    s->n = 0;
    if (p->inside >= 0){
        Building *b = &c->bld[p->inside];
        emit_shell(b, sink_solid, s);
        for (int f = p->floor - 1; f <= p->floor + 1; f++) emit_floor(b, f, sink_solid, s);
    } else {
        for (int i = 0; i < c->nbld; i++){
            Building *b = &c->bld[i];
            if (p->x < bld_x0(b) - 40 || p->x > bld_x1(b) + 40 ||
                p->z < bld_z0(b) - 40 || p->z > bld_z1(b) + 40) continue;
            emit_shell(b, sink_solid, s);
            emit_floor(b, 0, sink_solid, s);
        }
    }
}

static int hits(const Solids *s, float x, float y, float z){
    float lo = y + 0.25f, hi = y + PLAYER_H;
    for (int i = 0; i < s->n; i++){
        const Box3 *b = &s->b[i];
        if (hi <= b->y0 || lo >= b->y1) continue;
        if (x + PLAYER_R <= b->x0 || x - PLAYER_R >= b->x1) continue;
        if (z + PLAYER_R <= b->z0 || z - PLAYER_R >= b->z1) continue;
        return 1;
    }
    return 0;
}

void player_update(City *c, Player *p, float fwd, float strafe, int jump, float dt){
    float s = sinf(p->yaw), co = cosf(p->yaw);
    float dx = co * fwd - s * strafe;
    float dz = s * fwd + co * strafe;

    if (p->noclip){
        float pitch = p->pitch;
        p->x += dx * cosf(pitch);
        p->z += dz * cosf(pitch);
        p->y += fwd * sinf(pitch) + (jump ? 0.6f : 0.0f);
        if (p->y < 0) p->y = 0;
        p->vy = 0;
        p->inside = world_locate(c, p->x, p->z, p->y);
        p->floor = p->inside >= 0 ? (int)(p->y / FLOOR_H) : 0;
        if (p->inside >= 0 && p->floor >= c->bld[p->inside].nfloors)
            p->floor = c->bld[p->inside].nfloors - 1;
        if (p->floor < 0) p->floor = 0;
        p->room = room_at(c, p);
        return;
    }

    static Solids sol;
    p->inside = world_locate(c, p->x, p->z, p->y);
    gather(c, p, &sol);

    if (!hits(&sol, p->x + dx, p->y, p->z)) p->x += dx;
    if (!hits(&sol, p->x, p->y, p->z + dz)) p->z += dz;

    int bi = world_locate(c, p->x, p->z, p->y);
    float g = ground_at(c, bi, p->x, p->z, p->y);

    if (p->grounded && jump){ p->vy = 5.2f; p->grounded = 0; }
    p->vy -= 17.0f * dt;
    if (p->vy < -40) p->vy = -40;
    p->y += p->vy * dt;

    if (p->y <= g + 0.001f){
        p->y = g; p->vy = 0; p->grounded = 1;
    } else if (p->y - g < STEP_UP && p->vy <= 0){
        p->y = g; p->vy = 0; p->grounded = 1;
    } else {
        p->grounded = 0;
    }

    p->inside = bi;
    p->floor = 0;
    if (bi >= 0){
        Building *b = &c->bld[bi];
        p->floor = (int)((p->y + 0.2f) / FLOOR_H);
        if (p->floor >= b->nfloors) p->floor = b->nfloors - 1;
        if (p->floor < 0) p->floor = 0;
    }
    p->room = room_at(c, p);
}

int room_at(City *c, const Player *p){
    if (p->inside < 0) return -1;
    Building *b = &c->bld[p->inside];
    if (p->floor < 0 || p->floor >= b->nfloors) return -1;
    float lx = p->x - b->bx, lz = p->z - b->bz;
    if (lx < 0) return -1;
    int side;
    if (lz < -CORR_W * 0.5f) side = 0;
    else if (lz > CORR_W * 0.5f) side = 1;
    else return -1;
    for (int i = b->floorStart[p->floor]; i < b->floorStart[p->floor + 1]; i++){
        Room *r = &b->rooms[i];
        if (r->side == side && lx >= r->x0 - 0.2f && lx <= r->x1 + 0.2f) return i;
    }
    return -1;
}
