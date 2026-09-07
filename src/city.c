/* city.c -- turn an ELF file into a walkable city
 *
 *   file      -> city
 *   section   -> building        (zoned into districts by flags/type)
 *   symbol    -> room            (or a slice of a table / of raw bytes)
 *   contiguous symbols -> a door in the wall between the two rooms
 *   rooms     -> stacked into floors around a central corridor
 *   stair core-> spiral staircase joining every floor
 */
#define _GNU_SOURCE
#include "model.h"
#include "ehframe.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11

/* No cap on rooms or floors: a floor costs almost nothing until you stand
   on it, so a 3 MB .text is allowed to be a 300-storey tower.  The bound
   below only stops a corrupt file from exhausting memory.               */
#define MAX_ROOMS_PER_BLD  200000
#define LINES_PER_ROOM     22
#define MAX_ROWS           24      /* comb rows per floor                */
/* The biggest room's grid may run this far before the tile is squeezed.
   It is a safety valve against one monster function, not a design driver:
   floors are cheap, so a big plate is preferable to a stack of closets.  */
#define PLATE_MAX_SIDE     40.0f

static float clampf(float v, float a, float b){ return v < a ? a : (v > b ? b : v); }

/* ------------------------------------------------------------------ */
/* room construction                                                   */
/* ------------------------------------------------------------------ */

typedef struct { Room *r; int n, cap; } RoomVec;

static Room *rv_add(RoomVec *v){
    if (v->n == v->cap){ v->cap = v->cap ? v->cap * 2 : 64; v->r = realloc(v->r, (size_t)v->cap * sizeof(Room)); }
    Room *r = &v->r[v->n++];
    memset(r, 0, sizeof *r);
    r->symidx = -1; r->linkPrev = -1; r->linkNext = -1; r->activeUnit = -1;
    return r;
}

static void human(char *out, size_t n, uint64_t v){
    if (v < 1024) snprintf(out, n, "%llu B", (unsigned long long)v);
    else if (v < 1024*1024) snprintf(out, n, "%.1f KiB", v / 1024.0);
    else snprintf(out, n, "%.2f MiB", v / (1024.0*1024.0));
}

/* split an unsymbolised span into a handful of rooms */
static void span_rooms(RoomVec *v, Sec *s, uint64_t addr, uint64_t len, int maxrooms){
    if (!len) return;
    uint64_t chunk = 1024;
    while (len / chunk > (uint64_t)maxrooms) chunk *= 2;
    char h[32];
    for (uint64_t o = 0; o < len && v->n < MAX_ROOMS_PER_BLD; o += chunk){
        uint64_t n = len - o < chunk ? len - o : chunk;
        uint64_t a = addr + o, rel = a - s->addr;
        Room *r = rv_add(v);
        r->kind = (s->type == SHT_NOBITS) ? RT_EMPTY : RT_BYTES;
        r->addr = a; r->size = n; r->fileoff = s->offset + rel;
        if (s->data && rel < s->datasz){
            r->data = s->data + rel;
            r->datasz = s->datasz - rel; if (r->datasz > n) r->datasz = n;
        }
        human(h, sizeof h, n);
        snprintf(r->title, sizeof r->title, "unnamed 0x%llx", (unsigned long long)a);
        snprintf(r->sub, sizeof r->sub, "%s  no symbol covers this", h);
    }
}

/* rooms from the symbols that live in this section */
static void rooms_from_syms(RoomVec *v, Elf *e, Sec *s){
    uint64_t cursor = s->addr;
    char sz[32];
    for (int k = 0; k < s->symCount && v->n < MAX_ROOMS_PER_BLD; k++){
        int si = e->secsym[s->symFirst + k];
        Sym *y = &e->sym[si];
        if (y->value < s->addr || y->value > s->addr + s->size) continue;
        /* a hole between the previous symbol and this one */
        if (y->value > cursor + 63 && v->n < MAX_ROOMS_PER_BLD - 1)
            span_rooms(v, s, cursor, y->value - cursor, 24);
        Room *r = rv_add(v);
        r->kind = (y->type == 2 || y->type == 10) ? RT_FUNC : RT_OBJECT;
        if (s->type == SHT_NOBITS) r->kind = RT_EMPTY;
        r->addr = y->value; r->size = y->size; r->symidx = si;
        r->fileoff = s->offset + (y->value - s->addr);
        if (s->data && y->value - s->addr < s->datasz){
            r->data = s->data + (y->value - s->addr);
            r->datasz = s->datasz - (y->value - s->addr);
            if (r->size && r->datasz > r->size) r->datasz = r->size;
        }
        human(sz, sizeof sz, y->size);
        snprintf(r->title, sizeof r->title, "%s", y->name);
        snprintf(r->sub, sizeof r->sub, "%s %s  0x%llx  %s",
                 elf_symbind_name(y->bind), elf_symtype_name(y->type),
                 (unsigned long long)y->value, sz);
        if (y->size) cursor = y->value + y->size;
        else if (y->value > cursor) cursor = y->value;
    }
    /* trailing hole */
    if (s->addr + s->size > cursor + 63 && v->n < MAX_ROOMS_PER_BLD)
        span_rooms(v, s, cursor, s->addr + s->size - cursor, 40);
    (void)sz;
}

/* rooms that hold a slice of a fixed-size entry table */
static void rooms_from_entries(RoomVec *v, Sec *s, int lsrc, uint64_t esz, const char *unit){
    if (!esz) esz = 1;
    uint64_t total = s->datasz / esz;
    if (!total){ return; }
    uint64_t per = LINES_PER_ROOM;
    uint64_t want = (total + per - 1) / per;
    if (want > MAX_ROOMS_PER_BLD){ per = (total + MAX_ROOMS_PER_BLD - 1) / MAX_ROOMS_PER_BLD; want = (total + per - 1) / per; }
    for (uint64_t i = 0; i < want; i++){
        Room *r = rv_add(v);
        r->kind = RT_LIST; r->lsrc = lsrc;
        r->lo = i * per; r->hi = r->lo + per; if (r->hi > total) r->hi = total;
        r->addr = s->addr ? s->addr + r->lo * esz : 0;
        r->fileoff = s->offset + r->lo * esz;
        r->size = (r->hi - r->lo) * esz;
        r->data = s->data; r->datasz = s->datasz;
        snprintf(r->title, sizeof r->title, "%s %llu-%llu", unit,
                 (unsigned long long)r->lo, (unsigned long long)(r->hi - 1));
        snprintf(r->sub, sizeof r->sub, "%llu entries  @0x%llx",
                 (unsigned long long)(r->hi - r->lo), (unsigned long long)r->fileoff);
    }
}

/* rooms full of NUL-terminated strings */
static void rooms_from_strings(RoomVec *v, Sec *s){
    if (!s->data) return;
    uint64_t i = 0, start = 0; int count = 0;
    while (i < s->datasz && v->n < MAX_ROOMS_PER_BLD){
        while (i < s->datasz && s->data[i]) i++;
        i++;                                  /* past the NUL */
        count++;
        if (count >= LINES_PER_ROOM || i >= s->datasz){
            Room *r = rv_add(v);
            r->kind = RT_LIST; r->lsrc = LS_STRINGS;
            r->lo = start; r->hi = i > s->datasz ? s->datasz : i;
            r->data = s->data; r->datasz = s->datasz;
            r->addr = s->addr ? s->addr + start : 0;
            r->fileoff = s->offset + start;
            r->size = r->hi - r->lo;
            snprintf(r->title, sizeof r->title, "strings @0x%llx", (unsigned long long)r->fileoff);
            snprintf(r->sub, sizeof r->sub, "%d strings  %llu bytes", count, (unsigned long long)r->size);
            start = i; count = 0;
        }
    }
}

/* rooms from recovered function boundaries: .eh_frame first, then a byte
   scan for whatever it does not reach.  This is what makes a stripped
   binary legible -- without it the whole of .text is arbitrary slabs.  */
static int cmp_u(const void *a, const void *b){
    const Unitrange *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return 0;
}

static int rooms_from_units(RoomVec *v, Elf *e, Sec *s,
                            const Unitrange *all, int nall){
    uint64_t lo = s->addr, hi = s->addr + s->size;
    if (!s->size) return 0;

    Unitrange *u = NULL; int n = 0, cap = 0;
    for (int i = 0; i < nall; i++){
        if (all[i].addr < lo || all[i].addr >= hi) continue;
        if (n == cap){ cap = cap ? cap * 2 : 256; u = realloc(u, (size_t)cap * sizeof *u); }
        u[n++] = all[i];
    }
    /* spans the unwind tables never described: scan them for entries */
    uint64_t cur = lo;
    int nfde = n;
    for (int i = 0; i < nfde; i++){
        if (u[i].addr > cur + 256) n = seed_units(e, s, cur, u[i].addr, &u, n);
        uint64_t end = u[i].addr + u[i].size;
        if (end > cur) cur = end;
    }
    if (hi > cur + 256) n = seed_units(e, s, cur, hi, &u, n);
    if (n < 1){ free(u); return 0; }

    qsort(u, (size_t)n, sizeof *u, cmp_u);
    int m = 0;                                     /* unique, ascending */
    for (int i = 0; i < n; i++)
        if (!m || u[i].addr > u[m-1].addr) u[m++] = u[i];
    n = m;

    char sz[32];
    for (int i = 0; i < n && v->n < MAX_ROOMS_PER_BLD; i++){
        uint64_t a = u[i].addr;
        uint64_t end = (i + 1 < n) ? u[i+1].addr : hi;
        if (u[i].size && a + u[i].size < end) end = a + u[i].size;
        if (end <= a) continue;
        uint64_t len = end - a, rel = a - s->addr;
        Room *r = rv_add(v);
        r->kind = (s->type == SHT_NOBITS) ? RT_EMPTY : RT_FUNC;
        r->addr = a; r->size = len; r->fileoff = s->offset + rel;
        if (s->data && rel < s->datasz){
            r->data = s->data + rel;
            r->datasz = s->datasz - rel; if (r->datasz > len) r->datasz = len;
        }
        human(sz, sizeof sz, len);
        uint64_t soff = 0;
        const char *nm = elf_sym_at(e, a, &soff);
        if (nm && nm[0] && soff == 0){
            snprintf(r->title, sizeof r->title, "%s", nm);
            snprintf(r->sub, sizeof r->sub, "%s  0x%llx", sz, (unsigned long long)a);
        } else {
            snprintf(r->title, sizeof r->title, "sub_%llx", (unsigned long long)a);
            snprintf(r->sub, sizeof r->sub, "%s  recovered %s", sz,
                     i < nfde ? "from .eh_frame" : "by call scan");
        }
    }
    free(u);
    return 1;
}

/* rooms holding a slab of raw bytes */
static void rooms_from_bytes(RoomVec *v, Sec *s){
    uint64_t sz = s->size;
    if (!sz){ return; }
    uint64_t chunk = 512;
    while (sz / chunk > MAX_ROOMS_PER_BLD) chunk *= 2;
    char h[32];
    for (uint64_t o = 0; o < sz && v->n < MAX_ROOMS_PER_BLD; o += chunk){
        uint64_t n = sz - o < chunk ? sz - o : chunk;
        Room *r = rv_add(v);
        r->kind = (s->type == SHT_NOBITS) ? RT_EMPTY : RT_BYTES;
        r->addr = s->addr ? s->addr + o : 0;
        r->fileoff = s->offset + o; r->size = n;
        if (s->data && o < s->datasz){
            r->data = s->data + o;
            r->datasz = s->datasz - o; if (r->datasz > n) r->datasz = n;
        }
        human(h, sizeof h, n);
        snprintf(r->title, sizeof r->title, "+0x%llx", (unsigned long long)o);
        snprintf(r->sub, sizeof r->sub, "%s at 0x%llx", h,
                 (unsigned long long)(s->addr ? s->addr + o : s->offset + o));
    }
}

/* ------------------------------------------------------------------ */
/* lazy text for listing rooms                                         */
/* ------------------------------------------------------------------ */

static void addline(Room *r, const char *fmt, ...){
    if (r->nlines >= 256) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r->lines = realloc(r->lines, (size_t)(r->nlines + 1) * sizeof(char *));
    r->lines[r->nlines++] = strdup(buf);
}

static void sanitize(char *s){
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) *s = '.';
}

static void realize_room(Building *b, Room *r){
    if (r->kind != RT_LIST || r->lines) return;
    Elf *e = b->elf; Sec *s = b->sec;
    char tmp[256];
    switch (r->lsrc){
    case LS_STRINGS: {
        uint64_t i = r->lo;
        while (i < r->hi && r->nlines < LINES_PER_ROOM + 4){
            const char *p = (const char *)r->data + i;
            uint64_t n = 0;
            while (i + n < r->datasz && p[n]) n++;
            if (n > 190) n = 190;
            memcpy(tmp, p, (size_t)n); tmp[n] = 0; sanitize(tmp);
            addline(r, "%s", n ? tmp : "\"\"");
            while (i < r->datasz && r->data[i]) i++;
            i++;
        }
        break; }
    case LS_SYMS: {
        uint64_t esz = s->entsize ? s->entsize : (e->is64 ? 24u : 16u);
        for (uint64_t i = r->lo; i < r->hi; i++){
            const uint8_t *p = r->data + i * esz;
            if ((i + 1) * esz > r->datasz) break;
            uint32_t no; uint64_t val, sz; uint8_t info; uint16_t sh;
            if (e->is64){ no = elf_rd32(e, p); info = p[4]; sh = elf_rd16(e, p+6); val = elf_rd64(e, p+8); sz = elf_rd64(e, p+16); }
            else        { no = elf_rd32(e, p); val = elf_rd32(e, p+4); sz = elf_rd32(e, p+8); info = p[12]; sh = elf_rd16(e, p+14); }
            const char *nm = elf_str(e, (int)s->link, no);
            snprintf(tmp, sizeof tmp, "%s", nm[0] ? nm : "(unnamed)");
            if (strlen(tmp) > 44) strcpy(tmp + 41, "...");
            sanitize(tmp);
            addline(r, "%-44s %-6s %-6s sec:%-3u 0x%llx (%llu)", tmp,
                    elf_symbind_name(info >> 4), elf_symtype_name(info & 0xf),
                    sh, (unsigned long long)val, (unsigned long long)sz);
        }
        break; }
    case LS_DYN: {
        uint64_t esz = e->is64 ? 16 : 8;
        if (s->entsize) esz = s->entsize;
        for (uint64_t i = r->lo; i < r->hi; i++){
            const uint8_t *p = r->data + i * esz;
            if ((i + 1) * esz > r->datasz) break;
            uint64_t tag = elf_rdw(e, p), val = elf_rdw(e, p + esz / 2);
            const char *tn = elf_dyntag_name(tag);
            int isstr = (tag == 1 || tag == 14 || tag == 15 || tag == 29);
            if (isstr){
                snprintf(tmp, sizeof tmp, "%s", elf_str(e, (int)s->link, val)); sanitize(tmp);
                if (tn) addline(r, "%-16s %s", tn, tmp);
                else    addline(r, "0x%-14llx %s", (unsigned long long)tag, tmp);
            } else if (tn) addline(r, "%-16s 0x%llx", tn, (unsigned long long)val);
            else addline(r, "0x%-14llx 0x%llx", (unsigned long long)tag, (unsigned long long)val);
        }
        break; }
    case LS_RELA: case LS_REL: {
        int rela = (r->lsrc == LS_RELA);
        uint64_t esz = s->entsize ? s->entsize : (e->is64 ? (rela ? 24u : 16u) : (rela ? 12u : 8u));
        int symsec = (int)s->link;
        uint64_t symesz = (symsec > 0 && symsec < e->nsec) ?
            (e->sec[symsec].entsize ? e->sec[symsec].entsize : (e->is64 ? 24u : 16u)) : 0;
        int strsec = (symsec > 0 && symsec < e->nsec) ? (int)e->sec[symsec].link : -1;
        for (uint64_t i = r->lo; i < r->hi; i++){
            const uint8_t *p = r->data + i * esz;
            if ((i + 1) * esz > r->datasz) break;
            uint64_t off = elf_rdw(e, p), info = elf_rdw(e, p + (e->is64 ? 8 : 4));
            int64_t add = 0;
            if (rela) add = (int64_t)elf_rdw(e, p + (e->is64 ? 16 : 8));
            uint32_t rsym = e->is64 ? (uint32_t)(info >> 32) : (uint32_t)(info >> 8);
            uint32_t rtyp = e->is64 ? (uint32_t)(info & 0xffffffffu) : (uint32_t)(info & 0xff);
            const char *tn = elf_reltype_name(e->machine, rtyp);
            char tbuf[24]; if (!tn){ snprintf(tbuf, sizeof tbuf, "type %u", rtyp); tn = tbuf; }
            const char *sn = "";
            if (rsym && symsec > 0 && symsec < e->nsec && e->sec[symsec].data && symesz){
                const uint8_t *sp = e->sec[symsec].data + (uint64_t)rsym * symesz;
                if (((uint64_t)rsym + 1) * symesz <= e->sec[symsec].datasz)
                    sn = elf_str(e, strsec, elf_rd32(e, sp));
            }
            snprintf(tmp, sizeof tmp, "%s", sn); sanitize(tmp);
            if (strlen(tmp) > 36) strcpy(tmp + 33, "...");
            if (rela) addline(r, "0x%-12llx %-14s %-36s %+lld", (unsigned long long)off, tn, tmp, (long long)add);
            else      addline(r, "0x%-12llx %-14s %s", (unsigned long long)off, tn, tmp);
        }
        break; }
    case LS_NOTE: {
        uint64_t o = r->lo;
        while (o + 12 <= r->hi && o + 12 <= r->datasz && r->nlines < LINES_PER_ROOM){
            uint32_t nsz = elf_rd32(e, r->data + o);
            uint32_t dsz = elf_rd32(e, r->data + o + 4);
            uint32_t typ = elf_rd32(e, r->data + o + 8);
            uint64_t na = (nsz + 3) & ~3ull, da = (dsz + 3) & ~3ull;
            if (o + 12 + na + da > r->datasz || (!nsz && !dsz)) break;
            char nm[64]; uint32_t c = nsz > 60 ? 60 : nsz;
            memcpy(nm, r->data + o + 12, c); nm[c ? c - 1 : 0] = 0; sanitize(nm);
            addline(r, "%-20s type 0x%-8x %u bytes", nm, typ, dsz);
            if (dsz && dsz <= 32){
                char hx[128]; int w = 0;
                for (uint32_t k = 0; k < dsz && w < 110; k++)
                    w += snprintf(hx + w, sizeof hx - (size_t)w, "%02x ", r->data[o + 12 + na + k]);
                addline(r, "    %s", hx);
            }
            o += 12 + na + da;
        }
        break; }
    case LS_HEX: default: {
        for (uint64_t o = r->lo; o < r->hi && r->nlines < LINES_PER_ROOM; o += 16){
            char hx[64] = {0}, as[24] = {0}; int w = 0, a = 0;
            for (int k = 0; k < 16; k++){
                if (o + (uint64_t)k >= r->datasz){ w += snprintf(hx + w, sizeof hx - (size_t)w, "   "); continue; }
                uint8_t byte = r->data[o + k];
                w += snprintf(hx + w, sizeof hx - (size_t)w, "%02x ", byte);
                as[a++] = (byte >= 32 && byte < 127) ? (char)byte : '.';
            }
            as[a] = 0;
            addline(r, "%08llx  %s |%s|", (unsigned long long)(r->fileoff + o - r->lo), hx, as);
        }
        break; }
    }
}

/* ------------------------------------------------------------------ */
/* code rooms                                                          */
/* ------------------------------------------------------------------ */

#define SHF_EXECINSTR 0x4
/* No cap: a room is sized for its whole function, so showing only the
   first N instructions leaves most of the floor bare.  Only one room is
   decoded at a time, and the largest function in /usr/bin is ~5000
   instructions, so this is under a megabyte of Insn.                    */
#define MAX_INS_PER_ROOM 0
/* ... but do cap the bytes.  The largest real function measured across
   /usr/lib is 38 KiB; a 330 KiB "function" is a slab of data that the
   boundary recovery guessed wrong about.                              */
#define MAX_ROOM_BYTES (128u * 1024u)

int room_is_code(const Building *b, const Room *r){
    /* A chamber is not one run of code -- it holds several unrelated units,
       and decoding r->data would disassemble the first one across the whole
       room.  Its alcoves are decoded one by one, by city_enter_room().    */
    return b && r && b->sec && (b->sec->flags & SHF_EXECINSTR) &&
           r->data && r->datasz > 0 &&
           r->kind != RT_EMPTY && r->kind != RT_LIST && r->kind != RT_GROUP;
}

int unit_is_code(const Building *b, const Unit *u){
    return b && u && b->sec && (b->sec->flags & SHF_EXECINSTR) &&
           u->data && u->datasz > 0;
}

/* which room, if any, currently holds a decoding */
/* Two rooms may be decoded at once, and only two: the one you are standing
   in, and the one the wisp is in when it has gone on without you (§8 B3,
   trimmed to exactly what that needs).  They are usually the same room, in
   which case only one decoding exists and both slots name it -- so the
   sharing has to be checked before anything is freed.               */
static Building *g_openBld;
static int       g_openRoom = -1;
static Building *g_wispBld;
static int       g_wispRoom = -1;

static void room_undecode(Room *r){
    disasm_free(r->dis);
    r->dis = NULL;
    for (int i = 0; i < r->nunits && r->units; i++){
        disasm_free(r->units[i].dis);
        r->units[i].dis = NULL;
    }
    r->activeUnit = -1;
}

static int same_room(Building *b1, int r1, Building *b2, int r2){
    return b1 && b1 == b2 && r1 >= 0 && r1 == r2;
}

void city_leave_room(City *c){
    (void)c;
    if (g_openBld && g_openRoom >= 0 && g_openRoom < g_openBld->nrooms &&
        !same_room(g_openBld, g_openRoom, g_wispBld, g_wispRoom))
        room_undecode(&g_openBld->rooms[g_openRoom]);
    g_openBld = NULL; g_openRoom = -1;
}

/* the room the wisp is in, when it is not the one you are in */
void city_leave_wisp_room(City *c){
    (void)c;
    if (g_wispBld && g_wispRoom >= 0 && g_wispRoom < g_wispBld->nrooms &&
        !same_room(g_wispBld, g_wispRoom, g_openBld, g_openRoom))
        room_undecode(&g_wispBld->rooms[g_wispRoom]);
    g_wispBld = NULL; g_wispRoom = -1;
}

int city_wisp_room(const City *c, int *bi, int *ri){
    if (!g_wispBld || g_wispRoom < 0) return 0;
    for (int i = 0; i < c->nbld; i++)
        if (&c->bld[i] == g_wispBld){
            if (bi) *bi = i;
            if (ri) *ri = g_wispRoom;
            return 1;
        }
    return 0;
}

/* name the far end of every wire that leaves the room */
static void label_ports(const Elf *e, Disasm *d){
    if (!d) return;
    for (int i = 0; i < d->nports; i++){
        Port *p = &d->ports[i];
        uint64_t off = 0;
        const char *nm = elf_sym_at(e, p->addr, &off);
        if (nm && nm[0] && !off) snprintf(p->label, sizeof p->label, "%s", nm);
        else if (nm && nm[0])    snprintf(p->label, sizeof p->label, "%s+%llu", nm,
                                          (unsigned long long)off);
        else                     snprintf(p->label, sizeof p->label, "0x%llx",
                                          (unsigned long long)p->addr);
    }
}

/* A chamber's alcoves are separate decodings but one room: a call from one
   to another has not left the room, so it wires straight to the sculpture
   instead of earning a port.                                             */
static void chamber_link(Room *r){
    for (int i = 0; i < r->nunits; i++){
        Disasm *d = r->units[i].dis;
        if (!d) continue;
        for (int q = 0; q < d->n; q++){
            Insn *t = &d->ins[q];
            if (t->target >= 0 || !t->taddr) continue;
            for (int j = 0; j < r->nunits; j++){
                if (j == i) continue;
                int at = disasm_index_of(r->units[j].dis, t->taddr);
                if (at < 0) continue;
                t->target = at; t->tunit = j;
                d->nlinks++; d->nexits--;
                break;
            }
        }
    }
}

static Disasm *decode(const uint8_t *data, uint64_t datasz, uint64_t size,
                      uint64_t addr, uint64_t fileoff){
    uint64_t n = datasz;
    if (size && n > size) n = size;
    /* A symbol of unrecorded size runs to the end of its section, and a run
       of data mistaken for a function can be just as long: cap it, or one
       step through the wrong door decodes a megabyte.                     */
    if (n > MAX_ROOM_BYTES) n = MAX_ROOM_BYTES;
    return disasm_run(data, n, addr ? addr : fileoff, MAX_INS_PER_ROOM);
}

/* decode a room, whichever slot wants it; 1 if there is code there */
static int room_decode(Building *b, Room *r){
    if (r->kind == RT_GROUP && r->units){
        if (r->units[0].dis) return 1;                /* the other slot has it */
        int any = 0;
        for (int i = 0; i < r->nunits; i++){
            Unit *u = &r->units[i];
            if (!unit_is_code(b, u)) continue;
            u->dis = decode(u->data, u->datasz, u->size, u->addr, u->fileoff);
            if (u->dis) any = 1;
        }
        if (!any) return 0;
        chamber_link(r);
        for (int i = 0; i < r->nunits; i++){
            disasm_ports(r->units[i].dis);
            label_ports(b->elf, r->units[i].dis);
        }
        return 1;
    }
    if (r->dis) return 1;                             /* the other slot has it */
    if (!room_is_code(b, r)) return 0;
    r->dis = decode(r->data, r->datasz, r->size, r->addr, r->fileoff);
    if (!r->dis) return 0;
    disasm_ports(r->dis);
    label_ports(b->elf, r->dis);
    return 1;
}

/* Where the wisp is when it has gone on without you.  The room you are
   standing in stays decoded; this is the only other one that ever is. */
void city_enter_wisp_room(City *c, int bi, int ri){
    if (bi >= 0 && bi < c->nbld && g_wispBld == &c->bld[bi] && g_wispRoom == ri) return;
    city_leave_wisp_room(c);
    if (bi < 0 || bi >= c->nbld) return;
    Building *b = &c->bld[bi];
    if (ri < 0 || ri >= b->nrooms) return;
    if (!room_decode(b, &b->rooms[ri])) return;
    g_wispBld = b; g_wispRoom = ri;
}

void city_enter_room(City *c, int bi, int ri){
    if (bi >= 0 && bi < c->nbld && g_openBld == &c->bld[bi] && g_openRoom == ri) return;
    city_leave_room(c);
    if (bi < 0 || bi >= c->nbld) return;
    Building *b = &c->bld[bi];
    if (ri < 0 || ri >= b->nrooms) return;
    Room *r = &b->rooms[ri];

    if (!room_decode(b, r)) return;
    g_openBld = b; g_openRoom = ri;
}

/* ------------------------------------------------------------------ */
/* finding an address in the city                                      */
/* ------------------------------------------------------------------ */

int city_find_addr(const City *c, uint64_t addr, int *bi, int *ri, int *ui){
    if (!c || !addr) return 0;
    for (int i = 0; i < c->nbld; i++){
        const Building *b = &c->bld[i];
        const Sec *s = b->sec;
        if (!s || !s->addr || addr < s->addr || addr >= s->addr + s->size) continue;
        /* the room that starts nearest below the address; chambers are
           checked alcove by alcove, since their own addr is only the
           lowest of the bunch                                          */
        int best = -1, bestu = -1;
        uint64_t bestat = 0;
        for (int k = 0; k < b->nrooms; k++){
            const Room *r = &b->rooms[k];
            if (r->kind == RT_GROUP && r->units){
                for (int u = 0; u < r->nunits; u++){
                    const Unit *un = &r->units[u];
                    if (!un->addr || un->addr > addr) continue;
                    if (un->size && addr >= un->addr + un->size) continue;
                    if (best < 0 || un->addr > bestat){ bestat = un->addr; best = k; bestu = u; }
                }
                continue;
            }
            if (!r->addr || r->addr > addr) continue;
            if (r->size && addr >= r->addr + r->size) continue;
            if (best < 0 || r->addr > bestat){ bestat = r->addr; best = k; bestu = -1; }
        }
        if (best < 0) continue;
        if (bi) *bi = i;
        if (ri) *ri = best;
        if (ui) *ui = bestu;
        return 1;
    }
    return 0;
}

void floor_release(Building *b){
    if (b->realized < 0) return;
    int a = b->floorStart[b->realized], z = b->floorStart[b->realized + 1];
    for (int i = a; i < z; i++){
        Room *r = &b->rooms[i];
        for (int k = 0; k < r->nlines; k++) free(r->lines[k]);
        free(r->lines); r->lines = NULL; r->nlines = 0;
    }
    b->realized = -1;
}

void floor_realize(Building *b, int f){
    if (f < 0 || f >= b->nfloors || b->realized == f) return;
    floor_release(b);
    for (int i = b->floorStart[f]; i < b->floorStart[f + 1]; i++) realize_room(b, &b->rooms[i]);
    b->realized = f;
}

/* ------------------------------------------------------------------ */
/* building assembly                                                   */
/* ------------------------------------------------------------------ */

static const float DISTRICT_COL[DK_COUNT][3] = {
    { 0.42f, 0.62f, 0.86f },   /* CODE    - blue glass    */
    { 0.78f, 0.62f, 0.36f },   /* RODATA  - sandstone     */
    { 0.80f, 0.44f, 0.40f },   /* DATA    - red brick     */
    { 0.55f, 0.78f, 0.62f },   /* LINKAGE - green         */
    { 0.60f, 0.58f, 0.66f },   /* DEBUG   - grey concrete */
    { 0.70f, 0.70f, 0.50f },   /* MISC                    */
};

/* ------------------------------------------------------------------ */
/* content grid                                                        */
/* ------------------------------------------------------------------ */

/* the most-filled near-square rectangle holding at least n tiles.
   15 -> 3x5 exactly; 17 -> 4x5, because 1x17 is not a room.            */
static void tile_rect(int n, int *w, int *h){
    if (n < 1) n = 1;
    int a = (int)sqrt((double)n);
    if (a < 2) a = 2;
    int b = (n + a - 1) / a;
    if (b < a){ int t = a; a = b; b = t; }
    *w = b; *h = a;                       /* long side along the row */
}

/* how many tiles of content a room holds: one per instruction, per
   16-byte line, or per printed row, depending on what it is.           */
static int room_tiles(const Elf *e, const Room *r){
    uint64_t n;
    switch (r->kind){
    case RT_FUNC: {
        /* Bytes per instruction.  A single constant cannot fit both ends:
           measured over 1500 git rooms, short functions run about 3.2 B an
           instruction (prologue and epilogue are one and two byte ops)
           while the largest run 5.2 (SSE, long displacements).  So take it
           as rising with the log of the size, and sit a little under the
           measurement so the grid still holds the decode.                */
        if (e->machine == 40 || e->machine == 183){          /* fixed width */
            n = (r->size + 3) / 4;
        } else if (e->machine == 243){                        /* RISC-V     */
            n = (r->size + 2) / 3;
        } else {                                              /* x86, x86-64 */
            double bpi = 2.9 + 0.20 * (log((double)(r->size ? r->size : 1) / 64.0) / log(2.0));
            if (bpi < 2.8) bpi = 2.8;
            if (bpi > 5.0) bpi = 5.0;
            n = (uint64_t)((double)r->size / bpi) + 1;
        }
        break; }
    case RT_LIST:   n = r->hi > r->lo ? r->hi - r->lo : 1; break;
    case RT_EMPTY:  n = 1; break;
    default:        n = (r->size + 15) / 16; break;
    }
    if (n < 1) n = 1;
    if (n > 40000) n = 40000;
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* chambers: small units share a 3x3 room with the middle left open    */
/* ------------------------------------------------------------------ */

/* the seven cells a visitor can reach: the middle is circulation and
   (1,0) is the doorway.                                               */
#define GROUP_PER 7   /* 3x3 cells, less the middle and the doorway */

static int groupable(const Room *r){
    return (r->kind == RT_FUNC || r->kind == RT_OBJECT ||
            r->kind == RT_BYTES || r->kind == RT_EMPTY) &&
           r->ntiles <= GROUP_TILES;
}

static int cmp_tiles_desc(const void *a, const void *b){
    const Room *x = a, *y = b;
    if (x->ntiles != y->ntiles) return x->ntiles < y->ntiles ? 1 : -1;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return 0;
}

/* Replaces runs of small rooms with chambers.  Returns the new count. */
static int group_small(Room **rp, int n){
    Room *r = *rp;
    int nsmall = 0;
    for (int i = 0; i < n; i++) if (groupable(&r[i])) nsmall++;
    if (nsmall < GROUP_PER * 2) return n;           /* not worth it */

    Room *big = malloc((size_t)(n - nsmall + 1) * sizeof(Room));
    Room *sml = malloc((size_t)nsmall * sizeof(Room));
    int nb = 0, ns = 0;
    for (int i = 0; i < n; i++){
        if (groupable(&r[i])) sml[ns++] = r[i]; else big[nb++] = r[i];
    }
    /* group units of similar size together, so cells waste little */
    qsort(sml, (size_t)ns, sizeof(Room), cmp_tiles_desc);

    int nchamber = (ns + GROUP_PER - 1) / GROUP_PER;
    Room *out = malloc((size_t)(nb + nchamber) * sizeof(Room));
    memcpy(out, big, (size_t)nb * sizeof(Room));
    int m = nb;

    for (int i = 0; i < ns; i += GROUP_PER){
        int k = ns - i < GROUP_PER ? ns - i : GROUP_PER;
        Room *c = &out[m++];
        memset(c, 0, sizeof *c);
        c->kind = RT_GROUP; c->symidx = -1; c->linkPrev = c->linkNext = -1;
        c->activeUnit = -1;
        c->units = malloc((size_t)k * sizeof(Unit));
        c->nunits = k;
        int maxt = 1; uint64_t lo = UINT64_MAX, tot = 0;
        for (int j = 0; j < k; j++){
            Room *u = &sml[i + j];
            Unit *un = &c->units[j];
            memset(un, 0, sizeof *un);
            un->addr = u->addr; un->size = u->size; un->fileoff = u->fileoff;
            un->data = u->data; un->datasz = u->datasz; un->symidx = u->symidx;
            un->cell = j; un->ntiles = u->ntiles;
            tile_rect(u->ntiles, &un->tw, &un->th);
            snprintf(un->title, sizeof un->title, "%s", u->title);
            if (u->ntiles > maxt) maxt = u->ntiles;
            if (u->addr && u->addr < lo) lo = u->addr;
            tot += u->size;
            if (!c->data){ c->data = u->data; c->datasz = u->datasz; }
        }
        tile_rect(maxt, &c->cellw, &c->cellh);
        c->tw = c->cellw * 3; c->th = c->cellh * 3;
        c->ntiles = c->tw * c->th;
        c->addr = lo == UINT64_MAX ? 0 : lo;
        c->size = tot;
        c->fileoff = sml[i].fileoff;
        snprintf(c->title, sizeof c->title, "%d small units", k);
        if (c->addr) snprintf(c->sub, sizeof c->sub, "%d units  %llu bytes  from 0x%llx",
                              k, (unsigned long long)tot, (unsigned long long)c->addr);
        else         snprintf(c->sub, sizeof c->sub, "%d units  %llu bytes",
                              k, (unsigned long long)tot);
    }
    free(big); free(sml); free(r);
    *rp = out;
    return m;
}

/* ------------------------------------------------------------------ */
/* floor plate + shelf packing                                         */
/* ------------------------------------------------------------------ */

static void room_metres(const Building *b, const Room *r, float *w, float *d){
    float t = b->tile;
    float ww = r->tw * t + 2 * TILE_MARGIN + 2 * WALL_T;
    float dd = r->th * t + TILE_MARGIN + TILE_SETBACK;
    *w = ww < b->wmin ? b->wmin : ww;
    *d = dd < b->dmin ? b->dmin : dd;
}

typedef struct { float used[MAX_ROWS], depth[MAX_ROWS]; int cnt[MAX_ROWS]; int nrow, count; } Fl;

static int cmp_int(const void *a, const void *b){
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_area_desc(const void *a, const void *b){
    const Room *const *x = a, *const *y = b;
    long ax = (long)(*x)->tw * (*x)->th, ay = (long)(*y)->tw * (*y)->th;
    if (ax != ay) return ax < ay ? 1 : -1;
    if ((*x)->addr != (*y)->addr) return (*x)->addr < (*y)->addr ? -1 : 1;
    return 0;
}

/* Places every room on a floor and a row.  Returns the floor count. */
static int shelf_pack(Building *b, Room **ord, int n, float W, float D){
    float usable = W - CORR_W;                    /* the spine eats the rest */
    Fl *fl = NULL; int nfl = 0, cap = 0, startf = 0;
    for (int i = 0; i < n; i++){
        Room *r = ord[i];
        float w, d; room_metres(b, r, &w, &d);
        int done = 0;
        while (startf < nfl && fl[startf].count >= MAXPF) startf++;
        for (int f = startf; f < nfl && !done; f++){
            Fl *F = &fl[f];
            if (F->count >= MAXPF) continue;
            float usedD = 0;
            for (int k = 0; k < F->nrow; k++) usedD += F->depth[k] + CORR_W;
            for (int k = 0; k < F->nrow; k++){
                if (F->used[k] + w + WALL_T > usable) continue;
                if (d <= F->depth[k]){
                    F->used[k] += w + WALL_T; F->cnt[k]++; F->count++;
                    r->floor = f; r->row = k; done = 1; break;
                }
                if (usedD - F->depth[k] + d + CORR_W <= D){      /* deepen it */
                    F->depth[k] = d;
                    F->used[k] += w + WALL_T; F->cnt[k]++; F->count++;
                    r->floor = f; r->row = k; done = 1; break;
                }
            }
            if (done) break;
            if (F->nrow < MAX_ROWS && usedD + d + CORR_W <= D){
                int k = F->nrow++;
                F->used[k] = w + WALL_T; F->depth[k] = d; F->cnt[k] = 1;
                F->count++; r->floor = f; r->row = k; done = 1;
            }
        }
        if (!done){
            if (nfl == cap){ cap = cap ? cap * 2 : 64;
                             fl = realloc(fl, (size_t)cap * sizeof *fl); }
            Fl *F = &fl[nfl];
            memset(F, 0, sizeof *F);
            F->nrow = 1; F->used[0] = w + WALL_T; F->depth[0] = d; F->cnt[0] = 1;
            F->count = 1;
            r->floor = nfl; r->row = 0;
            nfl++;
        }
    }
    /* second pass: give every room its rect now that row depths are final */
    for (int f = 0; f < nfl; f++){
        Fl *F = &fl[f];
        float z = 0, cur[MAX_ROWS], rz[MAX_ROWS];
        for (int k = 0; k < F->nrow; k++){
            z += CORR_W;                        /* this row's corridor */
            rz[k] = z;
            z += F->depth[k];
            cur[k] = CORR_W;                    /* rooms start past the spine */
        }
        /* spread the row's leftover into the gaps between its rooms */
        float slack[MAX_ROWS];
        for (int k = 0; k < F->nrow; k++)
            slack[k] = F->cnt[k] ? (usable - F->used[k]) / (float)(F->cnt[k] + 1) : 0;
        for (int i = 0; i < n; i++){
            Room *r = ord[i];
            if (r->floor != f) continue;
            int k = r->row;
            float w, d; room_metres(b, r, &w, &d);
            cur[k] += slack[k];
            r->x0 = cur[k]; r->x1 = cur[k] + w;
            cur[k] += w + WALL_T;
            r->z0 = rz[k]; r->z1 = rz[k] + d;
        }
    }
    free(fl);
    return nfl;
}

static int cmp_place(const void *a, const void *b){
    const Room *x = a, *y = b;
    if (x->floor != y->floor) return x->floor < y->floor ? -1 : 1;
    if (x->row   != y->row)   return x->row   < y->row   ? -1 : 1;
    if (x->x0    != y->x0)    return x->x0    < y->x0    ? -1 : 1;
    return 0;
}

static void build_one(Building *b, Elf *e, Sec *s,
                      const Unitrange *units, int nunits){
    memset(b, 0, sizeof *b);
    b->elf = e; b->sec = s; b->district = s->district; b->realized = -1;
    snprintf(b->label, sizeof b->label, "%s", s->name);
    for (int i = 0; i < 3; i++){
        float j = ((float)((s->index * 2654435761u) >> (i * 8) & 31) / 31.0f - 0.5f) * 0.10f;
        b->col[i] = clampf(DISTRICT_COL[s->district][i] + j, 0.08f, 0.98f);
    }

    RoomVec v = {0};
    int isexec = (s->flags & 0x4) != 0;
    /* Recovered boundaries beat symbols for code: a stripped file still has
       a .dynsym full of imports, which covers almost none of .text.       */
    if (isexec && nunits > 0 && rooms_from_units(&v, e, s, units, nunits))
        ;
    else if (s->symCount > 0)               rooms_from_syms(&v, e, s);
    else if (s->type == SHT_STRTAB)         rooms_from_strings(&v, s);
    else if (s->type == SHT_SYMTAB || s->type == SHT_DYNSYM)
        rooms_from_entries(&v, s, LS_SYMS, s->entsize ? s->entsize : (e->is64 ? 24u : 16u), "symbols");
    else if (s->type == SHT_DYNAMIC)
        rooms_from_entries(&v, s, LS_DYN, s->entsize ? s->entsize : (e->is64 ? 16u : 8u), "entries");
    else if (s->type == SHT_RELA)
        rooms_from_entries(&v, s, LS_RELA, s->entsize ? s->entsize : (e->is64 ? 24u : 12u), "relocs");
    else if (s->type == SHT_REL)
        rooms_from_entries(&v, s, LS_REL, s->entsize ? s->entsize : (e->is64 ? 16u : 8u), "relocs");
    else if (s->type == SHT_NOTE){
        Room *r = rv_add(&v);
        r->kind = RT_LIST; r->lsrc = LS_NOTE; r->lo = 0; r->hi = s->datasz;
        r->data = s->data; r->datasz = s->datasz; r->addr = s->addr; r->fileoff = s->offset;
        r->size = s->size;
        snprintf(r->title, sizeof r->title, "notes");
        snprintf(r->sub, sizeof r->sub, "%llu bytes", (unsigned long long)s->size);
    }
    else                                    rooms_from_bytes(&v, s);

    if (v.n == 0){
        Room *r = rv_add(&v);
        r->kind = RT_EMPTY; r->addr = s->addr; r->fileoff = s->offset; r->size = s->size;
        snprintf(r->title, sizeof r->title, "%s", s->name);
        snprintf(r->sub, sizeof r->sub, "%s, %llu bytes", elf_sectype_name(s->type),
                 (unsigned long long)s->size);
    }
    b->rooms = v.r; b->nrooms = v.n;
    b->truncated = (v.n >= MAX_ROOMS_PER_BLD);

    /* --- how much content each room holds ------------------------- */
    for (int i = 0; i < b->nrooms; i++){
        Room *r = &b->rooms[i];
        r->ntiles = room_tiles(e, r);
        tile_rect(r->ntiles, &r->tw, &r->th);
    }

    /* --- small units share a chamber ------------------------------ */
    b->nrooms = group_small(&b->rooms, b->nrooms);

    /* --- metres per tile -----------------------------------------
       Set from the *typical* room, not the largest.  Sizing off the
       largest squeezes every other room down to the minimum, and since
       a floor holds at most MAXPF rooms either way, the only thing that
       buys is a more slender tower.  The largest room is merely capped. */
    int maxside = 1;
    {
        int nr = b->nrooms > 0 ? b->nrooms : 1;
        int *sides = malloc((size_t)nr * sizeof(int));
        for (int i = 0; i < b->nrooms; i++){
            int sv = b->rooms[i].tw > b->rooms[i].th ? b->rooms[i].tw : b->rooms[i].th;
            sides[i] = sv;
            if (sv > maxside) maxside = sv;
        }
        qsort(sides, (size_t)nr, sizeof(int), cmp_int);
        int med = b->nrooms > 0 ? sides[b->nrooms / 2] : 1;
        if (med < 1) med = 1;
        free(sides);
        b->tile = ROOM_TYPICAL / (float)med;
        if (b->tile > TILE_M) b->tile = TILE_M;
        if (maxside * b->tile > PLATE_MAX_SIDE) b->tile = PLATE_MAX_SIDE / (float)maxside;
        if (b->tile < TILE_MIN) b->tile = TILE_MIN;
        /* and no room smaller than about half the typical one, so a
           building of uniformly tiny units is not a stack of closets  */
        float typ = med * b->tile + 2 * TILE_MARGIN + 2 * WALL_T;
        b->wmin = clampf(typ * 0.55f, ROOM_W_MIN, 8.0f);
        b->dmin = clampf(typ * 0.50f, ROOM_D_MIN, 7.0f);
    }

    /* --- the floor plate: big enough for the largest room, and for a
           full quota of typical ones ------------------------------- */
    float wmax = 0, dmax = 0, asum = 0;
    for (int i = 0; i < b->nrooms; i++){
        float w, d; room_metres(b, &b->rooms[i], &w, &d);
        if (w > wmax) wmax = w;
        if (d > dmax) dmax = d;
        asum += w * d;
    }
    float mean = b->nrooms ? asum / (float)b->nrooms : 1.0f;
    /* room for a full floor -- but never for more rooms than exist, or a
       three-room section gets the footprint of a ten-room one            */
    int quota = b->nrooms < MAXPF ? b->nrooms : MAXPF;
    float A = wmax * dmax;
    if (quota * mean > A) A = quota * mean;
    A /= PLATE_FILL;
    float side = sqrtf(A);
    b->plateW = wmax + CORR_W + 2 * WALL_T;  if (side > b->plateW) b->plateW = side;
    b->plateD = dmax + CORR_W + 2 * WALL_T;
    if (A / b->plateW > b->plateD) b->plateD = A / b->plateW;
    b->len = b->plateW;

    /* --- pack, biggest first -------------------------------------- */
    Room **ord = malloc((size_t)(b->nrooms ? b->nrooms : 1) * sizeof(Room *));
    for (int i = 0; i < b->nrooms; i++) ord[i] = &b->rooms[i];
    qsort(ord, (size_t)b->nrooms, sizeof(Room *), cmp_area_desc);

    /* The area bounds above can still leave a plate that does not
       geometrically hold MAXPF rooms -- one wide room blocks a row.  Grow
       and repack until the floor count stops falling: fewer floors on a
       wider plate is exactly the surface-to-height trade we want.        */
    int lb = (b->nrooms + MAXPF - 1) / MAXPF;
    int bestF = shelf_pack(b, ord, b->nrooms, b->plateW, b->plateD);
    float bestW = b->plateW, bestD = b->plateD;
    float W = b->plateW, D = b->plateD;
    /* One step may not cross a threshold -- a row only takes another room
       once the plate has grown a whole room's width -- so keep going and
       remember the smallest plate that gave the fewest floors.           */
    for (int it = 0; it < 10 && bestF > lb; it++){
        W *= ASPECT_STEP; D *= ASPECT_STEP;
        int nf = shelf_pack(b, ord, b->nrooms, W, D);
        if (nf < bestF){ bestF = nf; bestW = W; bestD = D; }
    }
    b->plateW = bestW; b->plateD = bestD;
    b->nfloors = shelf_pack(b, ord, b->nrooms, bestW, bestD);
    b->len = b->plateW;
    free(ord);
    if (b->nfloors < 1) b->nfloors = 1;

    /* --- reorder so a floor's rooms are contiguous ----------------- */
    qsort(b->rooms, (size_t)b->nrooms, sizeof(Room), cmp_place);
    b->floorStart = malloc((size_t)(b->nfloors + 1) * sizeof(int));
    {
        int f = 0;
        b->floorStart[0] = 0;
        for (int i = 0; i < b->nrooms; i++)
            while (f < b->rooms[i].floor){ b->floorStart[++f] = i; }
        while (f < b->nfloors) b->floorStart[++f] = b->nrooms;
    }

    /* --- enfilade doors: contiguous code running on into code ------ */
    for (int f = 0; f < b->nfloors; f++)
        for (int i = b->floorStart[f]; i + 1 < b->floorStart[f + 1]; i++){
            Room *q = &b->rooms[i], *r = &b->rooms[i + 1];
            if (q->row != r->row) continue;
            if (q->kind == RT_GROUP || r->kind == RT_GROUP) continue;
            if (!q->addr || !r->addr) continue;
            if (q->addr + q->size + 16 < r->addr) continue;
            /* they must actually share enough wall for a doorway */
            float lo = q->z0 > r->z0 ? q->z0 : r->z0;
            float hi = q->z1 < r->z1 ? q->z1 : r->z1;
            if (hi - lo < DOOR_W + 0.4f) continue;
            q->linkNext = i + 1; r->linkPrev = i;
        }

    b->w = CORE_W + b->plateW;
    b->d = b->plateD;
    b->h = b->nfloors * FLOOR_H + 1.6f;
}

/* ------------------------------------------------------------------ */
/* packing                                                             */
/* ------------------------------------------------------------------ */

typedef struct { float w, d; float x, z; int id; } Box;

static int cmp_box(const void *a, const void *b){
    const Box *x = a, *y = b;
    if (x->d != y->d) return x->d < y->d ? 1 : -1;
    if (x->w != y->w) return x->w < y->w ? 1 : -1;
    return 0;
}

/* shelf pack; returns the bounding size in *ow,*od */
static void pack(Box *b, int n, float gapx, float gapz, float *ow, float *od){
    if (n <= 0){ *ow = *od = 0; return; }
    double area = 0; float widest = 0;
    for (int i = 0; i < n; i++){ area += (double)(b[i].w + gapx) * (b[i].d + gapz);
                                 if (b[i].w > widest) widest = b[i].w; }
    float target = (float)sqrt(area) * 1.55f;
    if (target < widest) target = widest;
    qsort(b, (size_t)n, sizeof(Box), cmp_box);
    float x = 0, z = 0, rowd = 0, maxw = 0;
    for (int i = 0; i < n; i++){
        if (x > 0 && x + b[i].w > target){ z += rowd + gapz; x = 0; rowd = 0; }
        b[i].x = x; b[i].z = z;
        x += b[i].w + gapx;
        if (x - gapx > maxw) maxw = x - gapx;
        if (b[i].d > rowd) rowd = b[i].d;
    }
    *ow = maxw; *od = z + rowd;
}

/* ------------------------------------------------------------------ */
/* dependency portals                                                  */
/* ------------------------------------------------------------------ */

static const char *SEARCH[] = {
    "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64",
    "/lib", "/usr/lib", "/usr/local/lib", NULL
};

static void resolve_lib(const char *name, char *out, size_t n){
    out[0] = 0;
    if (strchr(name, '/')){ if (!access(name, R_OK)) snprintf(out, n, "%s", name); return; }
    for (int i = 0; SEARCH[i]; i++){
        char p[1000];
        snprintf(p, sizeof p, "%s/%s", SEARCH[i], name);
        if (!access(p, R_OK)){ snprintf(out, n, "%s", p); return; }
    }
}

/* ------------------------------------------------------------------ */

City *city_build(Elf *e){
    City *c = calloc(1, sizeof(City));
    c->elf = e;

    /* function boundaries survive stripping in the unwind tables */
    Unitrange *units = NULL;
    int nunits = ehframe_units(e, &units);

    int n = 0;
    for (int i = 0; i < e->nsec; i++)
        if (e->sec[i].type != 0 && e->sec[i].name[0]) n++;
    c->bld = calloc((size_t)(n ? n : 1), sizeof(Building));
    for (int i = 0; i < e->nsec; i++){
        Sec *s = &e->sec[i];
        if (s->type == 0 || !s->name[0]) continue;
        build_one(&c->bld[c->nbld], e, s, units, nunits);
        s->bld = &c->bld[c->nbld];
        c->dcount[s->district]++;
        c->nbld++;
    }

    /* pack buildings inside each district, then districts inside the city */
    Box dboxes[DK_COUNT]; int nd = 0;
    struct { float ox, oz; int id; } dplot[DK_COUNT];
    Box *tmp = malloc((size_t)(c->nbld ? c->nbld : 1) * sizeof(Box));

    for (int d = 0; d < DK_COUNT; d++){
        if (!c->dcount[d]) continue;
        int k = 0;
        for (int i = 0; i < c->nbld; i++)
            if (c->bld[i].district == d){ tmp[k].w = c->bld[i].w; tmp[k].d = c->bld[i].d; tmp[k].id = i; k++; }
        float ow, od;
        pack(tmp, k, 10.0f, 16.0f, &ow, &od);
        for (int i = 0; i < k; i++){
            c->bld[tmp[i].id].bx = tmp[i].x;      /* district-local for now */
            c->bld[tmp[i].id].bz = tmp[i].z;
        }
        dboxes[nd].w = ow + 16.0f; dboxes[nd].d = od + 22.0f; dboxes[nd].id = d;
        dplot[nd].id = d;
        nd++;
    }
    float cw, cd;
    pack(dboxes, nd, 34.0f, 40.0f, &cw, &cd);
    for (int i = 0; i < nd; i++)
        for (int j = 0; j < nd; j++)
            if (dplot[j].id == dboxes[i].id){ dplot[j].ox = dboxes[i].x + 8.0f; dplot[j].oz = dboxes[i].z + 11.0f; }

    for (int d = 0; d < DK_COUNT; d++){
        c->dmin[d][0] = c->dmin[d][1] = 1e9f;
        c->dmax[d][0] = c->dmax[d][1] = -1e9f;
    }
    for (int i = 0; i < nd; i++){
        int d = dboxes[i].id;
        c->dmin[d][0] = dboxes[i].x;             c->dmin[d][1] = dboxes[i].z;
        c->dmax[d][0] = dboxes[i].x + dboxes[i].w; c->dmax[d][1] = dboxes[i].z + dboxes[i].d;
    }

    /* district-local -> world, and shift so the city is centred on X */
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        for (int j = 0; j < nd; j++)
            if (dplot[j].id == b->district){ b->bx += dplot[j].ox; b->bz += dplot[j].oz; break; }
        /* bx,bz currently hold the min corner; move to the local origin */
        b->bx += CORE_W;
        b->bz += CORR_W * 0.5f + ROOM_D;
    }

    float shiftx = -cw * 0.5f, shiftz = 40.0f;   /* leave room for the plaza at -Z */
    for (int i = 0; i < c->nbld; i++){ c->bld[i].bx += shiftx; c->bld[i].bz += shiftz; }
    for (int d = 0; d < DK_COUNT; d++)
        if (c->dcount[d]){
            c->dmin[d][0] += shiftx; c->dmax[d][0] += shiftx;
            c->dmin[d][1] += shiftz; c->dmax[d][1] += shiftz;
        }

    c->minx = shiftx - 20; c->maxx = shiftx + cw + 20;
    c->minz = -30;         c->maxz = shiftz + cd + 20;
    c->plazaR = 22.0f;

    /* portals to the libraries this file needs */
    if (e->nneeded){
        c->nportal = e->nneeded;
        c->portal = calloc((size_t)c->nportal, sizeof(Portal));
        for (int i = 0; i < c->nportal; i++){
            Portal *p = &c->portal[i];
            snprintf(p->name, sizeof p->name, "%s", e->needed[i]);
            resolve_lib(p->name, p->path, sizeof p->path);
            /* an avenue of gateways flanking the approach to the plaza */
            int side = (i & 1) ? 1 : -1, rank = i / 2;
            int col = rank % 8, ring = rank / 8;
            p->x = side * (13.0f + ring * 9.0f);
            p->z = -(c->plazaR + 7.0f + col * 12.0f);
            p->ang = 0.0f;
        }
    }
    /* the far plane has to clear the ground quad, which runs GROUND_MARGIN
       past the city on every side -- that, not tower height, is the term
       that dominates                                                     */
    {
        float gw = (c->maxx - c->minx) + 2 * GROUND_MARGIN;
        float gd = (c->maxz - c->minz) + 2 * GROUND_MARGIN;
        float gh = 0;
        for (int i = 0; i < c->nbld; i++) if (c->bld[i].h > gh) gh = c->bld[i].h;
        c->farPlane = sqrtf(gw*gw + gd*gd + gh*gh) * 1.05f + 50.0f;
    }

    free(units);
    free(tmp);
    return c;
}

void city_free(City *c){
    if (!c) return;
    city_leave_room(c);
    for (int i = 0; i < c->nbld; i++){
        Building *b = &c->bld[i];
        floor_release(b);
        for (int k = 0; k < b->nrooms; k++){
            Room *r = &b->rooms[k];
            for (int j = 0; j < r->nlines; j++) free(r->lines[j]);
            free(r->lines);
            for (int j = 0; j < r->nunits && r->units; j++)
                disasm_free(r->units[j].dis);     /* belt and braces */
            free(r->units);
            disasm_free(r->dis); r->dis = NULL;
        }
        free(b->rooms); free(b->floorStart);
    }
    free(c->bld); free(c->portal); free(c);
}
