/* elfload.c -- a self-contained ELF32/64 reader (no libelf needed) */
#define _GNU_SOURCE
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- ELF constants we care about ---------------------------------- */
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP 17
#define SHT_GNU_HASH 0x6ffffff6
#define SHT_GNU_verdef 0x6ffffffd
#define SHT_GNU_verneed 0x6ffffffe
#define SHT_GNU_versym 0x6fffffff

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_SONAME 14
#define DT_RPATH 15
#define DT_RUNPATH 29
#define DT_STRTAB 5

/* ---- byte access -------------------------------------------------- */
static int G_be;
static uint16_t rd16(const uint8_t *p){ return G_be ? (uint16_t)((p[0]<<8)|p[1]) : (uint16_t)((p[1]<<8)|p[0]); }
static uint32_t rd32(const uint8_t *p){
    return G_be ? ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]
                : ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0];
}
static uint64_t rd64(const uint8_t *p){
    return G_be ? ((uint64_t)rd32(p)<<32)|rd32(p+4) : ((uint64_t)rd32(p+4)<<32)|rd32(p);
}

typedef struct { const uint8_t *m; size_t n; int is64; } Img;
static int inbounds(Img *g, uint64_t off, uint64_t len){
    return off <= g->n && len <= g->n - off;
}
/* read a native word (4 or 8 bytes) */
static uint64_t rdw(Img *g, const uint8_t *p){ return g->is64 ? rd64(p) : rd32(p); }

static const char *strat(const uint8_t *tab, uint64_t tabsz, uint64_t off){
    if (!tab || off >= tabsz) return "";
    const char *s = (const char *)tab + off;
    /* ensure NUL within the table */
    if (!memchr(s, 0, (size_t)(tabsz - off))) return "";
    return s;
}

/* ---- names -------------------------------------------------------- */
const char *elf_machine_name(int m){
    switch (m){
    case 3: return "x86";      case 62: return "x86-64";  case 40: return "ARM";
    case 183: return "AArch64";case 20: return "PowerPC"; case 21: return "PowerPC64";
    case 8: return "MIPS";     case 243: return "RISC-V"; case 22: return "S390";
    case 2: return "SPARC";    case 42: return "SuperH";  case 0: return "none";
    default: return "unknown";
    }
}
const char *elf_type_name(int t){
    switch (t){ case 1: return "relocatable"; case 2: return "executable";
                case 3: return "shared object"; case 4: return "core dump";
                default: return "none"; }
}
const char *elf_sectype_name(uint32_t t){
    switch (t){
    case SHT_NULL: return "NULL"; case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB: return "SYMTAB"; case SHT_STRTAB: return "STRTAB";
    case SHT_RELA: return "RELA"; case SHT_HASH: return "HASH";
    case SHT_DYNAMIC: return "DYNAMIC"; case SHT_NOTE: return "NOTE";
    case SHT_NOBITS: return "NOBITS"; case SHT_REL: return "REL";
    case SHT_DYNSYM: return "DYNSYM"; case SHT_INIT_ARRAY: return "INIT_ARRAY";
    case SHT_FINI_ARRAY: return "FINI_ARRAY"; case SHT_PREINIT_ARRAY: return "PREINIT_ARRAY";
    case SHT_GROUP: return "GROUP"; case SHT_GNU_HASH: return "GNU_HASH";
    case SHT_GNU_verdef: return "VERDEF"; case SHT_GNU_verneed: return "VERNEED";
    case SHT_GNU_versym: return "VERSYM";
    default: return "OTHER";
    }
}
const char *elf_symtype_name(int t){
    switch (t){ case 0: return "notype"; case 1: return "object"; case 2: return "func";
                case 3: return "section"; case 4: return "file"; case 5: return "common";
                case 6: return "tls"; case 10: return "ifunc"; default: return "?"; }
}
const char *elf_symbind_name(int b){
    switch (b){ case 0: return "local"; case 1: return "global"; case 2: return "weak";
                case 10: return "gnu_unique"; default: return "?"; }
}
const char *district_name(int d){
    static const char *n[DK_COUNT] = { "CODE", "RODATA", "DATA", "LINKAGE", "DEBUG", "MISC" };
    return (d >= 0 && d < DK_COUNT) ? n[d] : "?";
}

/* ---- zoning ------------------------------------------------------- */
static int classify(const Sec *s){
    const char *n = s->name;
    if (!strncmp(n, ".debug", 6) || !strncmp(n, ".zdebug", 7) ||
        !strcmp(n, ".comment") || !strncmp(n, ".gnu_debug", 10) ||
        !strncmp(n, ".stab", 5) || !strncmp(n, ".pdb", 4))
        return DK_DEBUG;
    switch (s->type){
    case SHT_SYMTAB: case SHT_DYNSYM: case SHT_STRTAB: case SHT_DYNAMIC:
    case SHT_RELA: case SHT_REL: case SHT_HASH: case SHT_GNU_HASH:
    case SHT_NOTE: case SHT_GNU_verdef: case SHT_GNU_verneed:
    case SHT_GNU_versym: case SHT_GROUP:
        return DK_LINK;
    }
    if (s->flags & SHF_EXECINSTR) return DK_CODE;
    if (s->type == SHT_NOBITS || ((s->flags & SHF_WRITE) && (s->flags & SHF_ALLOC))) return DK_DATA;
    if (s->flags & SHF_ALLOC) return DK_RODATA;
    return DK_OTHER;
}

/* ---- symbol ordering ---------------------------------------------- */
static Sym *G_sym;
static int cmp_addrsym(const void *a, const void *b){
    const Sym *x = &G_sym[*(const int *)a], *y = &G_sym[*(const int *)b];
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    if (x->size  != y->size)  return x->size  > y->size  ? -1 : 1;
    return 0;
}
static int cmp_secsym(const void *a, const void *b){
    const Sym *x = &G_sym[*(const int *)a], *y = &G_sym[*(const int *)b];
    if (x->shndx != y->shndx) return x->shndx - y->shndx;
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    if (x->size  != y->size)  return x->size  > y->size  ? -1 : 1;   /* bigger first */
    return strcmp(x->name, y->name);
}

/* ---- symbol table reading ----------------------------------------- */
static void read_symtab(Elf *e, Img *g, Sec *st, int isdyn, Sym **arr, int *n, int *cap){
    if (!st->data || !st->entsize) return;
    if (st->link <= 0 || st->link >= (uint32_t)e->nsec) return;
    Sec *str = &e->sec[st->link];
    if (!str->data) return;
    uint64_t cnt = st->datasz / st->entsize;
    if (cnt > 4000000) cnt = 4000000;
    for (uint64_t i = 1; i < cnt; i++){
        const uint8_t *p = st->data + i * st->entsize;
        uint32_t nameoff; uint64_t value, size; uint16_t shndx; uint8_t info;
        if (g->is64){
            nameoff = rd32(p); info = p[4]; shndx = rd16(p+6);
            value = rd64(p+8); size = rd64(p+16);
        } else {
            nameoff = rd32(p); value = rd32(p+4); size = rd32(p+8);
            info = p[12]; shndx = rd16(p+14);
        }
        const char *nm = strat(str->data, str->datasz, nameoff);
        if (!nm[0]) continue;
        uint8_t ty = info & 0xf, bd = info >> 4;
        if (ty == 3 || ty == 4) continue;          /* section / file symbols */
        if (shndx == 0 || shndx >= (uint16_t)e->nsec) continue;  /* undefined / special */
        if (*n == *cap){ *cap = *cap ? *cap * 2 : 1024; *arr = realloc(*arr, (size_t)*cap * sizeof(Sym)); }
        Sym *s = &(*arr)[(*n)++];
        s->name = nm; s->value = value; s->size = size;
        s->shndx = shndx; s->type = ty; s->bind = bd; s->isdyn = (uint8_t)isdyn;
    }
}

/* ---- .dynamic ----------------------------------------------------- */
static void read_dynamic(Elf *e, Img *g, Sec *dyn){
    if (!dyn->data) return;
    uint64_t esz = g->is64 ? 16 : 8;
    if (dyn->entsize) esz = dyn->entsize;
    /* the string table for .dynamic is its sh_link, normally .dynstr */
    const uint8_t *stab = NULL; uint64_t stabsz = 0;
    if (dyn->link < (uint32_t)e->nsec && e->sec[dyn->link].data){
        stab = e->sec[dyn->link].data; stabsz = e->sec[dyn->link].datasz;
    }
    uint64_t cnt = dyn->datasz / esz;
    for (uint64_t i = 0; i < cnt; i++){
        const uint8_t *p = dyn->data + i * esz;
        uint64_t tag = rdw(g, p), val = rdw(g, p + (g->is64 ? 8 : 4));
        if (tag == DT_NULL) break;
        const char *s;
        switch (tag){
        case DT_NEEDED:
            s = strat(stab, stabsz, val);
            if (s[0] && e->nneeded < MAX_NEED) e->needed[e->nneeded++] = strdup(s);
            break;
        case DT_SONAME: s = strat(stab, stabsz, val); if (s[0] && !e->soname) e->soname = strdup(s); break;
        case DT_RPATH: case DT_RUNPATH:
            s = strat(stab, stabsz, val); if (s[0] && !e->rpath) e->rpath = strdup(s); break;
        }
    }
}

/* ---- open --------------------------------------------------------- */
Elf *elf_open(const char *path, char *err, size_t errlen){
    int fd = open(path, O_RDONLY);
    if (fd < 0){ snprintf(err, errlen, "cannot open %s", path); return NULL; }
    struct stat sb;
    if (fstat(fd, &sb) || !S_ISREG(sb.st_mode) || sb.st_size < 64){
        snprintf(err, errlen, "not a regular file (or too small)"); close(fd); return NULL;
    }
    void *m = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED){ snprintf(err, errlen, "mmap failed"); close(fd); return NULL; }
    const uint8_t *b = m;
    if (memcmp(b, "\177ELF", 4)){
        snprintf(err, errlen, "not an ELF file"); munmap(m, (size_t)sb.st_size); close(fd); return NULL;
    }
    Elf *e = calloc(1, sizeof(Elf));
    e->map = b; e->maplen = (size_t)sb.st_size; e->mapfd = fd;
    snprintf(e->path, sizeof e->path, "%s", path);
    const char *bs = strrchr(path, '/');
    snprintf(e->base, sizeof e->base, "%s", bs ? bs + 1 : path);

    e->is64 = (b[4] == 2);
    e->be   = (b[5] == 2);
    G_be = e->be;
    Img g = { b, e->maplen, e->is64 };

    uint64_t shoff, phoff; uint16_t shnum, shentsize, phnum, phentsize, shstrndx;
    e->etype   = rd16(b + 16);
    e->machine = rd16(b + 18);
    if (e->is64){
        e->entry = rd64(b + 24); phoff = rd64(b + 32); shoff = rd64(b + 40);
        phentsize = rd16(b + 54); phnum = rd16(b + 56);
        shentsize = rd16(b + 58); shnum = rd16(b + 60); shstrndx = rd16(b + 62);
    } else {
        e->entry = rd32(b + 24); phoff = rd32(b + 28); shoff = rd32(b + 32);
        phentsize = rd16(b + 42); phnum = rd16(b + 44);
        shentsize = rd16(b + 46); shnum = rd16(b + 48); shstrndx = rd16(b + 50);
    }

    /* program headers */
    if (phoff && phnum && inbounds(&g, phoff, (uint64_t)phnum * phentsize)){
        for (int i = 0; i < phnum && e->nseg < MAX_SEG; i++){
            const uint8_t *p = b + phoff + (uint64_t)i * phentsize;
            Seg *s = &e->seg[e->nseg++];
            if (e->is64){
                s->type = rd32(p); s->flags = rd32(p+4); s->offset = rd64(p+8);
                s->vaddr = rd64(p+16); s->filesz = rd64(p+32); s->memsz = rd64(p+40);
                s->align = rd64(p+48);
            } else {
                s->type = rd32(p); s->offset = rd32(p+4); s->vaddr = rd32(p+8);
                s->filesz = rd32(p+16); s->memsz = rd32(p+20); s->flags = rd32(p+24);
                s->align = rd32(p+28);
            }
            if (s->type == 3 /*PT_INTERP*/ && !e->interp && inbounds(&g, s->offset, s->filesz) && s->filesz){
                char *t = malloc((size_t)s->filesz + 1);
                memcpy(t, b + s->offset, (size_t)s->filesz); t[s->filesz] = 0;
                e->interp = t;
            }
        }
    }

    /* section headers */
    if (!shoff || !shnum || !inbounds(&g, shoff, (uint64_t)shnum * shentsize)){
        snprintf(err, errlen, "no usable section header table");
        elf_close(e); return NULL;
    }
    if (shnum > MAX_SEC) shnum = MAX_SEC;
    e->nsec = shnum;
    for (int i = 0; i < shnum; i++){
        const uint8_t *p = b + shoff + (uint64_t)i * shentsize;
        Sec *s = &e->sec[i];
        s->index = i;
        if (e->is64){
            s->type = rd32(p+4); s->flags = rd64(p+8); s->addr = rd64(p+16);
            s->offset = rd64(p+24); s->size = rd64(p+32); s->link = rd32(p+40);
            s->info = rd32(p+44); s->align = rd64(p+48); s->entsize = rd64(p+56);
        } else {
            s->type = rd32(p+4); s->flags = rd32(p+8); s->addr = rd32(p+12);
            s->offset = rd32(p+16); s->size = rd32(p+20); s->link = rd32(p+24);
            s->info = rd32(p+28); s->align = rd32(p+32); s->entsize = rd32(p+36);
        }
        s->symFirst = s->symCount = 0;
        if (s->type != SHT_NOBITS && s->type != SHT_NULL && s->size &&
            inbounds(&g, s->offset, s->size)){
            s->data = b + s->offset; s->datasz = s->size;
        }
    }
    /* section names */
    if (shstrndx < shnum && e->sec[shstrndx].data){
        const uint8_t *st = e->sec[shstrndx].data; uint64_t sz = e->sec[shstrndx].datasz;
        for (int i = 0; i < shnum; i++){
            const uint8_t *p = b + shoff + (uint64_t)i * shentsize;
            uint32_t no = rd32(p);
            snprintf(e->sec[i].name, sizeof e->sec[i].name, "%s", strat(st, sz, no));
        }
    }
    for (int i = 0; i < shnum; i++){
        if (!e->sec[i].name[0]) snprintf(e->sec[i].name, sizeof e->sec[i].name, "<sec %d>", i);
        e->sec[i].district = classify(&e->sec[i]);
    }

    /* symbols */
    int cap = 0;
    e->stripped = 1;
    for (int i = 0; i < shnum; i++){
        if (e->sec[i].type == SHT_SYMTAB){ e->stripped = 0; read_symtab(e, &g, &e->sec[i], 0, &e->sym, &e->nsym, &cap); }
    }
    for (int i = 0; i < shnum; i++)
        if (e->sec[i].type == SHT_DYNSYM) read_symtab(e, &g, &e->sec[i], 1, &e->sym, &e->nsym, &cap);

    /* group symbols by section, sorted by address */
    if (e->nsym){
        e->secsym = malloc((size_t)e->nsym * sizeof(int));
        for (int i = 0; i < e->nsym; i++) e->secsym[i] = i;
        G_sym = e->sym;
        qsort(e->secsym, (size_t)e->nsym, sizeof(int), cmp_secsym);
        /* drop exact duplicates (same section, addr and name from symtab+dynsym) */
        int w = 0;
        for (int i = 0; i < e->nsym; i++){
            if (w > 0){
                Sym *a = &e->sym[e->secsym[w-1]], *b2 = &e->sym[e->secsym[i]];
                if (a->shndx == b2->shndx && a->value == b2->value && !strcmp(a->name, b2->name))
                    continue;
            }
            e->secsym[w++] = e->secsym[i];
        }
        e->nsym = w;
        int i = 0;
        while (i < e->nsym){
            int sh = e->sym[e->secsym[i]].shndx, j = i;
            while (j < e->nsym && e->sym[e->secsym[j]].shndx == sh) j++;
            if (sh >= 0 && sh < e->nsec){ e->sec[sh].symFirst = i; e->sec[sh].symCount = j - i; }
            i = j;
        }
    }

    if (e->nsym > 0){                   /* address-ordered index */
        e->addrsym = malloc((size_t)(unsigned)e->nsym * sizeof(int));
        e->naddrsym = 0;
        for (int i = 0; i < e->nsym; i++)
            if (e->sym[i].value) e->addrsym[e->naddrsym++] = i;
        G_sym = e->sym;
        qsort(e->addrsym, (size_t)e->naddrsym, sizeof(int), cmp_addrsym);
    }

    for (int i = 0; i < shnum; i++)
        if (e->sec[i].type == SHT_DYNAMIC) read_dynamic(e, &g, &e->sec[i]);

    return e;
}

void elf_close(Elf *e){
    if (!e) return;
    if (e->map) munmap((void *)e->map, e->maplen);
    if (e->mapfd >= 0) close(e->mapfd);
    for (int i = 0; i < e->nneeded; i++) free(e->needed[i]);
    free(e->soname); free(e->interp); free(e->rpath);
    free(e->sym); free(e->secsym); free(e->addrsym);
    free(e);
}

/* the symbol covering an address, if any; *off gets the distance into it */
const char *elf_sym_at(const Elf *e, uint64_t addr, uint64_t *off){
    if (off) *off = 0;
    if (!e->naddrsym || !addr) return NULL;
    int lo = 0, hi = e->naddrsym - 1, best = -1;
    while (lo <= hi){                        /* last symbol with value <= addr */
        int mid = (lo + hi) / 2;
        if (e->sym[e->addrsym[mid]].value <= addr){ best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    const Sym *y = &e->sym[e->addrsym[best]];
    uint64_t delta = addr - y->value;
    if (y->size && delta >= y->size) return NULL;
    if (!y->size && delta > 0) return NULL;
    if (off) *off = delta;
    return y->name;
}

/* ---- endian-aware readers exposed to the rest of the program ------- */
uint16_t elf_rd16(const Elf *e, const uint8_t *p){ G_be = e->be; return rd16(p); }
uint32_t elf_rd32(const Elf *e, const uint8_t *p){ G_be = e->be; return rd32(p); }
uint64_t elf_rd64(const Elf *e, const uint8_t *p){ G_be = e->be; return rd64(p); }
uint64_t elf_rdw (const Elf *e, const uint8_t *p){ G_be = e->be; return e->is64 ? rd64(p) : rd32(p); }
const char *elf_str(const Elf *e, int strsec, uint64_t off){
    if (strsec < 0 || strsec >= e->nsec) return "";
    return strat(e->sec[strsec].data, e->sec[strsec].datasz, off);
}

const char *elf_dyntag_name(uint64_t t){
    switch (t){
    case 0: return "NULL"; case 1: return "NEEDED"; case 2: return "PLTRELSZ";
    case 3: return "PLTGOT"; case 4: return "HASH"; case 5: return "STRTAB";
    case 6: return "SYMTAB"; case 7: return "RELA"; case 8: return "RELASZ";
    case 9: return "RELAENT"; case 10: return "STRSZ"; case 11: return "SYMENT";
    case 12: return "INIT"; case 13: return "FINI"; case 14: return "SONAME";
    case 15: return "RPATH"; case 16: return "SYMBOLIC"; case 17: return "REL";
    case 18: return "RELSZ"; case 19: return "RELENT"; case 20: return "PLTREL";
    case 21: return "DEBUG"; case 22: return "TEXTREL"; case 23: return "JMPREL";
    case 24: return "BIND_NOW"; case 25: return "INIT_ARRAY"; case 26: return "FINI_ARRAY";
    case 27: return "INIT_ARRAYSZ"; case 28: return "FINI_ARRAYSZ"; case 29: return "RUNPATH";
    case 30: return "FLAGS"; case 32: return "PREINIT_ARRAY"; case 33: return "PREINIT_ARRAYSZ";
    case 0x6ffffef5: return "GNU_HASH"; case 0x6ffffff0: return "VERSYM";
    case 0x6ffffffe: return "VERNEED"; case 0x6fffffff: return "VERNEEDNUM";
    case 0x6ffffffc: return "VERDEF"; case 0x6ffffffd: return "VERDEFNUM";
    case 0x6ffffff9: return "RELACOUNT"; case 0x6ffffffa: return "RELCOUNT";
    case 0x6ffffffb: return "FLAGS_1"; case 0x7ffffffd: return "AUXILIARY";
    default: return NULL;
    }
}

const char *elf_reltype_name(int machine, uint32_t t){
    if (machine == 62) switch (t){   /* x86-64 */
        case 0: return "NONE"; case 1: return "64"; case 2: return "PC32";
        case 3: return "GOT32"; case 4: return "PLT32"; case 5: return "COPY";
        case 6: return "GLOB_DAT"; case 7: return "JUMP_SLOT"; case 8: return "RELATIVE";
        case 9: return "GOTPCREL"; case 10: return "32"; case 11: return "32S";
        case 16: return "DTPMOD64"; case 17: return "DTPOFF64"; case 18: return "TPOFF64";
        case 19: return "TLSGD"; case 20: return "TLSLD"; case 21: return "DTPOFF32";
        case 22: return "GOTTPOFF"; case 23: return "TPOFF32"; case 24: return "PC64";
        case 37: return "IRELATIVE"; case 42: return "REX_GOTPCRELX";
    }
    if (machine == 183) switch (t){  /* AArch64 */
        case 257: return "ABS64"; case 258: return "ABS32"; case 1024: return "COPY";
        case 1025: return "GLOB_DAT"; case 1026: return "JUMP_SLOT"; case 1027: return "RELATIVE";
        case 1032: return "IRELATIVE";
    }
    if (machine == 3) switch (t){    /* i386 */
        case 1: return "32"; case 2: return "PC32"; case 5: return "COPY";
        case 6: return "GLOB_DAT"; case 7: return "JMP_SLOT"; case 8: return "RELATIVE";
    }
    return NULL;
}
