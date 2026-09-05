#ifndef RENDER_H
#define RENDER_H
#include "app.h"
void render_init(void);
void render_set_city(City *c);
void render_scene(App *a);
/* The exit port the crosshair is pointing at, in the room the player is
   standing in: 1 when there is one, filling *addr with its destination and
   *label with its name.  Also lights that port up in the next frame.    */
int  render_pick_port(App *a, uint64_t *addr, const char **label);
void hud_draw(App *a);
#endif
