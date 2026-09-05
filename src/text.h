#ifndef TEXT_H
#define TEXT_H
enum { FNT_MONO, FNT_MONO_BIG, FNT_SIGN, FNT_COUNT };
int  text_init(void);
/* start of a frame: nothing drawn since this call may be evicted from the
   glyph cache, so a heavy frame cannot make itself re-rasterize forever */
void text_frame(void);
void text_shutdown(void);
/* measure: returns aspect (w/h) of the rendered string */
float text_aspect(int font, const char *s);
/* HUD text in pixels; returns advance in px */
float text_2d(int font, float x, float y, float pxheight, const char *s);
/* a quad in world space: origin p, right vector r (unit), up vector u (unit),
   glyph height h; anchor 0=left 1=centre 2=right */
void text_3d(int font, const float p[3], const float r[3], const float u[3],
             float h, int anchor, const char *s);
/* billboard facing the camera (camera right/up supplied) */
void text_billboard(int font, float x, float y, float z, float h, const char *s);
void text_set_cam(const float right[3], const float up[3]);
void text_begin_2d(int w, int h);
void text_end_2d(void);
#endif
