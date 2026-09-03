#ifndef WORLD_H
#define WORLD_H
#include "model.h"

enum { M_SHELL, M_WALL, M_PART, M_SLAB, M_CORE, M_COLUMN, M_LINTEL, M_ROOF, M_PLINTH };

typedef struct { float x0,y0,z0,x1,y1,z1; int mat; } Box3;
typedef void (*BoxSink)(void *ud, const Box3 *b);

/* geometry emission, in world coordinates */
void emit_shell(const Building *b, BoxSink f, void *ud);
void emit_floor(const Building *b, int fl, BoxSink f, void *ud);
void emit_ceiling(const Building *b, int fl, BoxSink f, void *ud);

/* spiral stair: tread top height at a point, or -1e9 if not over a tread */
float stair_height(const Building *b, float wx, float wz, float curY);
void  stair_center(const Building *b, float *cx, float *cz);

typedef struct {
    float x, y, z;          /* feet */
    float yaw, pitch;
    float vy;
    int   grounded;
    int   inside;           /* building index, or -1 */
    int   floor;
    int   room;             /* room index within that building, or -1 */
    int   noclip;
} Player;

#define PLAYER_R    0.34f
#define PLAYER_H    1.78f
#define EYE_H       1.62f
#define STEP_UP     0.72f

int   world_locate(City *c, float wx, float wz, float wy);   /* building index or -1 */
float ground_at(City *c, int bi, float wx, float wz, float curY);
void  player_update(City *c, Player *p, float fwd, float strafe, int jump, float dt);
int   room_at(City *c, const Player *p);

#endif
