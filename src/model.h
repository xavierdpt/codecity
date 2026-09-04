/* model.h -- shared data model for the Code City explorer */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <stddef.h>
#include "disasm.h"

/* ------------------------------------------------------------------ */
/* ELF model                                                           */
/* ------------------------------------------------------------------ */

#define MAX_SEC   400
#define MAX_SEG   64
#define MAX_NEED  128

/* district / zoning of a section */
enum { DK_CODE, DK_RODATA, DK_DATA, DK_LINK, DK_DEBUG, DK_OTHER, DK_COUNT };

typedef struct Sec {
    char        name[96];
    uint32_t    type, link, info;
    uint64_t    flags, addr, offset, size, entsize, align;
    int         index;
    int         district;
    const uint8_t *data;        /* NULL for NOBITS / out-of-range */
    uint64_t    datasz;
    int         symFirst, symCount;   /* range into Elf.secsym[] */
    struct Building *bld;
} Sec;

typedef struct Sym {
    const char *name;
    uint64_t    value, size;
    int         shndx;
    uint8_t     bind, type, isdyn;
} Sym;

typedef struct Seg {
    uint32_t type, flags;
    uint64_t offset, vaddr, filesz, memsz, align;
} Seg;

typedef struct Elf {
    char        path[1024];
    char        base[256];
    const uint8_t *map;
    size_t      maplen;
    int         mapfd;

    int         is64, be;
    int         etype, machine;
    uint64_t    entry;
    int         stripped;

    Sec         sec[MAX_SEC];
    int         nsec;
    Seg         seg[MAX_SEG];
    int         nseg;

    Sym        *sym;
    int         nsym;
    int        *secsym;         /* symbol indices grouped by section, sorted by value */

    char       *needed[MAX_NEED];
    int         nneeded;
    char       *soname, *interp, *rpath;
    int        *addrsym;        /* symbol indices sorted by address, for lookups */
    int         naddrsym;
} Elf;

Elf  *elf_open(const char *path, char *err, size_t errlen);
void  elf_close(Elf *e);
const char *elf_machine_name(int m);
const char *elf_type_name(int t);
const char *elf_sectype_name(uint32_t t);
const char *elf_symtype_name(int t);
const char *elf_symbind_name(int b);
const char *district_name(int d);

/* raw readers + tables (elfload.c) */
uint16_t elf_rd16(const Elf *e, const uint8_t *p);
uint32_t elf_rd32(const Elf *e, const uint8_t *p);
uint64_t elf_rd64(const Elf *e, const uint8_t *p);
uint64_t elf_rdw (const Elf *e, const uint8_t *p);
const char *elf_str(const Elf *e, int strsec, uint64_t off);
const char *elf_dyntag_name(uint64_t t);
const char *elf_reltype_name(int machine, uint32_t t);


/* ------------------------------------------------------------------ */
/* City model                                                          */
/* ------------------------------------------------------------------ */

/* architectural constants (metres) */
#define FLOOR_H     4.0f    /* floor-to-floor                         */
#define ROOM_D      6.5f    /* legacy fixed room depth (plaza props)  */
#define CORR_W      4.0f    /* corridor width                         */
#define WALL_T      0.30f   /* wall thickness                         */
#define CORE_W      8.0f    /* stair-core footprint (square)          */
#define DOOR_W      1.9f    /* doorway width                          */
#define DOOR_H      2.7f    /* doorway height                         */
#define STAIR_RIN   0.70f   /* spiral stair inner radius              */
#define STAIR_ROUT  3.30f   /* spiral stair outer radius              */
#define STEPS_PER_FLOOR 16

/* content grid: one tile is one unit of content (an instruction, a 16-byte
   line, a printed row).  Rooms are sized from their tile count.          */
#define TILE_M      0.55f   /* metres per tile, before per-building scaling */
#define TILE_MIN    0.22f
#define TILE_MARGIN 0.70f   /* clear strip between the grid and the walls   */
#define TILE_SETBACK 1.20f  /* clear strip inside the door                  */
#define ROOM_W_MIN  2.60f   /* absolute floor: you must fit through it     */
#define ROOM_D_MIN  2.40f
#define ROOM_TYPICAL 7.0f   /* what a median room should measure across     */
#define ASPECT_STEP 1.12f   /* plate growth per repack attempt              */
#define MAXPF       10      /* rooms per floor, hard cap                    */
#define PLATE_FILL  0.62f   /* assumed packing efficiency when sizing a plate */
#define GROUP_TILES 64      /* units at or below this share a chamber       */
#define GROUND_MARGIN 400.0f/* how far the ground quad runs past the city   */

/* one piece of content: a function, an object, a slice of a table.  A plain
   room holds exactly one and mirrors it in its own fields; a chamber holds
   several, one per cell of a 3x3 grid with the middle left open.          */
typedef struct Unit {
    uint64_t    addr, size, fileoff;
    const uint8_t *data;
    uint64_t    datasz;
    int         symidx;
    int         cell;               /* 0..7 around the 3x3, -1 if whole room */
    int         ntiles, tw, th;
    char        title[96];
} Unit;

typedef enum {
    RT_FUNC,      /* a function: byte columns + plaque      */
    RT_OBJECT,    /* a data object: crates                  */
    RT_LIST,      /* a table slice: text lines on shelves   */
    RT_BYTES,     /* raw bytes: voxel wall                  */
    RT_EMPTY,     /* NOBITS / nothing here                  */
    RT_GROUP      /* a chamber of small units around an open middle */
} RoomKind;

/* how a RT_LIST room regenerates its text lines on demand */
enum { LS_NONE, LS_STRINGS, LS_SYMS, LS_DYN, LS_RELA, LS_REL, LS_NOTE, LS_HEX };

typedef struct Room {
    RoomKind    kind;
    char        title[192];
    char        sub[192];
    uint64_t    addr, size, fileoff;
    const uint8_t *data;            /* content bytes, may be NULL */
    uint64_t    datasz;

    int         floor, row;         /* which floor, which comb row */
    float       x0, x1;             /* plate-local rect, along the row   */
    float       z0, z1;             /* z0 is the wall the door is in     */
    int         symidx;             /* -1 if none */

    int         ntiles, tw, th;     /* content grid, in tiles            */
    int         cellw, cellh;       /* chamber cell size; 0 if not RT_GROUP */
    Unit       *units;              /* NULL when the room is its own unit */
    int         nunits;
    int         activeUnit;         /* alcove currently decoded, -1 if none */

    int         linkPrev, linkNext; /* interior doors to neighbours */

    /* lazily realized listing text */
    int         lsrc;               /* LS_* */
    uint64_t    lo, hi;             /* byte range or entry range   */
    char      **lines;
    int         nlines;

    /* decoded on entry, freed on the way out -- only ever one at a time */
    Disasm     *dis;
} Room;

typedef struct Building {
    Sec        *sec;
    Elf        *elf;
    char        label[96];
    int         district;

    Room       *rooms;
    int         nrooms;
    int        *floorStart;         /* nfloors+1 entries */
    int         nfloors;
    int         truncated;          /* rooms dropped for size */

    float       bx, bz;             /* world position of local origin  */
    float       plateW, plateD;     /* the floor plate, every floor alike */
    float       tile;               /* metres per content tile         */
    float       wmin, dmin;         /* this building's smallest room   */
    float       len;                /* == plateW, kept for the exterior */
    float       w, d, h;            /* overall footprint + height      */
    float       col[3];

    int         realized;           /* which floor has text realized   */
} Building;

typedef struct Portal {
    char  name[128];
    char  path[1024];               /* resolved, "" if not found */
    float x, z, ang;
} Portal;

typedef struct City {
    Elf       *elf;
    Building  *bld;
    int        nbld;
    Portal    *portal;
    int        nportal;
    float      minx, maxx, minz, maxz;
    float      plazaR;
    float      farPlane;            /* derived from the city's extent  */
    int        dcount[DK_COUNT];
    float      dmin[DK_COUNT][2], dmax[DK_COUNT][2];
} City;

City *city_build(Elf *e);
void  city_free(City *c);
void  floor_realize(Building *b, int f);
void  floor_release(Building *b);

/* code rooms: decode on entry, throw away on exit */
int   room_is_code(const Building *b, const Room *r);
int   unit_is_code(const Building *b, const Unit *u);
void  city_enter_room(City *c, int bi, int ri);   /* frees whatever was open */
void  city_enter_unit(City *c, int bi, int ri, int ui);  /* one chamber alcove */
void  city_leave_room(City *c);
const char *elf_sym_at(const Elf *e, uint64_t addr, uint64_t *off);

/* geometry helpers shared by render + collision */
static inline float bld_x0(const Building *b) { return b->bx - CORE_W; }
static inline float bld_x1(const Building *b) { return b->bx + b->plateW; }
static inline float bld_z0(const Building *b) { return b->bz - b->plateD * 0.5f; }
static inline float bld_z1(const Building *b) { return b->bz + b->plateD * 0.5f; }

/* a room's rect in world coordinates */
static inline float room_x0(const Building *b, const Room *r){ return b->bx + r->x0; }
static inline float room_x1(const Building *b, const Room *r){ return b->bx + r->x1; }
static inline float room_z0(const Building *b, const Room *r){ return bld_z0(b) + r->z0; }
static inline float room_z1(const Building *b, const Room *r){ return bld_z0(b) + r->z1; }

#endif
