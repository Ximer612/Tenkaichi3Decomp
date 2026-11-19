/* Taking characters, stages and songs out of a modded disc image, and the stages and songs folders as lists that can
 * be switched on and off (port/include/plat_discimport.h).
 *
 * The disc: its directory is read as UDF, else as ISO 9660 (bridge discs have both; some mods hide the game behind a
 * UDF tree that holds only VIDEO_TS), and DATA/PZS3US1.AFS and PZS3US2.AFS (PZS4US* on BT4 mods built on the USA
 * disc: the same numbering) are opened as AFS archives. Discs of other editions number their files differently and
 * are refused. A character's files are the ones character_packages.md names; what its table (section 18 of its
 * first model) says it turns into decides the forms and fusions brought along, and the folders and links are made
 * the way port/tools/import_chara.py makes them. Stages are files 0x171 + n with their picture (0x39D + n) and their
 * icon (the select's chip pack, section 46); songs are 0x10B16 + n (ADX).
 *
 * Names of stages and songs exist on a disc only as pictures: a stage or song whose file is the original game's gets
 * the original's name, any other "Stage n" / "Song n" until the player types one (Di_NamePicture shows what the
 * disc calls it). */
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../include/plat_discimport.h"
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#define SEEK64(f, o) _fseeki64((f), (long long)(o), SEEK_SET)
#else
#define MAKE_DIR(p) mkdir((p), 0755)
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#endif

#define SECTOR 2048
#define MODEL_MAX 0x800000L /* PORT_MODEL_MAX */
#define CHIP_MAX 5504

static uint32_t rd16(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
static uint32_t rd32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t rd64(const unsigned char *b) { return (uint64_t)rd32(b) | (uint64_t)rd32(b + 4) << 32; }

extern unsigned char *Port_BpeUnpack(const unsigned char *src, long n, long max, long *size); /* gs/modmgr.c */

/* ---- reading the disc -------------------------------------------------------------------------------------- */

typedef struct {
    uint64_t pos, len; /* bytes */
} Ext;

typedef struct {
    Ext ext[64];
    int n;
    uint64_t size;
} DFile;

static int read_at(FILE *f, uint64_t pos, void *buf, size_t n) {
    return SEEK64(f, pos) == 0 && fread(buf, 1, n, f) == n;
}

static int dfile_read(FILE *f, const DFile *df, uint64_t off, void *buf, size_t n) {
    unsigned char *o = buf;
    int i;
    for (i = 0; i < df->n && n > 0; i++) {
        if (off >= df->ext[i].len) {
            off -= df->ext[i].len;
            continue;
        }
        uint64_t take = df->ext[i].len - off;
        if (take > n) {
            take = n;
        }
        if (!read_at(f, df->ext[i].pos + off, o, (size_t)take)) {
            return 0;
        }
        o += take;
        n -= (size_t)take;
        off = 0;
    }
    return n == 0;
}

/* a name as the disc has it, compared to a file name of DATA (no version, any case) */
static int name_is(const char *disc, const char *want) {
    size_t i;
    for (i = 0; want[i] != '\0'; i++) {
        if (toupper((unsigned char)disc[i]) != toupper((unsigned char)want[i])) {
            return 0;
        }
    }
    return disc[i] == '\0' || disc[i] == ';';
}

typedef int (*FoundFn)(void *user, const char *name, const DFile *df, int isDir);

/* ISO 9660: the entries of a directory */
static int iso_dir(FILE *f, uint32_t lba, uint32_t size, FoundFn fn, void *user) {
    unsigned char *d;
    uint32_t p = 0;
    if (size == 0 || size > (16u << 20) || (d = malloc(size)) == NULL) {
        return 0;
    }
    if (!read_at(f, (uint64_t)lba * SECTOR, d, size)) {
        free(d);
        return 0;
    }
    while (p < size) {
        unsigned len = d[p];
        if (len == 0) {
            p = (p / SECTOR + 1) * SECTOR;
            continue;
        }
        if (p + len > size || len < 34) {
            break;
        }
        unsigned nl = d[p + 32];
        if (33 + nl <= len && !(nl == 1 && (d[p + 33] == 0 || d[p + 33] == 1))) {
            char name[256];
            DFile df;
            memcpy(name, d + p + 33, nl);
            name[nl] = '\0';
            df.n = 1;
            df.ext[0].pos = (uint64_t)rd32(d + p + 2) * SECTOR;
            df.ext[0].len = df.size = rd32(d + p + 10);
            if (fn(user, name, &df, (d[p + 25] & 2) != 0)) {
                free(d);
                return 1;
            }
        }
        p += len;
    }
    free(d);
    return 0;
}

/* UDF */
typedef struct {
    FILE *f;
    uint32_t part; /* the partition's first sector */
} Udf;

static int udf_tag(const unsigned char *b, int id) { return (int)rd16(b) == id; }

/* a file entry: its extents (or, for data inside it, the place of that data) */
static int udf_entry(const Udf *u, uint32_t lbn, DFile *df, unsigned char *inl, uint32_t *inlLen) {
    unsigned char b[SECTOR];
    uint32_t ea, ad, at, k;
    int efe;
    if (!read_at(u->f, (uint64_t)(u->part + lbn) * SECTOR, b, SECTOR)) {
        return 0;
    }
    if (!udf_tag(b, 261) && !udf_tag(b, 266)) {
        return 0;
    }
    efe = udf_tag(b, 266);
    df->size = rd64(b + 56);
    ea = rd32(b + (efe ? 208 : 168));
    ad = rd32(b + (efe ? 212 : 172));
    at = (efe ? 216 : 176) + ea;
    if (at + ad > SECTOR) {
        return 0;
    }
    df->n = 0;
    switch (rd16(b + 34) & 7) {
    case 0: /* short */
        for (k = 0; k + 8 <= ad && df->n < 64; k += 8) {
            uint32_t len = rd32(b + at + k) & 0x3FFFFFFF;
            if (len == 0) {
                break;
            }
            df->ext[df->n].pos = (uint64_t)(u->part + rd32(b + at + k + 4)) * SECTOR;
            df->ext[df->n++].len = len;
        }
        return 1;
    case 1: /* long */
        for (k = 0; k + 16 <= ad && df->n < 64; k += 16) {
            uint32_t len = rd32(b + at + k) & 0x3FFFFFFF;
            if (len == 0) {
                break;
            }
            df->ext[df->n].pos = (uint64_t)(u->part + rd32(b + at + k + 4)) * SECTOR;
            df->ext[df->n++].len = len;
        }
        return 1;
    case 3: /* the data is in the entry */
        if (inl == NULL || ad > SECTOR) {
            return 0;
        }
        memcpy(inl, b + at, ad);
        *inlLen = ad;
        df->n = -1;
        return 1;
    }
    return 0;
}

static int udf_dir(const Udf *u, uint32_t lbn, FoundFn fn, void *user) {
    unsigned char inl[SECTOR], *d;
    uint32_t inlLen = 0, p = 0;
    DFile dir;
    int r = 0;
    if (!udf_entry(u, lbn, &dir, inl, &inlLen)) {
        return 0;
    }
    if (dir.n == -1) {
        d = malloc(inlLen ? inlLen : 1);
        if (d == NULL) {
            return 0;
        }
        memcpy(d, inl, inlLen);
        dir.size = inlLen;
    } else {
        if (dir.size > (16u << 20) || (d = malloc((size_t)dir.size + 1)) == NULL || !dfile_read(u->f, &dir, 0, d, (size_t)dir.size)) {
            if (dir.size <= (16u << 20)) {
                free(d);
            }
            return 0;
        }
    }
    while (p + 38 <= dir.size && !r) {
        unsigned lfi, liu, total;
        if (!udf_tag(d + p, 257)) {
            break;
        }
        lfi = d[p + 19];
        liu = rd16(d + p + 36);
        total = (38 + liu + lfi + 3) & ~3u;
        if (p + 38 + liu + lfi > dir.size) {
            break;
        }
        if (!(d[p + 18] & 8) && lfi > 1) { /* (not the parent) */
            const unsigned char *nm = d + p + 38 + liu;
            char name[256];
            unsigned k, o = 0;
            if (nm[0] == 16) {
                for (k = 1; k + 1 < lfi && o < 255; k += 2) {
                    unsigned c = (unsigned)nm[k] << 8 | nm[k + 1];
                    name[o++] = c < 128 ? (char)c : '?';
                }
            } else {
                for (k = 1; k < lfi && o < 255; k++) {
                    name[o++] = (char)nm[k];
                }
            }
            name[o] = '\0';
            uint32_t child = rd32(d + p + 24);
            DFile df;
            int isDir = (d[p + 18] & 2) != 0;
            if (isDir) {
                df.n = 0;
                df.size = child; /* (a directory: its entry's place, for udf_dir) */
                r = fn(user, name, &df, 1);
            } else if (udf_entry(u, child, &df, NULL, NULL)) {
                r = fn(user, name, &df, 0);
            }
        }
        p += total;
    }
    free(d);
    return r;
}

typedef struct {
    DFile afs[2];
    int found[2];
    uint32_t dataDir; /* UDF: the DATA directory's entry; ISO: its sector */
    uint32_t dataSize;
    int haveData;
    char seen[64]; /* the first PZS*1.AFS name seen, for the message when it is another edition's */
} Look;

static int look_data_files(void *user, const char *name, const DFile *df, int isDir) {
    Look *l = user;
    static const char *const kNames[2][2] = {{"PZS3US1.AFS", "PZS4US1.AFS"}, {"PZS3US2.AFS", "PZS4US2.AFS"}};
    int a, k;
    if (isDir) {
        return 0;
    }
    for (a = 0; a < 2; a++) {
        for (k = 0; k < 2; k++) {
            if (name_is(name, kNames[a][k])) {
                l->afs[a] = *df;
                l->found[a] = 1;
            }
        }
    }
    if (l->seen[0] == '\0' && (name[0] == 'P' || name[0] == 'p') && strstr(name, "1.AFS") != NULL) {
        snprintf(l->seen, sizeof(l->seen), "%s", name);
    }
    return l->found[0] && l->found[1];
}

static int look_root(void *user, const char *name, const DFile *df, int isDir) {
    Look *l = user;
    if (isDir && name_is(name, "DATA")) {
        l->haveData = 1;
        l->dataDir = (uint32_t)(df->n == 0 && df->ext[0].len == 0 ? df->size : df->ext[0].pos / SECTOR);
        l->dataSize = (uint32_t)df->size;
        return 1;
    }
    return 0;
}

static int look_root_iso(void *user, const char *name, const DFile *df, int isDir) {
    Look *l = user;
    if (isDir && name_is(name, "DATA")) {
        l->haveData = 1;
        l->dataDir = (uint32_t)(df->ext[0].pos / SECTOR);
        l->dataSize = (uint32_t)df->size;
        return 1;
    }
    return 0;
}

/* finds the two archives, through UDF and then ISO 9660 */
static int find_archives(FILE *f, Look *l) {
    unsigned char b[SECTOR];
    /* UDF */
    if (read_at(f, 256ull * SECTOR, b, SECTOR) && udf_tag(b, 2)) {
        uint32_t vlen = rd32(b + 16), vloc = rd32(b + 20), s;
        Udf u = {f, 0};
        uint32_t fsd = 0;
        int havePart = 0, haveFsd = 0;
        for (s = 0; s < vlen / SECTOR && s < 64; s++) {
            if (!read_at(f, (uint64_t)(vloc + s) * SECTOR, b, SECTOR)) {
                break;
            }
            if (udf_tag(b, 5)) {
                u.part = rd32(b + 188);
                havePart = 1;
            } else if (udf_tag(b, 6)) {
                fsd = rd32(b + 252);
                haveFsd = 1;
            } else if (udf_tag(b, 8)) {
                break;
            }
        }
        if (havePart && haveFsd && read_at(f, (uint64_t)(u.part + fsd) * SECTOR, b, SECTOR) && udf_tag(b, 256)) {
            uint32_t root = rd32(b + 404);
            memset(l, 0, sizeof(*l));
            /* (udf_dir hands a directory over as df.size = the place of its entry) */
            if (udf_dir(&u, root, look_root, l) && l->haveData) {
                l->found[0] = l->found[1] = 0;
                if (udf_dir(&u, l->dataDir, look_data_files, l)) {
                    return 1;
                }
            }
        }
    }
    /* ISO 9660 */
    if (read_at(f, 16ull * SECTOR, b, SECTOR) && b[0] == 1 && memcmp(b + 1, "CD001", 5) == 0) {
        char seen[64];
        snprintf(seen, sizeof(seen), "%s", l->seen);
        memset(l, 0, sizeof(*l));
        snprintf(l->seen, sizeof(l->seen), "%s", seen);
        if (iso_dir(f, rd32(b + 156 + 2), rd32(b + 156 + 10), look_root_iso, l) && l->haveData) {
            if (iso_dir(f, l->dataDir, l->dataSize, look_data_files, l)) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the archives ---- */

typedef struct {
    DFile df;
    uint32_t count;
    unsigned char *table; /* count * 8 */
} Afs;

struct DiDisc {
    FILE *f;
    Afs a[2];
    DiItem items[DI_CHARAS + DI_STAGES + DI_SONGS];
    int count;
    unsigned char *params[DI_CHARAS]; /* section 18 of each character's model, 0xC6 bytes; NULL: none */
    int charaAt[DI_CHARAS];           /* item of slot c, -1 */
    unsigned char *stageNames, *songNames; /* the name sheets' sections */
    char disc[96]; /* the image's file name, without its folder and extension */
    long stageNamesLen, songNamesLen;
};

static int afs_open(FILE *f, const DFile *df, Afs *a) {
    unsigned char h[8];
    if (!dfile_read(f, df, 0, h, 8) || memcmp(h, "AFS", 3) != 0) {
        return 0;
    }
    a->count = rd32(h + 4);
    if (a->count == 0 || a->count > 200000 || (a->table = malloc((size_t)a->count * 8)) == NULL) {
        return 0;
    }
    a->df = *df;
    if (!dfile_read(f, df, 8, a->table, (size_t)a->count * 8)) {
        return 0;
    }
    /* Some mods (Goku-Verse) zero every offset but the first. Files are packed back to back on 0x800: rebuild them. */
    for (uint32_t i = 1; i < a->count; i++) {
        unsigned char *e = a->table + i * 8;
        if (rd32(e) == 0) {
            uint32_t o = (uint32_t)((rd32(e - 8) + (uint64_t)rd32(e - 4) + 0x7FF) & ~(uint64_t)0x7FF);
            e[0] = (unsigned char)o, e[1] = (unsigned char)(o >> 8), e[2] = (unsigned char)(o >> 16), e[3] = (unsigned char)(o >> 24);
        }
    }
    return 1;
}

/* the size of game file id (0 if there is none) */
static uint32_t file_size(const DiDisc *d, int id) {
    const Afs *a = &d->a[id < 0xD48 ? 0 : 1];
    uint32_t i = (uint32_t)(id < 0xD48 ? id - 1 : id - 0xD48);
    if (id < 1 || i >= a->count) {
        return 0;
    }
    return rd32(a->table + i * 8 + 4);
}

/* game file id read whole (NULL if none or above max); *n its size */
static unsigned char *file_read(const DiDisc *d, int id, long max, long *n) {
    const Afs *a = &d->a[id < 0xD48 ? 0 : 1];
    uint32_t i = (uint32_t)(id < 0xD48 ? id - 1 : id - 0xD48), size;
    unsigned char *b;
    if (id < 1 || i >= a->count || (size = rd32(a->table + i * 8 + 4)) == 0 || (long)size > max || (b = malloc(size)) == NULL) {
        return NULL;
    }
    if (!dfile_read(d->f, &a->df, rd32(a->table + i * 8), b, size)) {
        free(b);
        return NULL;
    }
    *n = (long)size;
    return b;
}

/* ---- packs and pictures ---- */

static int section(const unsigned char *b, long n, unsigned i, long *at, long *len) {
    unsigned count;
    long a, z;
    if (n < 8 || (count = rd32(b)) == 0 || i < 1 || i > count || (long)(i + 1) * 4 + 4 > n) {
        return 0;
    }
    a = (long)(rd32(b + 4 * i) & ~3u);
    z = i < count ? (long)(rd32(b + 4 * (i + 1)) & ~3u) : n;
    if (z > n) {
        z = n; /* (some models end before their table says) */
    }
    if (a < 0 || a > n || z < a) {
        return 0;
    }
    *at = a;
    *len = z - a;
    return 1;
}

/* UTF-16 (a line of the menu's name lists) to UTF-8 */
static void utf8_of(const unsigned char *s, long n, char *out, int size) {
    long k = 0;
    int o = 0;
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) {
        k = 2;
    }
    for (; k + 1 < n; k += 2) {
        unsigned c = (unsigned)s[k] | (unsigned)s[k + 1] << 8;
        if (c == 0) {
            break;
        }
        if (c < 0x80 && o + 1 < size) {
            out[o++] = (char)c;
        } else if (c < 0x800 && o + 2 < size) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 63));
        } else if (c >= 0x800 && o + 3 < size) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 63));
            out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
    }
}

static int pic_place(int x, int y, int w) {
    int block = (y & ~0xF) * w + (x & ~0xF) * 2, swap = ((y + 2) >> 2 & 1) * 4, row = (((y & ~3) >> 1) + (y & 1)) & 7;
    return block + row * w * 2 + ((x + swap) & 7) * 4 + (y >> 1 & 1) + (x >> 2 & 2);
}

unsigned char *Di_Picture(const unsigned char *b, int n, int *w, int *h) {
    long rec, pix, clut;
    uint64_t tex0;
    int x, y;
    unsigned char *o;
    if (n < 0x60 || rd32(b) != 1) {
        return NULL;
    }
    rec = (long)rd32(b + 4) * 4;
    if (rec < 0 || rec + 0x38 > n) {
        return NULL;
    }
    pix = (long)rd32(b + rec) * 4 + 0x60;
    clut = (long)rd32(b + rec + 4) * 4 + 0x60;
    tex0 = rd64(b + rec + 0x30);
    *w = 1 << (int)(tex0 >> 26 & 15);
    *h = 1 << (int)(tex0 >> 30 & 15);
    if ((tex0 >> 20 & 0x3F) != 0x13 || *w > 512 || *h > 512 || pix + (long)*w * *h > n || clut + 1024 > n || (o = malloc((size_t)*w * *h * 4)) == NULL) {
        return NULL;
    }
    for (y = 0; y < *h; y++) {
        for (x = 0; x < *w; x++) {
            long q = pix + pic_place(x, y, *w);
            int i = b[q], s = (i & 0x18) == 8 || (i & 0x18) == 16 ? i ^ 0x18 : i;
            const unsigned char *c = b + clut + s * 4;
            unsigned char *t = o + ((size_t)y * *w + x) * 4;
            int a = c[3] * 2 - (c[3] >> 7);
            t[0] = c[0];
            t[1] = c[1];
            t[2] = c[2];
            t[3] = (unsigned char)(a > 255 ? 255 : a);
        }
    }
    return o;
}

static const int kB32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const int kC32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                               {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const int kC8[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                               {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};

/* row n of the select's name sheets (512 x 256, 8 bits, stored as the console's memory holds them) as RGBA */
static unsigned char *name_row(const unsigned char *sec, long n, int row, int perSheet, int height) {
    long ent, pix, clut;
    int sheet = row / perSheet, x, y, y0 = (row % perSheet) * height;
    unsigned char *mem, *o;
    if (sec == NULL || n < 8) {
        return NULL;
    }
    ent = (long)rd32(sec + 4) * 4 + (long)sheet * 0x40;
    if (ent + 16 > n) {
        return NULL;
    }
    pix = (long)rd32(sec + ent) * 4 + 0x60;
    clut = (long)rd32(sec + ent + 4) * 4 + 0x60;
    if ((long)rd32(sec + ent + 8) != 0x60 + 512 * 256 + 0x20 || pix + 512L * 256 > n || clut + 1024 > n) {
        return NULL;
    }
    mem = malloc(512 * 256);
    o = malloc((size_t)512 * height * 4);
    if (mem == NULL || o == NULL) {
        free(mem);
        free(o);
        return NULL;
    }
    for (y = 0; y < 128; y++) {
        for (x = 0; x < 256; x++) {
            int a = (((y & ~0x1F) * 4 + ((x >> 1) & ~0x1F) + kB32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kC32[y & 7][x & 7];
            memcpy(mem + a * 4, sec + pix + ((long)y * 256 + x) * 4, 4);
        }
    }
    for (y = y0; y < y0 + height; y++) {
        int col = (y & 15) >> 2;
        for (x = 0; x < 512; x++) {
            int sx = (col & 1) ? ((x & 15) ^ 4) : (x & 15);
            int i = mem[((((y >> 1) & ~0x1F) * 4 + ((x >> 2) & ~0x1F) + kB32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + kC8[y & 3][sx] + col * 64];
            i = (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1);
            const unsigned char *c = sec + clut + i * 4;
            unsigned char *t = o + ((size_t)(y - y0) * 512 + x) * 4;
            int a = c[3] * 2;
            t[0] = c[0];
            t[1] = c[1];
            t[2] = c[2];
            t[3] = (unsigned char)(a > 255 ? 255 : a);
        }
    }
    free(mem);
    return o;
}

/* ---- opening a disc ---- */

static const char *const kStages[DI_STAGES] = {
    "Wasteland - Noon", "Rocky Area - Noon", "Planet Namek", "Dying Namek", "World Tournament Stage - Noon", "Kami's Lookout",
    "Cell Games Arena - Evening", "Supreme Kai's World", "Hyperbolic Time Chamber", "City Ruins - Noon", "Mountain Road - Noon", "Islands",
    "Kame House", "Planet - Night", "Glacier", "Ruined Earth", "Outer Space", "Penguin Village", "Hell", "Desert - Noon", "King's Castle",
    "Muscle Tower", "Mount Paozu", "Wasteland - Evening", "Wasteland - Night", "Rocky Area - Evening", "Rocky Area - Night",
    "World Tournament Stage - Evening", "Cell Game - Noon", "City Ruins - Evening", "City Ruins - Night", "Desert - Evening", "Desert - Night",
    "Mountain Road - Evening", "Planet - Evening"};
static const char *const kSongs[20] = {"The Meteor", "Vital Burner", "Innocent World", "After The Fire", "Sweet Vibration", "Survive", "Heat Capacity",
                                       "Overture", "Shine", "Power Scale", "Edge of Spirit", "Caution", "Menace", "Hot Soul", "High and Scream",
                                       "Shootout In Meteor", "Dynamite Battle", "Burnin' Up", "Wild Rush", "Evolution"};

#define MODEL_ID(c) (0x590 + (c) * 10)
#define STAGE_ID(s) (0x171 + (s))
#define THUMB_ID(s) (0x39D + (s))
#define SONG_ID(s) (0x10B16 + (s))

/* whether game file id is the same as the player's own copy of it in gamedata (1), not (0), not known (-1) */
static int same_as_stock(const DiDisc *d, int id, const char *gamedata) {
    char path[600];
    FILE *f;
    long n = 0, size = (long)file_size(d, id), got;
    unsigned char *a, *b;
    int same;
    if (gamedata == NULL || gamedata[0] == '\0') {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s/%05d.bin", gamedata, id < 0xD48 ? "pzs3us1" : "pzs3us2", id < 0xD48 ? id - 1 : id - 0xD48);
    if ((f = fopen(path, "rb")) == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    got = ftell(f);
    if (got != size) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);
    a = malloc((size_t)size + 1);
    if (a == NULL || fread(a, 1, (size_t)size, f) != (size_t)size) {
        free(a);
        fclose(f);
        return -1;
    }
    fclose(f);
    b = file_read(d, id, 64L << 20, &n);
    same = b != NULL && n == size && memcmp(a, b, (size_t)size) == 0;
    free(a);
    free(b);
    return same;
}

/* the menu archive's name lists, label lists and chip pack (sections 43, 44, 45 of its second screen) */
typedef struct {
    unsigned char *res;
    long resLen;
    long at[3], len[3];
} MenuPack;

static int menu_open(const DiDisc *d, MenuPack *m) {
    long n, at, len, rl;
    unsigned char *arc = file_read(d, 0x1C1, 16L << 20, &n);
    int k;
    memset(m, 0, sizeof(*m));
    if (arc == NULL || !section(arc, n, 2, &at, &len) || (m->res = Port_BpeUnpack(arc + at, len, 32L << 20, &rl)) == NULL) {
        free(arc);
        return 0;
    }
    free(arc);
    m->resLen = rl;
    for (k = 0; k < 3; k++) {
        if (!section(m->res, rl, 43 + k, &m->at[k], &m->len[k])) {
            return 0;
        }
    }
    return 1;
}

/* entry e (from 1) of list k of the menu pack */
static int menu_entry(const MenuPack *m, int k, int e, const unsigned char **p, long *len) {
    long at, l;
    if (!section(m->res + m->at[k], m->len[k], (unsigned)e, &at, &l)) {
        return 0;
    }
    *p = m->res + m->at[k] + at;
    *len = l;
    return 1;
}

DiDisc *Di_Open(const char *iso, const char *gamedata, char *why, int whyLen, volatile int *progress) {
    DiDisc *d = calloc(1, sizeof(DiDisc));
    Look l;
    MenuPack m, stock;
    char stockPath[600];
    int c, s, done = 0, total = DI_CHARAS + DI_STAGES + DI_SONGS;
    long at, len;

    if (d == NULL) {
        return NULL;
    }
    {
        const char *b = strrchr(iso, '/'), *e;
        int k;
        if (strrchr(iso, '\\') > b) {
            b = strrchr(iso, '\\');
        }
        b = b ? b + 1 : iso;
        e = strrchr(b, '.');
        for (k = 0; k < 90 && b + k != e && b[k] != '\0'; k++) {
            d->disc[k] = (unsigned char)b[k] < 32 ? ' ' : b[k];
        }
        d->disc[k] = '\0';
    }
    if ((d->f = fopen(iso, "rb")) == NULL) {
        snprintf(why, whyLen, "The disc image cannot be opened.");
        free(d);
        return NULL;
    }
    memset(&l, 0, sizeof(l));
    if (!find_archives(d->f, &l)) {
        if (l.seen[0] != '\0') {
            snprintf(why, whyLen, "This disc's data is %s: only USA-based discs are supported (other editions number their files differently).", l.seen);
        } else {
            snprintf(why, whyLen, "No game archives on this disc.");
        }
        Di_Close(d);
        return NULL;
    }
    if (!afs_open(d->f, &l.afs[0], &d->a[0]) || !afs_open(d->f, &l.afs[1], &d->a[1])) {
        snprintf(why, whyLen, "This disc's archives are scrambled or in a custom format, so they cannot be read.");
        Di_Close(d);
        return NULL;
    }
    if (!menu_open(d, &m)) {
        snprintf(why, whyLen, "This disc's character select could not be read.");
        Di_Close(d);
        return NULL;
    }
    /* the player's own names, to tell which slots are different */
    memset(&stock, 0, sizeof(stock));
    snprintf(stockPath, sizeof(stockPath), "%s/pzs3us1/00448.bin", gamedata != NULL ? gamedata : "");
    {
        FILE *sf = fopen(stockPath, "rb");
        if (sf != NULL) {
            long sn;
            unsigned char *sb;
            fseek(sf, 0, SEEK_END);
            sn = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            if (sn > 0 && sn < (16L << 20) && (sb = malloc((size_t)sn)) != NULL) {
                if (fread(sb, 1, (size_t)sn, sf) == (size_t)sn && section(sb, sn, 2, &at, &len)) {
                    long rl;
                    if ((stock.res = Port_BpeUnpack(sb + at, len, 32L << 20, &rl)) != NULL) {
                        stock.resLen = rl;
                        if (!section(stock.res, rl, 43, &stock.at[0], &stock.len[0])) {
                            free(stock.res);
                            stock.res = NULL;
                        }
                    }
                }
                free(sb);
            }
            fclose(sf);
        }
    }

    for (c = 0; c < DI_CHARAS; c++) {
        DiItem *it;
        const unsigned char *p;
        long n;
        unsigned char *model;
        d->charaAt[c] = -1;
        if (progress != NULL) {
            *progress = done++ * 1000 / total;
        }
        if (file_size(d, MODEL_ID(c)) <= 4096) {
            continue;
        }
        it = &d->items[d->count];
        d->charaAt[c] = d->count++;
        it->kind = DI_CHARA;
        it->index = c;
        if (menu_entry(&m, 0, c + 1, &p, &n)) {
            utf8_of(p, n, it->name, sizeof(it->name));
        }
        if (menu_entry(&m, 1, c + 1, &p, &n)) {
            utf8_of(p, n, it->form, sizeof(it->form));
        }
        if (it->name[0] == '\0') {
            snprintf(it->name, sizeof(it->name), "Slot %d", c);
        }
        if (menu_entry(&m, 2, c + 1, &p, &n) && n > 0 && n <= CHIP_MAX) {
            memcpy(it->chip, p, (size_t)n);
            it->chipLen = (int)n;
        }
        it->changed = -1;
        if (stock.res != NULL && menu_entry(&stock, 0, c + 1, &p, &n)) {
            char own[96];
            utf8_of(p, n, own, sizeof(own));
            it->changed = strcmp(own, it->name) != 0;
        }
        if ((model = file_read(d, MODEL_ID(c), MODEL_MAX * 2, &n)) != NULL) {
            if (section(model, n, 18, &at, &len) && len >= 0xC6 && (d->params[c] = malloc(0xC6)) != NULL) {
                memcpy(d->params[c], model + at, 0xC6);
            }
            free(model);
        }
    }
    for (c = 0; c < DI_CHARAS; c++) {
        const unsigned char *p = d->params[c];
        int k;
        if (p == NULL || d->charaAt[c] < 0) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            int t = p[0x98 + k];
            if (t != 0xFF && t != c && t < DI_CHARAS && d->charaAt[t] >= 0) {
                d->items[d->charaAt[c]].hasForms = 1;
                if (k < 3) {
                    d->items[d->charaAt[t]].formOf = 1;
                }
            }
        }
        for (k = 0; k < 3; k++) {
            int t = p[0xB4 + k];
            if (t != 0xFF && t < DI_CHARAS && d->charaAt[t] >= 0) {
                d->items[d->charaAt[t]].fusion = 1;
            }
        }
    }
    /* stages and songs, with their name sheets and the stages' icons */
    {
        long ib, il;
        if (section(m.res, m.resLen, 6, &ib, &il) && (d->stageNames = malloc((size_t)il)) != NULL) {
            memcpy(d->stageNames, m.res + ib, (size_t)il);
            d->stageNamesLen = il;
        }
        if (section(m.res, m.resLen, 4, &ib, &il) && (d->songNames = malloc((size_t)il)) != NULL) {
            memcpy(d->songNames, m.res + ib, (size_t)il);
            d->songNamesLen = il;
        }
    }
    for (s = 0; s < DI_STAGES; s++) {
        DiItem *it;
        long ia, il, ea, el;
        if (progress != NULL) {
            *progress = done++ * 1000 / total;
        }
        if (file_size(d, STAGE_ID(s)) < 4096) {
            continue;
        }
        it = &d->items[d->count++];
        it->kind = DI_STAGE;
        it->index = s;
        it->changed = same_as_stock(d, STAGE_ID(s), gamedata);
        it->changed = it->changed < 0 ? -1 : !it->changed;
        if (it->changed == 0) {
            snprintf(it->name, sizeof(it->name), "%s", kStages[s]);
        } else {
            snprintf(it->name, sizeof(it->name), "Stage %d", s + 1);
        }
        if (section(m.res, m.resLen, 46, &ia, &il) && section(m.res + ia, il, (unsigned)(s + 1), &ea, &el) && el <= CHIP_MAX) {
            memcpy(it->chip, m.res + ia + ea, (size_t)el);
            it->chipLen = (int)el;
        }
    }
    for (s = 0; s < DI_SONGS; s++) {
        DiItem *it;
        if (progress != NULL) {
            *progress = done++ * 1000 / total;
        }
        if (file_size(d, SONG_ID(s)) < 4096) {
            continue;
        }
        it = &d->items[d->count++];
        it->kind = DI_SONG;
        it->index = s;
        it->changed = same_as_stock(d, SONG_ID(s), gamedata);
        it->changed = it->changed < 0 ? -1 : !it->changed;
        if (it->changed == 0 && s < 20) {
            snprintf(it->name, sizeof(it->name), "%s", kSongs[s]);
        } else {
            snprintf(it->name, sizeof(it->name), "Song %d", s + 1);
        }
    }
    free(m.res);
    free(stock.res);
    if (progress != NULL) {
        *progress = 1000;
    }
    return d;
}

void Di_Close(DiDisc *d) {
    int c;
    if (d == NULL) {
        return;
    }
    if (d->f != NULL) {
        fclose(d->f);
    }
    free(d->a[0].table);
    free(d->a[1].table);
    for (c = 0; c < DI_CHARAS; c++) {
        free(d->params[c]);
    }
    free(d->stageNames);
    free(d->songNames);
    free(d);
}

int Di_Count(const DiDisc *d) { return d != NULL ? d->count : 0; }
const DiItem *Di_Item(const DiDisc *d, int i) { return d != NULL && i >= 0 && i < d->count ? &d->items[i] : NULL; }

unsigned char *Di_NamePicture(const DiDisc *d, int i, int *w, int *h) {
    const DiItem *it = Di_Item(d, i);
    if (it == NULL || it->kind == DI_CHARA) {
        return NULL;
    }
    *w = 512;
    *h = it->kind == DI_STAGE ? 64 : 32;
    return it->kind == DI_STAGE ? name_row(d->stageNames, d->stageNamesLen, it->index, 4, 64) : name_row(d->songNames, d->songNamesLen, it->index, 8, 32);
}

/* The picture of a name as chips of the roster's kind (64 x 64 textures, tile 0..7 across), for screens that draw such files:
   a copy of a character's chip with the sheet's pixels and colours in it. */
int Di_NameChip(const DiDisc *d, int i, int tile, unsigned char *out, int max) {
    const DiItem *it = Di_Item(d, i), *model = NULL;
    const unsigned char *sec;
    long n, ent, pix, clut, rec, cp, cc;
    int k, x, y, row, perSheet, height, sheet, y0, w, h;
    unsigned char *mem;
    if (it == NULL || it->kind == DI_CHARA || tile < 0 || tile > 7) {
        return 0;
    }
    for (k = 0; k < d->count && model == NULL; k++) {
        if (d->items[k].chipLen >= 0x1160 && d->items[k].kind == DI_CHARA) {
            model = &d->items[k];
        }
    }
    if (model == NULL || model->chipLen > max) {
        return 0;
    }
    sec = it->kind == DI_STAGE ? d->stageNames : d->songNames;
    n = it->kind == DI_STAGE ? d->stageNamesLen : d->songNamesLen;
    perSheet = it->kind == DI_STAGE ? 4 : 8;
    height = it->kind == DI_STAGE ? 64 : 32;
    row = it->index;
    sheet = row / perSheet;
    y0 = (row % perSheet) * height;
    if (sec == NULL || n < 8) {
        return 0;
    }
    ent = (long)rd32(sec + 4) * 4 + (long)sheet * 0x40;
    if (ent + 16 > n) {
        return 0;
    }
    pix = (long)rd32(sec + ent) * 4 + 0x60;
    clut = (long)rd32(sec + ent + 4) * 4 + 0x60;
    if (pix + 512L * 256 > n || clut + 1024 > n || (mem = malloc(512 * 256)) == NULL) {
        return 0;
    }
    for (y = 0; y < 128; y++) {
        for (x = 0; x < 256; x++) {
            int a = (((y & ~0x1F) * 4 + ((x >> 1) & ~0x1F) + kB32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kC32[y & 7][x & 7];
            memcpy(mem + a * 4, sec + pix + ((long)y * 256 + x) * 4, 4);
        }
    }
    memcpy(out, model->chip, (size_t)model->chipLen);
    rec = (long)rd32(out + 4) * 4;
    cp = (long)rd32(out + rec) * 4 + 0x60;
    cc = (long)rd32(out + rec + 4) * 4 + 0x60;
    w = h = 64;
    memset(out + cp, 0, (size_t)w * h);
    for (y = 0; y < height; y++) {
        int yy = y0 + y, col = (yy & 15) >> 2;
        for (x = 0; x < w; x++) {
            int xx = tile * 64 + x, sx = (col & 1) ? ((xx & 15) ^ 4) : (xx & 15);
            out[cp + pic_place(x, y, w)] = mem[((((yy >> 1) & ~0x1F) * 4 + ((xx >> 2) & ~0x1F) + kB32[(yy >> 4) & 3][(xx >> 4) & 7]) << 8) + kC8[yy & 3][sx] + col * 64];
        }
    }
    memcpy(out + cc, sec + clut, 1024);
    free(mem);
    return model->chipLen;
}

/* ---- picks ---- */

void Di_Closure(const DiDisc *d, DiPicks *p) {
    int changed = 1, c, k;
    while (changed) {
        changed = 0;
        for (c = 0; c < DI_CHARAS; c++) {
            const unsigned char *t = d->params[c];
            int at = d->charaAt[c];
            if (at < 0 || !p->take[at] || t == NULL) {
                continue;
            }
            int want[16], n = 0;
            if (p->linkForms) {
                for (k = 0; k < 4; k++) {
                    want[n++] = t[0x98 + k];
                }
            }
            if (p->linkFusions) {
                for (k = 0; k < 3; k++) {
                    if (t[0xB4 + k] != 0xFF) {
                        int m;
                        want[n++] = t[0xB4 + k];
                        for (m = 0; m < 4 && n < 16; m++) {
                            want[n++] = t[0xBA + k * 4 + m];
                        }
                    }
                }
            }
            for (k = 0; k < n; k++) {
                if (want[k] < DI_CHARAS && d->charaAt[want[k]] >= 0 && !p->take[d->charaAt[want[k]]]) {
                    p->take[d->charaAt[want[k]]] = 1;
                    changed = 1;
                }
            }
        }
    }
}

/* ---- writing ---- */

static const char kPlain[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

/* A name as a folder's or a file's: plain letters (accents taken off), no character a link or a file system trips on. */
static void clean_name(const char *in, char *out, int size) {
    int o = 0, space = 1;
    const unsigned char *s = (const unsigned char *)in;
    while (*s != '\0' && o + 1 < size && o < 56) {
        unsigned c = *s++;
        if (c >= 0xC0 && (*s & 0xC0) == 0x80) { /* a two-byte UTF-8 letter */
            c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
            c = c >= 0xC0 && c <= 0xFF ? (unsigned char)kPlain[c - 0xC0] : ' ';
            if (c == '/' || c == 'x') {
                c = c == 'x' ? 'x' : 'o';
            }
        } else if (c >= 0x80) {
            while ((*s & 0xC0) == 0x80) {
                s++;
            }
            c = ' ';
        }
        if (!(isalnum((int)c) || strchr(" -_'()+!&", (int)c) != NULL)) {
            c = ' ';
        }
        if (c == ' ') {
            if (space) {
                continue;
            }
            space = 1;
        } else {
            space = 0;
        }
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        o--;
    }
    out[o] = '\0';
}

typedef struct {
    char (*names)[128];
    int n, cap;
} Taken;

static int taken_has(const Taken *t, const char *n) {
    int i;
    for (i = 0; i < t->n; i++) {
        if (strcasecmp(t->names[i], n) == 0) {
            return 1;
        }
    }
    return 0;
}

static void taken_add(Taken *t, const char *n) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->names = realloc(t->names, (size_t)t->cap * 128);
    }
    snprintf(t->names[t->n++], 128, "%s", n);
}

static void unique(Taken *t, const char *base, char *out, int size) {
    int i;
    snprintf(out, size, "%s", base);
    for (i = 2; taken_has(t, out); i++) {
        snprintf(out, size, "%s (%d)", base, i);
    }
    taken_add(t, out);
}

static int make_dirs(const char *path) {
    char p[700];
    size_t i;
    snprintf(p, sizeof(p), "%s", path);
    for (i = 1; p[i] != '\0'; i++) {
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = '\0';
            MAKE_DIR(p);
            p[i] = c;
        }
    }
    MAKE_DIR(p);
    return 1;
}

static int write_file(const char *path, const void *b, long n) {
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL) {
        return 0;
    }
    ok = fwrite(b, 1, (size_t)n, f) == (size_t)n;
    return fclose(f) == 0 && ok;
}

static void note(char *notes, int len, const char *fmt, const char *a) {
    size_t at = strlen(notes);
    if ((int)at + 2 < len) {
        snprintf(notes + at, (size_t)len - at, fmt, a);
    }
}

/* a value cut at a whole UTF-8 character to at most `bytes` */
static void fit(const char *in, char *out, int bytes) {
    int n = 0;
    while (in[n] != '\0' && n < bytes) {
        n++;
    }
    while (n > 0 && in[n] != '\0' && ((unsigned char)in[n] & 0xC0) == 0x80) {
        n--;
    }
    memcpy(out, in, (size_t)n);
    out[n] = '\0';
    for (int i = 0; i < n; i++) {
        if ((unsigned char)out[i] < 32) {
            out[i] = ' ';
        }
    }
}

/* the files of character c into dir */
static int write_chara(DiDisc *d, int c, const char *dir, const char *manifest, char *notes, int notesLen) {
    static const struct {
        int add;
        const char *name;
        long max;
    } kParts[] = {{8, "m_anm.pak", 1443840}, {9, "m_eff.pak", 845824}};
    char path[700];
    unsigned char *b;
    long n;
    int k, b0 = MODEL_ID(c), line;
    make_dirs(dir);
    for (k = 0; k < 4; k++) {
        if (file_size(d, b0 + k) <= 4096) {
            break;
        }
        if ((b = file_read(d, b0 + k, MODEL_MAX * 2, &n)) == NULL) {
            continue;
        }
        if ((n + 2047) / 2048 * 2048 <= MODEL_MAX) {
            snprintf(path, sizeof(path), "%s/m_%dp.pak", dir, k + 1);
            write_file(path, b, n);
        } else {
            note(notes, notesLen, "%s: a costume is larger than the game takes. ", d->items[d->charaAt[c]].name);
        }
        free(b);
        if (file_size(d, b0 + 4 + k) > 4096 && (b = file_read(d, b0 + 4 + k, MODEL_MAX * 2, &n)) != NULL) {
            if ((n + 2047) / 2048 * 2048 <= MODEL_MAX) {
                snprintf(path, sizeof(path), "%s/m_%dp_dmg.pak", dir, k + 1);
                write_file(path, b, n);
            }
            free(b);
        }
    }
    for (k = 0; k < 2; k++) {
        if ((b = file_read(d, b0 + kParts[k].add, 4L << 20, &n)) != NULL) {
            if (n >= 16 && (n + 2047) / 2048 * 2048 <= kParts[k].max) {
                snprintf(path, sizeof(path), "%s/%s", dir, kParts[k].name);
                write_file(path, b, n);
            }
            free(b);
        }
    }
    {
        static const struct {
            int first;
            const char *name;
            long max;
        } kOne[] = {{0xC7B, "m_voice_us.pak", 591872}, {0xBDA, "m_voice_jp.pak", 591872}, {0x2F9, "portrait.bin", 92160}, {8, "member_1p.bin", 4096}, {9, "member_2p.bin", 4096}};
        for (k = 0; k < 5; k++) {
            int id = k >= 3 ? kOne[k].first + c * 2 : kOne[k].first + c;
            if ((b = file_read(d, id, 4L << 20, &n)) == NULL) {
                continue;
            }
            if (k == 2) {
                while (n > 0 && b[n - 1] == 0) {
                    n--;
                }
            }
            if (n >= 16 && (n + 2047) / 2048 * 2048 <= kOne[k].max) {
                snprintf(path, sizeof(path), "%s/%s", dir, kOne[k].name);
                write_file(path, b, n);
            }
            free(b);
        }
    }
    if (d->items[d->charaAt[c]].chipLen > 0) {
        snprintf(path, sizeof(path), "%s/chip.bin", dir);
        write_file(path, d->items[d->charaAt[c]].chip, d->items[d->charaAt[c]].chipLen);
    }
    for (k = 0; k < 2; k++) {
        snprintf(path, sizeof(path), "%s/voice_%s", dir, k == 0 ? "us" : "jp");
        make_dirs(path);
        for (line = 0; line < 100; line++) {
            int id = (k == 0 ? 0xCC32 : 0x8D4E) + c * 100 + line;
            long z;
            if ((b = file_read(d, id, 16L << 20, &n)) == NULL) {
                continue;
            }
            for (z = 0; z < n && b[z] == 0; z++) {
            }
            if (z < n) {
                snprintf(path, sizeof(path), "%s/voice_%s/%03d.adx", dir, k == 0 ? "us" : "jp", line);
                write_file(path, b, n);
            }
            free(b);
        }
    }
    snprintf(path, sizeof(path), "%s/character.txt", dir);
    return write_file(path, manifest, (long)strlen(manifest));
}

/* the folder of every taken character: a family (characters joined by a transformation) is one folder, its head the
   one no other turns into, the forms folders inside it named by their label */
typedef struct {
    char top[128];
    char leaf[128]; /* "" for the head */
} Place;

static void ref(const Place *pl, int from, int to, char *out, int size) {
    if (strcmp(pl[from].top, pl[to].top) == 0) {
        snprintf(out, size, "%s", pl[to].leaf[0] ? pl[to].leaf : "..");
    } else if (pl[to].leaf[0] == '\0') {
        snprintf(out, size, "%s", pl[to].top);
    } else {
        snprintf(out, size, "%s/%s", pl[to].top, pl[to].leaf);
    }
}

static int top_of(const int *family, int x) {
    while (family[x] != x) {
        x = family[x];
    }
    return x;
}

int Di_Import(DiDisc *d, const DiPicks *p, const char *characters, const char *stages, const char *songs, char *notes, int notesLen,
              volatile int *progress) {
    static Place pl[DI_CHARAS];
    int family[DI_CHARAS], chosen[DI_CHARAS], c, k, made = 0, total = 0, done = 0, link = p->linkForms || p->linkFusions;
    Taken taken = {0};
    char stage[640], path[700];
    DIR *dir;
    struct dirent *de;

    notes[0] = '\0';
    for (c = 0; c < DI_CHARAS; c++) {
        chosen[c] = d->charaAt[c] >= 0 && p->take[d->charaAt[c]];
        family[c] = c;
        total += chosen[c];
    }
    for (k = 0; k < d->count; k++) {
        total += d->items[k].kind != DI_CHARA && p->take[k];
    }
    if ((dir = opendir(characters)) != NULL) {
        while ((de = readdir(dir)) != NULL) {
            taken_add(&taken, de->d_name);
        }
        closedir(dir);
    }
    snprintf(stage, sizeof(stage), "%s/.import", characters);
    make_dirs(stage);

    /* families */
#define TOP(x) top_of(family, (x))
    for (c = 0; c < DI_CHARAS; c++) {
        if (!chosen[c] || !link || d->params[c] == NULL) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            int t = d->params[c][0x98 + k];
            if (t < DI_CHARAS && chosen[t]) {
                family[TOP(t)] = TOP(c);
            }
        }
    }
    for (c = 0; c < DI_CHARAS; c++) {
        int head = -1, m, top;
        if (!chosen[c] || TOP(c) != c) {
            continue;
        }
        top = c;
        /* the head: the lowest member no other member reaches by left, up or right */
        for (m = 0; m < DI_CHARAS && head < 0; m++) {
            int reached = 0, o;
            if (!chosen[m] || TOP(m) != top) {
                continue;
            }
            for (o = 0; o < DI_CHARAS && !reached; o++) {
                if (o != m && chosen[o] && TOP(o) == top && link && d->params[o] != NULL) {
                    for (k = 0; k < 3; k++) {
                        reached |= d->params[o][0x98 + k] == m;
                    }
                }
            }
            if (!reached) {
                head = m;
            }
        }
        if (head < 0) {
            head = top;
        }
        {
            const DiItem *h = &d->items[d->charaAt[head]];
            char title[200], clean[128];
            if (h->form[0] != '\0') {
                snprintf(title, sizeof(title), "%s - %s", h->name, h->form);
            } else {
                snprintf(title, sizeof(title), "%s", h->name);
            }
            clean_name(title, clean, sizeof(clean));
            if (clean[0] == '\0') {
                snprintf(clean, sizeof(clean), "Slot %d", head);
            }
            unique(&taken, clean, pl[head].top, sizeof(pl[head].top));
            pl[head].leaf[0] = '\0';
        }
        {
            Taken used = {0};
            taken_add(&used, "voice_us");
            taken_add(&used, "voice_jp");
            for (m = 0; m < DI_CHARAS; m++) {
                char leaf[128];
                if (m == head || !chosen[m] || TOP(m) != top) {
                    continue;
                }
                clean_name(d->items[d->charaAt[m]].form, leaf, sizeof(leaf));
                if (leaf[0] == '\0') {
                    clean_name(d->items[d->charaAt[m]].name, leaf, sizeof(leaf));
                }
                if (leaf[0] == '\0') {
                    snprintf(leaf, sizeof(leaf), "Form %d", m);
                }
                snprintf(pl[m].top, sizeof(pl[m].top), "%s", pl[head].top);
                unique(&used, leaf, pl[m].leaf, sizeof(pl[m].leaf));
            }
            free(used.names);
        }
    }

    /* each character's folder and manifest */
    for (c = 0; c < DI_CHARAS; c++) {
        static const char *const kKeys[4] = {"left", "up", "right", "down"};
        char man[4096], v[256], name[96], form[128], about[220];
        const DiItem *it;
        const unsigned char *t = link ? d->params[c] : NULL;
        if (!chosen[c]) {
            continue;
        }
        it = &d->items[d->charaAt[c]];
        fit(it->name, name, 63);
        fit(it->form, form, 95);
        snprintf(about, sizeof(about), "Exported from %s, slot %d", d->disc, c);
        snprintf(man, sizeof(man), "format=1\nname=%s\nform=%s\n", name, form);
        if (p->replaces[c] >= 0) {
            snprintf(man + strlen(man), sizeof(man) - strlen(man), "replaces=%d\n", p->replaces[c]);
        } else {
            snprintf(man + strlen(man), sizeof(man) - strlen(man), "base=%d\n", c);
        }
        snprintf(man + strlen(man), sizeof(man) - strlen(man), "author=%s\nabout=%s\n", d->disc, about);
        for (k = 0; k < 4; k++) {
            int to = t != NULL ? t[0x98 + k] : 0xFF;
            if (to != c && to < DI_CHARAS && chosen[to]) {
                ref(pl, c, to, v, sizeof(v));
            } else {
                snprintf(v, sizeof(v), "none");
            }
            snprintf(man + strlen(man), sizeof(man) - strlen(man), "%s=%s\n", kKeys[k], v);
            if (p->transformCost >= 0) {
                snprintf(man + strlen(man), sizeof(man) - strlen(man), "%s_cost=%d\n", kKeys[k], p->transformCost);
            }
        }
        for (k = 0; k < 3; k++) {
            int to = t != NULL ? t[0xB4 + k] : 0xFF, m, any = 0;
            char who[600] = "";
            if (t != NULL) {
                for (m = 0; m < 4; m++) {
                    int pt = t[0xBA + k * 4 + m];
                    if (pt < DI_CHARAS && chosen[pt]) {
                        ref(pl, c, pt, v, sizeof(v));
                        if (strstr(who, v) == NULL) {
                            snprintf(who + strlen(who), sizeof(who) - strlen(who), "%s%s", any ? "," : "", v);
                            any = 1;
                        }
                    }
                }
            }
            if (to < DI_CHARAS && chosen[to] && any) {
                ref(pl, c, to, v, sizeof(v));
                snprintf(man + strlen(man), sizeof(man) - strlen(man), "fusion%d=%s\npartner%d=%s\n", k + 1, v, k + 1, who);
            } else {
                snprintf(man + strlen(man), sizeof(man) - strlen(man), "fusion%d=none\n", k + 1);
            }
        }
        if (pl[c].leaf[0] == '\0' && !(link && pl[c].top[0])) {
            /* (a character on its own, not linked: a folder of its own name) */
        }
        if (pl[c].top[0] == '\0') {
            char title[200], clean[128];
            snprintf(title, sizeof(title), it->form[0] ? "%s - %s" : "%s", it->name, it->form);
            clean_name(title, clean, sizeof(clean));
            unique(&taken, clean[0] ? clean : "Character", pl[c].top, sizeof(pl[c].top));
        }
        if (pl[c].leaf[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s/%s", stage, pl[c].top, pl[c].leaf);
        } else {
            snprintf(path, sizeof(path), "%s/%s", stage, pl[c].top);
        }
        if (write_chara(d, c, path, man, notes, notesLen)) {
            made++;
        }
        if (progress != NULL) {
            *progress = ++done * 1000 / (total ? total : 1);
        }
    }
    /* move the heads' folders in whole */
    for (c = 0; c < DI_CHARAS; c++) {
        char from[700], to[700];
        if (!chosen[c] || pl[c].leaf[0] != '\0') {
            continue;
        }
        snprintf(from, sizeof(from), "%s/%s", stage, pl[c].top);
        snprintf(to, sizeof(to), "%s/%s", characters, pl[c].top);
        if (rename(from, to) != 0) {
            note(notes, notesLen, "%s could not be moved into the characters folder. ", pl[c].top);
        }
    }
#undef TOP

    /* stages and songs */
    for (k = 0; k < d->count; k++) {
        const DiItem *it = &d->items[k];
        char base[700], file[128], nm[96];
        long n;
        unsigned char *b;
        int which = it->kind == DI_STAGE ? EX_STAGES : EX_SONGS;
        if (it->kind == DI_CHARA || !p->take[k]) {
            continue;
        }
        fit(p->names[k] != NULL && p->names[k][0] ? p->names[k] : it->name, nm, 63);
        clean_name(nm, file, sizeof(file));
        if (file[0] == '\0') {
            snprintf(file, sizeof(file), "%s %d", it->kind == DI_STAGE ? "Stage" : "Song", it->index + 1);
        }
        snprintf(base, sizeof(base), "%s/%s", stage, file);
        if (it->kind == DI_STAGE) {
            if ((b = file_read(d, STAGE_ID(it->index), 64L << 20, &n)) != NULL) {
                snprintf(path, sizeof(path), "%s.unk", base);
                write_file(path, b, n);
                free(b);
            }
            if ((b = file_read(d, THUMB_ID(it->index), 4L << 20, &n)) != NULL) {
                snprintf(path, sizeof(path), "%s.thumb", base);
                write_file(path, b, n);
                free(b);
            }
            if (it->chipLen > 0) {
                snprintf(path, sizeof(path), "%s.icon", base);
                write_file(path, it->chip, it->chipLen);
            }
            snprintf(path, sizeof(path), "%s.unk", base);
        } else {
            if ((b = file_read(d, SONG_ID(it->index), 128L << 20, &n)) != NULL) {
                snprintf(path, sizeof(path), "%s.adx", base);
                write_file(path, b, n);
                free(b);
            }
            snprintf(path, sizeof(path), "%s.adx", base);
        }
        if (Ex_Add(which, which == EX_STAGES ? stages : songs, path, nm)) {
            made++;
        } else {
            note(notes, notesLen, "%s was already there. ", nm);
        }
        remove(path);
        snprintf(path, sizeof(path), "%s.thumb", base);
        remove(path);
        snprintf(path, sizeof(path), "%s.icon", base);
        remove(path);
        if (progress != NULL) {
            *progress = ++done * 1000 / (total ? total : 1);
        }
    }
    rmdir(stage);
    free(taken.names);
    if (progress != NULL) {
        *progress = 1000;
    }
    return made;
}

/* ---- the stages and songs folders ------------------------------------------------------------------------ */

static const char *const kStageExt[] = {".unk", NULL};
static const char *const kSongExt[] = {".mp3", ".wav", ".flac", ".adx", NULL};
static const char *const kSides[] = {".thumb", ".icon"};

static int has_ext(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    int i;
    for (i = 0; dot != NULL && exts[i] != NULL; i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *list_name(int which) { return which == EX_STAGES ? "maps.txt" : "songs.txt"; }

/* the list of a folder: "file|Name" lines */
typedef struct {
    char file[128];
    char name[64];
} Line;

static int read_list(const char *dir, int which, Line *out, int max) {
    char path[700], buf[512];
    FILE *f;
    int n = 0;
    snprintf(path, sizeof(path), "%s/%s", dir, list_name(which));
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    while (n < max && fgets(buf, sizeof(buf), f) != NULL) {
        char *bar = strchr(buf, '|'), *e;
        if (buf[0] == '#') {
            continue;
        }
        for (e = buf + strlen(buf); e > buf && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) {
            e[-1] = '\0';
        }
        if (bar != NULL) {
            *bar = '\0';
        }
        if (buf[0] == '\0') {
            continue;
        }
        snprintf(out[n].file, sizeof(out[n].file), "%s", buf);
        snprintf(out[n].name, sizeof(out[n].name), "%s", bar != NULL ? bar + 1 : "");
        n++;
    }
    fclose(f);
    return n;
}

static int write_list(const char *dir, int which, const Line *l, int n) {
    char path[700], tmp[720];
    FILE *f;
    int i;
    snprintf(path, sizeof(path), "%s/%s", dir, list_name(which));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((f = fopen(tmp, "wb")) == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        fprintf(f, "%s|%s\n", l[i].file, l[i].name);
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

/* "my_cool-map.unk" -> "my cool-map" (as port/src/plat_extras.c names a file a list does not) */
static void name_of_file(const char *file, char *out, int size) {
    const char *dot = strrchr(file, '.');
    int n = dot != NULL ? (int)(dot - file) : (int)strlen(file), i;
    if (n >= size) {
        n = size - 1;
    }
    for (i = 0; i < n; i++) {
        out[i] = file[i] == '_' ? ' ' : file[i];
    }
    out[n] = '\0';
}

static long long size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/* the entries of one folder (on: dir, off: dir/off), in the order of its list, then by name */
static int list_folder(int which, const char *dir, int on, ExEntry *out, int max) {
    const char *const *exts = which == EX_STAGES ? kStageExt : kSongExt;
    static Line lines[512];
    char files[512][128], path[700];
    int nl = read_list(dir, which, lines, 512), nf = 0, n = 0, i, j;
    DIR *dd = opendir(dir);
    struct dirent *de;
    if (dd == NULL) {
        return 0;
    }
    while ((de = readdir(dd)) != NULL && nf < 512) {
        if (de->d_name[0] != '.' && has_ext(de->d_name, exts) && strlen(de->d_name) < 128) {
            snprintf(files[nf++], 128, "%s", de->d_name);
        }
    }
    closedir(dd);
    qsort(files, (size_t)nf, 128, (int (*)(const void *, const void *))strcasecmp);
    for (i = 0; i < nl && n < max; i++) {
        for (j = 0; j < nf; j++) {
            if (files[j][0] != '\0' && strcasecmp(files[j], lines[i].file) == 0) {
                snprintf(out[n].file, sizeof(out[n].file), "%s", files[j]);
                if (lines[i].name[0]) {
                    snprintf(out[n].name, sizeof(out[n].name), "%s", lines[i].name);
                } else {
                    name_of_file(files[j], out[n].name, sizeof(out[n].name));
                }
                snprintf(path, sizeof(path), "%s/%s", dir, files[j]);
                out[n].size = size_of(path);
                out[n].on = on;
                n++;
                files[j][0] = '\0';
            }
        }
    }
    for (j = 0; j < nf && n < max; j++) {
        if (files[j][0] == '\0') {
            continue;
        }
        snprintf(out[n].file, sizeof(out[n].file), "%s", files[j]);
        name_of_file(files[j], out[n].name, sizeof(out[n].name));
        snprintf(path, sizeof(path), "%s/%s", dir, files[j]);
        out[n].size = size_of(path);
        out[n].on = on;
        n++;
    }
    return n;
}

int Ex_List(int which, const char *dir, ExEntry *out, int max) {
    char off[640];
    int n = list_folder(which, dir, 1, out, max);
    snprintf(off, sizeof(off), "%s/off", dir);
    return n + list_folder(which, off, 0, out + n, max - n);
}

/* the folder's list written from its entries (names only where they are not the file's own) */
static int save_folder(int which, const char *dir, const ExEntry *e, int n) {
    static Line l[512];
    int i, k = 0;
    for (i = 0; i < n && k < 512; i++) {
        snprintf(l[k].file, sizeof(l[k].file), "%s", e[i].file);
        snprintf(l[k].name, sizeof(l[k].name), "%s", e[i].name);
        k++;
    }
    return write_list(dir, which, l, k);
}

static int move_with_sides(const char *fromDir, const char *toDir, const char *file) {
    char a[700], b[700], base[128];
    const char *dot = strrchr(file, '.');
    int i, n = dot != NULL ? (int)(dot - file) : (int)strlen(file);
    make_dirs(toDir);
    snprintf(a, sizeof(a), "%s/%s", fromDir, file);
    snprintf(b, sizeof(b), "%s/%s", toDir, file);
    if (rename(a, b) != 0) {
        return 0;
    }
    snprintf(base, sizeof(base), "%.*s", n, file);
    for (i = 0; i < 2; i++) {
        snprintf(a, sizeof(a), "%s/%s%s", fromDir, base, kSides[i]);
        snprintf(b, sizeof(b), "%s/%s%s", toDir, base, kSides[i]);
        rename(a, b);
    }
    return 1;
}

int Ex_SetOn(int which, const char *dir, const char *file, int on) {
    static ExEntry from[512], to[512];
    char off[640];
    const char *src, *dst;
    int nf, nt, i;
    snprintf(off, sizeof(off), "%s/off", dir);
    src = on ? off : dir;
    dst = on ? dir : off;
    nf = list_folder(which, src, !on, from, 512);
    nt = list_folder(which, dst, on, to, 512);
    for (i = 0; i < nf; i++) {
        if (strcmp(from[i].file, file) == 0) {
            if (!move_with_sides(src, dst, file)) {
                return 0;
            }
            to[nt++] = from[i];
            memmove(&from[i], &from[i + 1], (size_t)(nf - i - 1) * sizeof(ExEntry));
            nf--;
            save_folder(which, src, from, nf);
            save_folder(which, dst, to, nt);
            return 1;
        }
    }
    return 0;
}

int Ex_Rename(int which, const char *dir, const char *file, const char *name) {
    static ExEntry e[512];
    char off[640];
    int pass, n, i;
    snprintf(off, sizeof(off), "%s/off", dir);
    for (pass = 0; pass < 2; pass++) {
        const char *d = pass == 0 ? dir : off;
        n = list_folder(which, d, pass == 0, e, 512);
        for (i = 0; i < n; i++) {
            if (strcmp(e[i].file, file) == 0) {
                fit(name, e[i].name, 63);
                for (char *c = e[i].name; *c; c++) {
                    if (*c == '|') {
                        *c = ' ';
                    }
                }
                return save_folder(which, d, e, n);
            }
        }
    }
    return 0;
}

int Ex_Move(int which, const char *dir, const char *file, int by) {
    static ExEntry e[512];
    int n = list_folder(which, dir, 1, e, 512), i;
    for (i = 0; i < n; i++) {
        if (strcmp(e[i].file, file) == 0 && i + by >= 0 && i + by < n) {
            ExEntry t = e[i];
            e[i] = e[i + by];
            e[i + by] = t;
            return save_folder(which, dir, e, n);
        }
    }
    return 0;
}

int Ex_Remove(int which, const char *dir, const char *file) {
    static ExEntry e[512];
    char off[640], gone[640];
    int pass, n, i;
    snprintf(off, sizeof(off), "%s/off", dir);
    snprintf(gone, sizeof(gone), "%s/.removed", dir);
    for (pass = 0; pass < 2; pass++) {
        const char *d = pass == 0 ? dir : off;
        n = list_folder(which, d, pass == 0, e, 512);
        for (i = 0; i < n; i++) {
            if (strcmp(e[i].file, file) == 0) {
                if (!move_with_sides(d, gone, file)) {
                    return 0;
                }
                memmove(&e[i], &e[i + 1], (size_t)(n - i - 1) * sizeof(ExEntry));
                return save_folder(which, d, e, n - 1);
            }
        }
    }
    return 0;
}

static int same_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[65536], bb[65536];
    int same = fa != NULL && fb != NULL;
    while (same) {
        size_t na = fread(ba, 1, sizeof(ba), fa), nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) {
            same = 0;
        }
        if (na == 0) {
            break;
        }
    }
    if (fa) {
        fclose(fa);
    }
    if (fb) {
        fclose(fb);
    }
    return same;
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    int ok = 1;
    if (a == NULL) {
        return 0;
    }
    if ((b = fopen(to, "wb")) == NULL) {
        fclose(a);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) {
        ok &= fwrite(buf, 1, n, b) == n;
    }
    fclose(a);
    return (fclose(b) == 0) && ok;
}

int Ex_Add(int which, const char *dir, const char *source, const char *name) {
    static ExEntry e[1024];
    const char *dot = strrchr(source, '.'), *slash;
    char base[128], file[160], clean[128], path[700], off[640];
    long long size = size_of(source);
    int n, i, k, onCount = 0, limit = which == EX_STAGES ? 26 : 40;
    if (size <= 0 || dot == NULL) {
        return 0;
    }
    make_dirs(dir);
    n = Ex_List(which, dir, e, 1024);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof(path), e[i].on ? "%s/%s" : "%s/off/%s", dir, e[i].file);
        if (e[i].size == size && same_file(path, source)) {
            return 0;
        }
        onCount += e[i].on;
    }
    if (name != NULL && name[0] != '\0') {
        clean_name(name, clean, sizeof(clean));
    } else {
        slash = strrchr(source, '/');
        if (strrchr(source, '\\') > slash) {
            slash = strrchr(source, '\\');
        }
        snprintf(base, sizeof(base), "%.*s", (int)(dot - (slash ? slash + 1 : source)), slash ? slash + 1 : source);
        clean_name(base, clean, sizeof(clean));
    }
    if (clean[0] == '\0') {
        snprintf(clean, sizeof(clean), "%s", which == EX_STAGES ? "Stage" : "Song");
    }
    /* a file name not used on or off */
    for (k = 1;; k++) {
        int used = 0;
        if (k == 1) {
            snprintf(file, sizeof(file), "%s%s", clean, dot);
        } else {
            snprintf(file, sizeof(file), "%s (%d)%s", clean, k, dot);
        }
        for (i = 0; i < n && !used; i++) {
            used = strcasecmp(e[i].file, file) == 0;
        }
        if (!used) {
            break;
        }
    }
    /* full: it is kept, switched off */
    snprintf(off, sizeof(off), "%s/off", dir);
    {
        const char *into = onCount < limit ? dir : off;
        make_dirs(into);
        snprintf(path, sizeof(path), "%s/%s", into, file);
        if (!copy_file(source, path)) {
            return 0;
        }
        if (which == EX_STAGES) {
            for (i = 0; i < 2; i++) {
                char from[700], to[700];
                snprintf(from, sizeof(from), "%.*s%s", (int)(dot - source), source, kSides[i]);
                snprintf(to, sizeof(to), "%s/%.*s%s", into, (int)(strrchr(file, '.') - file), file, kSides[i]);
                if (size_of(from) > 0) {
                    copy_file(from, to);
                }
            }
        }
        {
            static ExEntry here[512];
            int h = list_folder(which, into, onCount < limit, here, 512);
            for (i = 0; i < h; i++) {
                if (strcmp(here[i].file, file) == 0 && name != NULL && name[0]) {
                    fit(name, here[i].name, 63);
                }
            }
            save_folder(which, into, here, h);
        }
    }
    return 1;
}

/* a folder that is a package (it holds a manifest or a first costume) */
static int is_package(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int yes = 0;
    if (d == NULL) {
        return 0;
    }
    while (!yes && (de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        yes = strcasecmp(de->d_name, "character.txt") == 0 || (l > 7 && strcasecmp(de->d_name + l - 7, "_1p.pak") == 0);
    }
    closedir(d);
    return yes;
}

static void listed_name(const char *dir, const char *file, char *out, int size) {
    static Line l[512];
    int which, n, i;
    out[0] = '\0';
    for (which = 0; which < 2; which++) {
        n = read_list(dir, which, l, 512);
        for (i = 0; i < n; i++) {
            if (strcasecmp(l[i].file, file) == 0 && l[i].name[0]) {
                snprintf(out, size, "%s", l[i].name);
                return;
            }
        }
    }
}

/* a stage's model: a pack of some twenty sections, large (a .unk may be a character's too: 252 sections, 440, 6 or 3) */
static int is_stage_file(const char *path) {
    unsigned char h[4];
    FILE *f = fopen(path, "rb");
    uint32_t n = 0;
    if (f != NULL) {
        if (fread(h, 1, 4, f) == 4) {
            n = rd32(h);
        }
        fclose(f);
    }
    return n >= 16 && n <= 40 && size_of(path) >= 1L << 20;
}

int Ex_AddLoose(const char *root, const char *stagesDir, const char *songsDir) {
    DIR *d;
    struct dirent *de;
    char path[700], name[64];
    int n = 0;
    if (is_package(root) || (d = opendir(root)) == NULL) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            n += Ex_AddLoose(path, stagesDir, songsDir);
        } else if ((has_ext(de->d_name, kStageExt) && is_stage_file(path)) || (has_ext(de->d_name, kSongExt) && (strcasecmp(strrchr(de->d_name, '.'), ".adx") != 0 || st.st_size >= 1L << 20))) {
            int stage = has_ext(de->d_name, kStageExt); /* (a small .adx is a spoken line, of a character) */
            listed_name(root, de->d_name, name, sizeof(name));
            n += Ex_Add(stage ? EX_STAGES : EX_SONGS, stage ? stagesDir : songsDir, path, name[0] ? name : NULL);
        }
    }
    closedir(d);
    return n;
}

/* the names of the game's own characters (the player's data: the select's name list), "" where there is none */
int Di_StockNames(const char *gamedata, char (*names)[64], int max) {
    char path[600];
    FILE *f;
    long n, at, len, rl;
    unsigned char *b, *res;
    int c, got = 0;
    MenuPack m;
    for (c = 0; c < max; c++) {
        names[c][0] = '\0';
    }
    snprintf(path, sizeof(path), "%s/pzs3us1/00448.bin", gamedata);
    if ((f = fopen(path, "rb")) == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = n > 0 && n < (16L << 20) ? malloc((size_t)n) : NULL;
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n || !section(b, n, 2, &at, &len) || (res = Port_BpeUnpack(b + at, len, 32L << 20, &rl)) == NULL) {
        free(b);
        fclose(f);
        return 0;
    }
    fclose(f);
    free(b);
    memset(&m, 0, sizeof(m));
    m.res = res;
    m.resLen = rl;
    if (section(res, rl, 43, &m.at[0], &m.len[0])) {
        for (c = 0; c < max; c++) {
            const unsigned char *p;
            long l;
            if (menu_entry(&m, 0, c + 1, &p, &l)) {
                utf8_of(p, l, names[c], 64);
                got += names[c][0] != '\0';
            }
        }
    }
    free(res);
    return got;
}
