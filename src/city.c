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

#define MAX_ROOMS_PER_BLD 1400
#define MAX_FLOORS         34
#define LINES_PER_ROOM     22

static float clampf(float v, float a, float b){ return v < a ? a : (v > b ? b : v); }
static float log2f_safe(double v){ return (float)(log(v + 1.0) / log(2.0)); }

/* ------------------------------------------------------------------ */
/* room construction                                                   */
/* ------------------------------------------------------------------ */

typedef struct { Room *r; int n, cap; } RoomVec;

static Room *rv_add(RoomVec *v){
    if (v->n == v->cap){ v->cap = v->cap ? v->cap * 2 : 64; v->r = realloc(v->r, (size_t)v->cap * sizeof(Room)); }
    Room *r = &v->r[v->n++];
    memset(r, 0, sizeof *r);
    r->symidx = -1; r->linkPrev = -1; r->linkNext = -1;
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
#define MAX_INS_PER_ROOM 640

int room_is_code(const Building *b, const Room *r){
    return b && r && b->sec && (b->sec->flags & SHF_EXECINSTR) &&
           r->data && r->datasz > 0 && r->kind != RT_EMPTY && r->kind != RT_LIST;
}

/* which room, if any, currently holds a decoding */
static Building *g_openBld;
static int       g_openRoom = -1;

void city_leave_room(City *c){
    (void)c;
    if (g_openBld && g_openRoom >= 0 && g_openRoom < g_openBld->nrooms){
        Room *r = &g_openBld->rooms[g_openRoom];
        disasm_free(r->dis);
        r->dis = NULL;
    }
    g_openBld = NULL; g_openRoom = -1;
}

void city_enter_room(City *c, int bi, int ri){
    if (bi >= 0 && bi < c->nbld && g_openBld == &c->bld[bi] && g_openRoom == ri) return;
    city_leave_room(c);
    if (bi < 0 || bi >= c->nbld) return;
    Building *b = &c->bld[bi];
    if (ri < 0 || ri >= b->nrooms) return;
    Room *r = &b->rooms[ri];
    if (!room_is_code(b, r)) return;
    uint64_t n = r->datasz;
    if (r->size && n > r->size) n = r->size;
    r->dis = disasm_run(r->data, n, r->addr ? r->addr : r->fileoff, MAX_INS_PER_ROOM);
    if (r->dis){ g_openBld = b; g_openRoom = ri; }
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

static void build_one(Building *b, Elf *e, Sec *s){
    memset(b, 0, sizeof *b);
    b->elf = e; b->sec = s; b->district = s->district; b->realized = -1;
    snprintf(b->label, sizeof b->label, "%s", s->name);
    for (int i = 0; i < 3; i++){
        float j = ((float)((s->index * 2654435761u) >> (i * 8) & 31) / 31.0f - 0.5f) * 0.10f;
        b->col[i] = clampf(DISTRICT_COL[s->district][i] + j, 0.08f, 0.98f);
    }

    RoomVec v = {0};
    if (s->symCount > 0)                    rooms_from_syms(&v, e, s);
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

    /* --- stack the rooms into floors ----------------------------- */
    int per = 8;
    if (b->nrooms > per * MAX_FLOORS){
        per = (b->nrooms + MAX_FLOORS - 1) / MAX_FLOORS;
        if (per & 1) per++;
        if (per > 44) per = 44;
    }
    b->nfloors = (b->nrooms + per - 1) / per;
    if (b->nfloors < 1) b->nfloors = 1;
    if (b->nfloors > MAX_FLOORS) b->nfloors = MAX_FLOORS;
    b->floorStart = malloc((size_t)(b->nfloors + 1) * sizeof(int));
    for (int f = 0; f <= b->nfloors; f++){
        int i = f * per; if (i > b->nrooms) i = b->nrooms;
        b->floorStart[f] = i;
    }
    b->floorStart[b->nfloors] = b->nrooms;

    /* --- natural widths, then a corridor length that fits ---------- */
    float natural = 0;
    for (int f = 0; f < b->nfloors; f++){
        float side[2] = {0, 0};
        int a = b->floorStart[f], z = b->floorStart[f + 1];
        for (int i = a; i < z; i++){
            Room *r = &b->rooms[i];
            r->floor = f; r->side = (i - a) & 1;
            side[r->side] += clampf(2.8f + 1.15f * log2f_safe((double)r->size), 2.8f, 15.0f);
        }
        if (side[0] > natural) natural = side[0];
        if (side[1] > natural) natural = side[1];
    }
    b->len = clampf(natural, 16.0f, 90.0f);

    /* --- lay each floor out along the corridor -------------------- */
    for (int f = 0; f < b->nfloors; f++){
        int a = b->floorStart[f], z = b->floorStart[f + 1];
        float sum[2] = {0, 0};
        for (int i = a; i < z; i++){
            Room *r = &b->rooms[i];
            sum[r->side] += clampf(2.8f + 1.15f * log2f_safe((double)r->size), 2.8f, 15.0f);
        }
        float cur[2] = {0, 0};
        int last[2] = {-1, -1};
        for (int i = a; i < z; i++){
            Room *r = &b->rooms[i];
            float w = clampf(2.8f + 1.15f * log2f_safe((double)r->size), 2.8f, 15.0f);
            float scale = sum[r->side] > 0.01f ? b->len / sum[r->side] : 1.0f;
            if (scale > 2.6f) scale = 2.6f;
            r->x0 = cur[r->side];
            cur[r->side] += w * scale;
            r->x1 = cur[r->side];
            /* a door through the party wall when the two rooms are
               contiguous in the file -- code that runs on into code   */
            int p = last[r->side];
            if (p >= 0){
                Room *q = &b->rooms[p];
                if (q->addr && r->addr && q->addr + q->size + 16 >= r->addr){
                    q->linkNext = i; r->linkPrev = p;
                }
            }
            last[r->side] = i;
        }
    }

    b->w = CORE_W + b->len;
    b->d = CORR_W + 2 * ROOM_D;
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

    int n = 0;
    for (int i = 0; i < e->nsec; i++)
        if (e->sec[i].type != 0 && e->sec[i].name[0]) n++;
    c->bld = calloc((size_t)(n ? n : 1), sizeof(Building));
    for (int i = 0; i < e->nsec; i++){
        Sec *s = &e->sec[i];
        if (s->type == 0 || !s->name[0]) continue;
        build_one(&c->bld[c->nbld], e, s);
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
            disasm_free(r->dis); r->dis = NULL;   /* belt and braces */
        }
        free(b->rooms); free(b->floorStart);
    }
    free(c->bld); free(c->portal); free(c);
}
