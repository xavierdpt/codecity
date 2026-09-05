/* text.c -- SDL_ttf glyph strings baked into cached GL textures */
#define _GNU_SOURCE
#include "text.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_BITS 12
#define CACHE_SZ   (1 << CACHE_BITS)
#define MAX_LIVE   2000
/* The count is not the budget that matters: a 190-character listing line at
   30 px is nearly half a megabyte of texture, so a cache bounded only by
   entries can hold a quarter of a gigabyte of them.  Bound the pixels too,
   and cap what one string may cost.                                      */
#define MAX_BYTES  (28u * 1024u * 1024u)
#define MAX_TEX_W  1280

typedef struct Entry {
    char   *key;
    int     font;
    GLuint  tex;
    int     w, h;
    unsigned frame;             /* the frame that last asked for it */
    struct Entry *next;         /* hash chain    */
    struct Entry *lru, *mru;    /* recency chain */
} Entry;

static TTF_Font *g_font[FNT_COUNT];
static TTF_Font *g_small[FNT_COUNT];    /* for strings too long to raster whole */
static Entry    *g_tab[CACHE_SZ];
static Entry    *g_new, *g_old;         /* most / least recently used */
static int       g_live;
static size_t    g_bytes;
static unsigned  g_frame = 1;
static float     g_right[3] = {1,0,0}, g_up[3] = {0,1,0};

static const struct { const char *path; int size; } FONTS[FNT_COUNT] = {
    { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",  30 },
    { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",  44 },
    { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 56 },
};
static const char *FALLBACK[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf", NULL
};

int text_init(void){
    if (TTF_Init() != 0){ fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 0; }
    for (int i = 0; i < FNT_COUNT; i++){
        g_font[i] = TTF_OpenFont(FONTS[i].path, FONTS[i].size);
        for (int k = 0; !g_font[i] && FALLBACK[k]; k++)
            g_font[i] = TTF_OpenFont(FALLBACK[k], FONTS[i].size);
        if (!g_font[i]){ fprintf(stderr, "no usable font for slot %d\n", i); return 0; }
        TTF_SetFontHinting(g_font[i], TTF_HINTING_LIGHT);
        /* the same face at half the point size: a long line rasterized here
           is a quarter of the texture, and it is only ever drawn small */
        g_small[i] = TTF_OpenFont(FONTS[i].path, FONTS[i].size / 2);
        for (int k = 0; !g_small[i] && FALLBACK[k]; k++)
            g_small[i] = TTF_OpenFont(FALLBACK[k], FONTS[i].size / 2);
        if (g_small[i]) TTF_SetFontHinting(g_small[i], TTF_HINTING_LIGHT);
    }
    return 1;
}

void text_frame(void){ g_frame++; }

static unsigned hashs(const char *s, int f){
    unsigned h = 2166136261u ^ (unsigned)f;
    while (*s){ h ^= (unsigned char)*s++; h *= 16777619u; }
    return h & (CACHE_SZ - 1);
}

/* recency list: g_new is the most recent, g_old the least */
static void lru_unlink(Entry *e){
    if (e->mru) e->mru->lru = e->lru; else g_new = e->lru;
    if (e->lru) e->lru->mru = e->mru; else g_old = e->mru;
    e->mru = e->lru = NULL;
}

static void lru_front(Entry *e){
    e->mru = NULL; e->lru = g_new;
    if (g_new) g_new->mru = e;
    g_new = e;
    if (!g_old) g_old = e;
}

static void touch(Entry *e){
    e->frame = g_frame;
    if (g_new != e){ lru_unlink(e); lru_front(e); }
}

static void drop(Entry *e){
    unsigned h = hashs(e->key, e->font);
    for (Entry **pp = &g_tab[h]; *pp; pp = &(*pp)->next)
        if (*pp == e){ *pp = e->next; break; }
    lru_unlink(e);
    glDeleteTextures(1, &e->tex);
    g_bytes -= (size_t)e->w * e->h * 4;
    g_live--;
    free(e->key); free(e);
}

/* Evict from the cold end until we are inside both budgets, but never touch
   anything this frame has already drawn -- that is the difference between a
   cache and a treadmill of rasterizing the same strings every frame.     */
static void cache_sweep(void){
    Entry *e = g_old;
    while (e && (g_live > MAX_LIVE || g_bytes > MAX_BYTES)){
        Entry *prev = e->mru;
        if (e->frame != g_frame) drop(e);
        e = prev;
    }
}

/* Rasterize, stepping down to the half-size face and then clipping the tail
   rather than ever producing a texture wider than MAX_TEX_W.            */
static SDL_Surface *raster(int font, const char *s){
    SDL_Color white = { 255, 255, 255, 255 };
    const char *txt = s[0] ? s : " ";
    SDL_Surface *sf = TTF_RenderUTF8_Blended(g_font[font], txt, white);
    if (sf && sf->w > MAX_TEX_W && g_small[font]){
        SDL_FreeSurface(sf);
        sf = TTF_RenderUTF8_Blended(g_small[font], txt, white);
    }
    /* still too wide -- a proportional face, so clip and check again */
    TTF_Font *f = g_small[font] ? g_small[font] : g_font[font];
    size_t keep = strlen(txt);
    for (int guard = 0; sf && sf->w > MAX_TEX_W && guard < 4; guard++){
        keep = (size_t)((double)keep * MAX_TEX_W / sf->w * 0.98);
        if (keep < 4) keep = 4;
        while (keep > 4 && ((unsigned char)txt[keep] & 0xc0) == 0x80) keep--;  /* whole UTF-8 */
        char *cut = malloc(keep + 1);
        memcpy(cut, txt, keep); cut[keep] = 0;
        SDL_FreeSurface(sf);
        sf = TTF_RenderUTF8_Blended(f, cut, white);
        free(cut);
    }
    return sf;
}

static Entry *get(int font, const char *s){
    if (font < 0 || font >= FNT_COUNT) font = 0;
    unsigned h = hashs(s, font);
    for (Entry *e = g_tab[h]; e; e = e->next)
        if (e->font == font && !strcmp(e->key, s)){ touch(e); return e; }

    SDL_Surface *sf = raster(font, s);
    if (!sf) return NULL;
    SDL_Surface *cv = SDL_ConvertSurfaceFormat(sf, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(sf);
    if (!cv) return NULL;

    Entry *e = calloc(1, sizeof(Entry));
    e->key = strdup(s); e->font = font; e->w = cv->w; e->h = cv->h;
    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, cv->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cv->w, cv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cv->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(cv);
    e->next = g_tab[h]; g_tab[h] = e;
    g_live++; g_bytes += (size_t)e->w * e->h * 4;
    lru_front(e); e->frame = g_frame;
    cache_sweep();
    return e;
}

float text_aspect(int font, const char *s){
    Entry *e = get(font, s);
    return e && e->h ? (float)e->w / (float)e->h : 1.0f;
}

void text_set_cam(const float right[3], const float up[3]){
    memcpy(g_right, right, sizeof g_right);
    memcpy(g_up, up, sizeof g_up);
}

static void quad(Entry *e, const float o[3], const float rx[3], const float uy[3], float w, float h){
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex3f(o[0],                     o[1],                     o[2]);
    glTexCoord2f(1, 1); glVertex3f(o[0]+rx[0]*w,             o[1]+rx[1]*w,             o[2]+rx[2]*w);
    glTexCoord2f(1, 0); glVertex3f(o[0]+rx[0]*w+uy[0]*h,     o[1]+rx[1]*w+uy[1]*h,     o[2]+rx[2]*w+uy[2]*h);
    glTexCoord2f(0, 0); glVertex3f(o[0]+uy[0]*h,             o[1]+uy[1]*h,             o[2]+uy[2]*h);
    glEnd();
    glDisable(GL_CULL_FACE);
}

void text_3d(int font, const float p[3], const float r[3], const float u[3],
             float h, int anchor, const char *s){
    if (!s || !*s) return;
    Entry *e = get(font, s);
    if (!e) return;
    float w = h * (float)e->w / (float)e->h;
    float o[3] = { p[0], p[1], p[2] };
    float shift = anchor == 1 ? -w * 0.5f : (anchor == 2 ? -w : 0.0f);
    for (int i = 0; i < 3; i++) o[i] += r[i] * shift;
    quad(e, o, r, u, w, h);
}

void text_billboard(int font, float x, float y, float z, float h, const char *s){
    float p[3] = { x, y, z };
    text_3d(font, p, g_right, g_up, h, 1, s);
}

void text_begin_2d(int w, int h){
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
void text_end_2d(void){
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glEnable(GL_DEPTH_TEST);
}

float text_2d(int font, float x, float y, float px, const char *s){
    if (!s || !*s) return 0;
    Entry *e = get(font, s);
    if (!e) return 0;
    float w = px * (float)e->w / (float)e->h;
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex2f(x,     y);
    glTexCoord2f(1,0); glVertex2f(x + w, y);
    glTexCoord2f(1,1); glVertex2f(x + w, y + px);
    glTexCoord2f(0,1); glVertex2f(x,     y + px);
    glEnd();
    return w;
}

void text_shutdown(void){
    for (int i = 0; i < CACHE_SZ; i++){
        Entry *e = g_tab[i];
        while (e){ Entry *n = e->next; glDeleteTextures(1, &e->tex); free(e->key); free(e); e = n; }
        g_tab[i] = NULL;
    }
    g_new = g_old = NULL; g_live = 0; g_bytes = 0;
    for (int i = 0; i < FNT_COUNT; i++){
        if (g_font[i]) TTF_CloseFont(g_font[i]);
        if (g_small[i]) TTF_CloseFont(g_small[i]);
        g_font[i] = g_small[i] = NULL;
    }
    TTF_Quit();
}
