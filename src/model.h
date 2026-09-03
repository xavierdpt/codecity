/* model.h -- shared data model for the ELF city explorer */
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
#define ROOM_D      6.5f    /* room depth, each side of the corridor  */
#define CORR_W      4.0f    /* corridor width                         */
#define WALL_T      0.30f   /* wall thickness                         */
#define CORE_W      8.0f    /* stair-core footprint (square)          */
#define DOOR_W      1.9f    /* doorway width                          */
#define DOOR_H      2.7f    /* doorway height                         */
#define STAIR_RIN   0.70f   /* spiral stair inner radius              */
#define STAIR_ROUT  3.30f   /* spiral stair outer radius              */
#define STEPS_PER_FLOOR 16

typedef enum {
    RT_FUNC,      /* a function: byte columns + plaque      */
    RT_OBJECT,    /* a data object: crates                  */
    RT_LIST,      /* a table slice: text lines on shelves   */
    RT_BYTES,     /* raw bytes: voxel wall                  */
    RT_EMPTY      /* NOBITS / nothing here                  */
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

    int         floor, side;        /* side 0 = -Z, 1 = +Z */
    float       x0, x1;             /* extent along the corridor  */
    int         symidx;             /* -1 if none */

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
    float       len;                /* corridor length (local x 0..len)*/
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
    int        dcount[DK_COUNT];
    float      dmin[DK_COUNT][2], dmax[DK_COUNT][2];
} City;

City *city_build(Elf *e);
void  city_free(City *c);
void  floor_realize(Building *b, int f);
void  floor_release(Building *b);

/* code rooms: decode on entry, throw away on exit */
int   room_is_code(const Building *b, const Room *r);
void  city_enter_room(City *c, int bi, int ri);   /* frees whatever was open */
void  city_leave_room(City *c);
const char *elf_sym_at(const Elf *e, uint64_t addr, uint64_t *off);

/* geometry helpers shared by render + collision */
static inline float bld_x0(const Building *b) { return b->bx - CORE_W; }
static inline float bld_x1(const Building *b) { return b->bx + b->len; }
static inline float bld_z0(const Building *b) { return b->bz - (CORR_W*0.5f + ROOM_D); }
static inline float bld_z1(const Building *b) { return b->bz + (CORR_W*0.5f + ROOM_D); }

#endif
