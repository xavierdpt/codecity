#ifndef TEXT_H
#define TEXT_H
#include <stddef.h>
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
/* --- the monospace atlas: text that may change every frame -----------
   The calls above cache a whole rendered string by its content, which is
   right for a label and wrong for a readout: a new string is a TTF
   rasterization and a texture upload, and it evicts the labels.  These
   draw FNT_MONO out of one atlas texture -- one bind, one quad per
   glyph, no rasterization -- so the content may be new every frame.
   Printable ASCII only; anything else shows as '?'.                  */
void  text_mono_2d(float x, float y, float pxheight, const char *s);
void  text_mono_3d(const float p[3], const float r[3], const float u[3],
                   float h, int anchor, const char *s);
void  text_mono_billboard(float x, float y, float z, float h, const char *s);
float text_mono_width(float h, const char *s);   /* advance of the whole run */
float text_mono_advance(void);                   /* one glyph, / its height  */
int   text_mono_ready(void);
void  text_mono_atlas(int *w, int *h, size_t *bytes);
/* what the string cache is holding, and how many strings it has had to
   rasterize since start -- the number the §13 test turns on */
void  text_cache_stats(int *live, size_t *bytes, unsigned long *rasters);

void text_begin_2d(int w, int h);
void text_end_2d(void);
#endif
