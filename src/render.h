#ifndef RENDER_H
#define RENDER_H
#include "app.h"
void render_init(void);
void render_set_city(City *c);
void render_scene(App *a);
void hud_draw(App *a);
#endif
