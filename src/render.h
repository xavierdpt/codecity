#ifndef RENDER_H
#define RENDER_H
#include "app.h"
/* the quadratic arc a branch wire is drawn along, and the lift that wire
   uses -- shared so a wisp taking the branch rides the same curve      */
void  arc_point(const float a[3], const float b[3], float lift, float t, float out[3]);
float arc_lift(const float a[3], const float b[3]);

void render_init(void);
void render_set_city(City *c);
void render_scene(App *a);
/* The exit port the crosshair is pointing at, in the room the player is
   standing in: 1 when there is one, filling *addr with its destination and
   *label with its name.  Also lights that port up in the next frame.    */
int  render_pick_port(App *a, uint64_t *addr, const char **label);
void hud_draw(App *a);
/* the §13 test panel: 200 strings a frame, none of them repeated */
void hud_stress(App *a);
#endif
