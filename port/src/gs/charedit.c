/* Changing what a character package's model files hold (port/include/plat_charedit.h).
 *
 * A model file is a pack: a count, the sections' offsets, the sections. The tables (layouts from
 * include/battle/btl_tech_a.h and btl_tech_b.h): section 18 the fighter's table (from 0x98 its forms and fusions),
 * 19 melee attacks (0xA3 records of 0x30), 20 ki blasts (13 of 0x34), 21 movement, 24 Blast 2 and Ultimate (each
 * field an array of 3), 25 Blast 1 (arrays of 2); 45..52 the skill list in each language (UTF-16 text, a move per
 * "!F*<cost><name>" line). Some disc models end before their table says: a section is cut at the file's end. */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "../../include/plat_charedit.h"

#define MODEL_MAX 0x800000L /* PORT_MODEL_MAX */
#define ANM_MAX 1443840L
#define EFF_MAX 845824L
#define FIRST_TABLE 18
#define LAST_TABLE 52
#define PARAM_KEEP 0x98
#define TEXT_FIRST 45
#define TEXT_LAST 52

static uint32_t rd32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void wr32(unsigned char *b, uint32_t v) {
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
}

/* ---- the tables ---- */

#define HP "health points, 10000 = one bar"
#define KI "100000 = full ki gauge"
#define KMH "10 km/h"
#define SEC "seconds"

static const CeCol kFighter[] = {
    {"Ki charge rate", 0x30, CE_S32, 0, "per second"}, {"Ki charge rate in water", 0x34, CE_S32, 0, "per second"},
    {"Ki regeneration", 0x38, CE_S32, 0, "per second"}, {"Ki regeneration stops at", 0x28, CE_S32, 0, KI},
    {"Ki recovery when exhausted", 0x3C, CE_S32, 0, "per second"}, {"Exhaustion ends at", 0x2C, CE_S32, 0, KI},
    {"Blast stock gain", 0x44, CE_S32, 0, "per second"}, {"Air recovery ki cost", 0x48, CE_S32, 0, KI},
    {"Ki spent by actions 0x67..0x69", 0x4C, CE_S32, 0, "per second"}, {"Max Power: time to charge", 0x50, CE_F32, 0, SEC},
    {"Max Power: duration", 0x54, CE_F32, 0, SEC}, {"Damage taken multiplier", 0x7C, CE_F32, 0, "1 = normal"},
    {"Hit reaction scale", 0x60, CE_F32, 0, "1 = normal"}, {"Attack charge rate (empty)", 0x74, CE_F32, 0, ""},
    {"Attack charge rate (full)", 0x78, CE_F32, 0, ""}, {"Ki blasts at once (class 0)", 0x80, CE_S16, 0, ""},
    {"Ki blasts at once (class 1)", 0x82, CE_S16, 0, ""}, {"Aura kind", 0x03, CE_S8, 0, ""},
};

static CeCol kMove[20 + 2 + 1 + 2 + 4 + 3 + 4 + 4];
static char kMoveNames[sizeof(kMove) / sizeof(kMove[0])][40];

static const CeCol kBlasts[] = {
    {"Damage", 0x04, CE_S32, 0, HP}, {"Guard damage", 0x08, CE_S32, 0, HP}, {"Ki cost", 0x0C, CE_S32, 0, KI},
    {"Speed", 0x10, CE_F32, 0, KMH}, {"Homing", 0x14, CE_F32, 0, "degrees per second"}, {"Lifetime", 0x18, CE_F32, 0, SEC},
    {"Knock-back", 0x1C, CE_F32, 0, KMH}, {"Guard knock-back", 0x20, CE_F32, 0, KMH}, {"Reaction", 0x24, CE_S8, 0, ""},
};

static const CeCol kBlast1[] = {
    {"Blast stocks", 0x94, CE_S8, 1, ""}, {"Damage", 0x84, CE_S32, 4, HP}, {"Guard damage", 0x8C, CE_S32, 4, HP},
    {"Effect time", 0xA0, CE_F32, 4, SEC}, {"Health given %", 0xA8, CE_S32, 4, "negative takes"}, {"Ki given", 0xB0, CE_S32, 4, KI},
    {"Stat level 0", 0x96, CE_S8, 1, ""}, {"Stat level 1", 0x9A, CE_S8, 1, ""}, {"Stat level 2", 0x98, CE_S8, 1, ""},
    {"Stat level 3", 0x9C, CE_S8, 1, ""}, {"Shot speed", 0x20, CE_F32, 4, KMH}, {"Shot homing", 0x28, CE_F32, 4, ""},
    {"Shot time", 0x18, CE_F32, 4, SEC}, {"Knock-back", 0x74, CE_F32, 4, ""}, {"Hits", 0x3A, CE_S8, 1, ""}, {"Reaction", 0x68, CE_S8, 1, ""},
};

static const CeCol kBlast2[] = {
    {"Ki cost", 0x19C, CE_S32, 4, KI}, {"Damage", 0x184, CE_S32, 4, HP}, {"Guard damage", 0x190, CE_S32, 4, HP},
    {"Damage to self", 0x1A8, CE_S32, 4, HP}, {"Drain", 0x1B4, CE_S32, 4, ""}, {"Cooldown", 0x1C0, CE_F32, 4, SEC},
    {"Charge time limit", 0x1CC, CE_F32, 4, SEC}, {"Full charge damage x", 0x1D8, CE_F32, 4, ""},
    {"Shot speed", 0x54, CE_F32, 4, KMH}, {"Shot homing", 0x60, CE_F32, 4, "degrees per second"}, {"Shot time", 0x24, CE_F32, 4, SEC},
    {"Rush speed", 0x1FC, CE_F32, 4, KMH}, {"Knock-back", 0x16C, CE_F32, 4, KMH}, {"Guard knock-back", 0x178, CE_F32, 4, KMH},
    {"Shots", 0x93, CE_S8, 1, ""}, {"Rush steps", 0x159, CE_S8, 1, ""}, {"Reaction", 0x141, CE_S8, 1, ""}, {"Guard kind", 0x150, CE_S8, 1, ""},
};

static const CeCol kMelee[] = {
    {"Damage", 0x04, CE_U16, 0, HP}, {"Guard damage", 0x06, CE_U16, 0, HP}, {"Guard ki cost", 0x08, CE_U16, 0, ""},
    {"Ki gained", 0x0A, CE_U16, 0, ""}, {"Push on hit", 0x12, CE_U8, 0, KMH}, {"Push on guard", 0x13, CE_U8, 0, KMH},
    {"Launch speed", 0x14, CE_U8, 0, KMH}, {"Launch yaw", 0x15, CE_S8, 0, "degrees"}, {"Launch pitch", 0x16, CE_S8, 0, "degrees"},
    {"Camera shake", 0x17, CE_U8, 0, ""}, {"Reaction", 0x1B, CE_S8, 0, ""}, {"Priority", 0x26, CE_S8, 0, ""},
    {"Ignores armour", 0x2C, CE_S8, 0, ""},
};

#define N(a) (int)(sizeof(a) / sizeof(a[0]))
static CeTable kTables[] = {
    {"Fighter", 18, 1, 0, kFighter, N(kFighter), "Health, starting ki and blast stocks are with the forms (they are the package's own keys)."},
    {"Movement", 21, 1, 0, kMove, N(kMove), "Which speed is which (dash, flight, ...) is not documented: change one at a time and try it."},
    {"Ki blasts", 20, 13, 0x34, kBlasts, N(kBlasts), ""},
    {"Blast 1", 25, 2, 0, kBlast1, N(kBlast1), ""},
    {"Blast 2 and Ultimate", 24, 3, 0, kBlast2, N(kBlast2), ""},
    {"Melee attacks", 19, 0xA3, 0x30, kMelee, N(kMelee), "Damage is per attack: the game shares it among the hits of a multi-hit animation."},
};

static void tables_init(void) {
    static int done;
    static const struct {
        const char *name;
        int off, type, n;
        const char *hint;
    } kArr[] = {{"Speed", 0x00, CE_F32, 20, KMH},          {"Acceleration", 0x50, CE_F32, 2, KMH " per second"},
                {"Angle", 0x58, CE_F32, 1, "degrees"},     {"Height", 0x5C, CE_F32, 2, "x10"},
                {"Ki cost of movement", 0x64, CE_S32, 4, "per second"}, {"Turn rate", 0x74, CE_F32, 3, "degrees per second"},
                {"Turn acceleration", 0x80, CE_F32, 4, ""}, {"Turn limit", 0x90, CE_F32, 4, "degrees"}};
    int a, i, k = 0;
    if (done) {
        return;
    }
    done = 1;
    for (a = 0; a < N(kArr); a++) {
        for (i = 0; i < kArr[a].n; i++, k++) {
            if (kArr[a].n == 1) {
                snprintf(kMoveNames[k], sizeof(kMoveNames[k]), "%s", kArr[a].name);
            } else {
                snprintf(kMoveNames[k], sizeof(kMoveNames[k]), "%s %d", kArr[a].name, i + 1);
            }
            kMove[k].name = kMoveNames[k];
            kMove[k].off = kArr[a].off + i * 4;
            kMove[k].type = kArr[a].type;
            kMove[k].step = 0;
            kMove[k].hint = kArr[a].hint;
        }
    }
}

int Ce_TableCount(void) { return N(kTables); }
const CeTable *Ce_Table(int t) {
    tables_init();
    return t >= 0 && t < N(kTables) ? &kTables[t] : NULL;
}

void Ce_RowLabel(int t, int row, char *out, int size) {
    static const char *const kBlast[13] = {"Shot", "Charged shot (low)", "Charged shot (mid)", "Charged shot (full)", "Shot B", "Charged B (low)",
                                           "Charged B (mid)", "Charged B (full)", "Shot C", "Charged C (low)", "Charged C (mid)", "Charged C (full)",
                                           "Special shot"};
    static const char *const kHeavy[3] = {"weak", "medium", "full"};
    out[0] = '\0';
    switch (t) {
    case 2:
        snprintf(out, size, "%s", row >= 0 && row < 13 ? kBlast[row] : "");
        break;
    case 3:
        snprintf(out, size, "Blast 1 (%c)", 'A' + row);
        break;
    case 4:
        snprintf(out, size, row < 2 ? "Blast 2 (%c)" : "Ultimate", 'A' + row);
        break;
    case 5:
        if (row == 0) {
            snprintf(out, size, "Rush: first hit");
        } else if (row == 1) {
            snprintf(out, size, "Rush: later hit");
        } else if (row <= 10) {
            snprintf(out, size, "Rush combo step %d", row);
        } else if (row <= 0xD) {
            snprintf(out, size, "Heavy blow %s", kHeavy[row - 0xB]);
        } else if (row >= 0x13 && row <= 0x21) {
            snprintf(out, size, "Charged smash %d %s", (row - 0x13) % 5 + 1, kHeavy[(row - 0x13) / 5]);
        } else if (row == 0x55) {
            snprintf(out, size, "Grab / throw");
        } else if (row == 0x66) {
            snprintf(out, size, "Ki charge");
        } else {
            snprintf(out, size, "Attack 0x%02X", row);
        }
        break;
    }
}

static int type_size(int t) { return t <= CE_S8 ? 1 : t <= CE_S16 ? 2 : 4; }

/* ---- files ---- */

static unsigned char *read_all(const char *path, long max, long *n) {
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long size;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > max || (b = malloc((size_t)size)) == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)size, f) != (size_t)size) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = size;
    return b;
}

static int write_all(const char *path, const unsigned char *b, long n) {
    char tmp[720];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((f = fopen(tmp, "wb")) == NULL) {
        return 0;
    }
    ok = fwrite(b, 1, (size_t)n, f) == (size_t)n;
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return 0;
    }
    remove(path);
    if (rename(tmp, path) == 0) {
        return 1;
    }
    { /* (the old file may be held open: moved aside if its opener allowed it (FILE_SHARE_DELETE), and the new one put in
         its place. The game's own reads keep no paks open: plat_fcache.c holds a copy in memory.) */
        char old[730];
        snprintf(old, sizeof(old), "%s.old", path);
        remove(old);
        if (rename(path, old) == 0) {
            if (rename(tmp, path) == 0) {
                remove(old);
                return 1;
            }
            rename(old, path);
        }
    }
    remove(tmp);
    return 0;
}

static long long size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/* <file>.orig kept once; an empty one for a file that was not there */
static void keep_original(const char *path) {
    char orig[720];
    long n;
    unsigned char *b;
    snprintf(orig, sizeof(orig), "%s.orig", path);
    if (size_of(orig) >= 0) {
        return;
    }
    b = read_all(path, 64L << 20, &n);
    write_all(orig, b != NULL ? b : (const unsigned char *)"", b != NULL ? n : 0);
    free(b);
}

/* m_<n>p.pak / m_<n>p_dmg.pak (any prefix, any case) */
static int is_costume(const char *name) {
    size_t l = strlen(name);
    if (l < 7 || strcasecmp(name + l - 4, ".pak") != 0) {
        return 0;
    }
    l -= 4;
    if (l >= 7 && strncasecmp(name + l - 4, "_dmg", 4) == 0) {
        l -= 4;
    }
    return l >= 3 && name[l - 3] == '_' && name[l - 2] >= '1' && name[l - 2] <= '4' && (name[l - 1] == 'p' || name[l - 1] == 'P');
}

static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* the costume files of dir, by name; *n how many */
static char (*costumes(const char *dir, int *n))[128] {
    static char list[16][128];
    DIR *d = opendir(dir);
    struct dirent *de;
    *n = 0;
    if (d == NULL) {
        return list;
    }
    while ((de = readdir(d)) != NULL && *n < 16) {
        size_t l = strlen(de->d_name);
        if (l < 128 && is_costume(de->d_name)) {
            /* (m_1p_dmg.pak has "_dmg" before ".pak": is_costume takes both) */
            snprintf(list[(*n)++], 128, "%s", de->d_name);
        }
    }
    closedir(d);
    qsort(list, (size_t)*n, 128, cmp_name);
    return list;
}

/* the first file of dir whose name ends so (by name) */
static int find_file(const char *dir, const char *ending, char *out, int size) {
    DIR *d = opendir(dir);
    struct dirent *de;
    char best[128] = "";
    size_t e = strlen(ending);
    if (d == NULL) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l >= e && l < 128 && strcasecmp(de->d_name + l - e, ending) == 0 && (best[0] == '\0' || strcasecmp(de->d_name, best) < 0)) {
            snprintf(best, sizeof(best), "%s", de->d_name);
        }
    }
    closedir(d);
    if (best[0] == '\0') {
        return 0;
    }
    snprintf(out, size, "%s/%s", dir, best);
    return 1;
}

/* ---- packs ---- */

static int pack_count(const unsigned char *b, long n) {
    uint32_t c = n >= 8 ? rd32(b) : 0;
    return c > 0 && (long)(c + 2) * 4 <= n ? (int)c : 0;
}

static long sec_start(const unsigned char *b, long n, int i) {
    long a = (long)(rd32(b + 4 * i) & ~3u);
    return a < n ? a : n;
}

static long sec_end(const unsigned char *b, long n, int i) {
    long z = (long)(rd32(b + 4 * (i + 1)) & ~3u), a = sec_start(b, n, i);
    if (z > n) {
        z = n;
    }
    return z < a ? a : z;
}

typedef struct {
    const unsigned char *b;
    long n;
} Span;

/* the pack laid out again with some sections replaced (each section on a 64-byte boundary) */
static unsigned char *rebuild(const unsigned char *b, long n, const Span *repl, long *outLen) {
    int count = pack_count(b, n), i;
    long head, total, pos;
    unsigned char *o;
    if (count == 0) {
        return NULL;
    }
    head = (long)(rd32(b + 4) & ~3u);
    if (head < (count + 2) * 4) {
        head = (count + 2) * 4;
    }
    total = head + 64;
    for (i = 1; i <= count; i++) {
        total += (repl != NULL && repl[i].b != NULL ? repl[i].n : sec_end(b, n, i) - sec_start(b, n, i)) + 64;
    }
    if ((o = calloc(1, (size_t)total)) == NULL) {
        return NULL;
    }
    wr32(o, (uint32_t)count);
    pos = head;
    for (i = 1; i <= count; i++) {
        const unsigned char *s = repl != NULL && repl[i].b != NULL ? repl[i].b : b + sec_start(b, n, i);
        long l = repl != NULL && repl[i].b != NULL ? repl[i].n : sec_end(b, n, i) - sec_start(b, n, i);
        pos = (pos + 63) / 64 * 64;
        wr32(o + 4 * i, (uint32_t)pos);
        memcpy(o + pos, s, (size_t)l);
        pos += l;
    }
    pos = (pos + 63) / 64 * 64;
    wr32(o + 4 * (count + 1), (uint32_t)pos);
    *outLen = pos;
    return o;
}

/* the body of model with the donor's tables (sections 18..52; of 18 only what comes before its forms) */
static int part_of(int sec) { /* which CE_PART_* a table section belongs to */
    return sec == 18 ? CE_PART_FIGHTER : sec == 19 ? CE_PART_MELEE : sec == 20 ? CE_PART_KI : sec == 21 ? CE_PART_MOVE
           : sec >= TEXT_FIRST ? CE_PART_TEXT : CE_PART_BLASTS;
}

static unsigned char *transplant(const unsigned char *m, long mn, const unsigned char *d, long dn, int parts, long *outLen) {
    static Span repl[256];
    int count = pack_count(m, mn), i;
    unsigned char *param = NULL, *o;
    if (count == 0 || count != pack_count(d, dn) || count < LAST_TABLE || count >= 256) {
        return NULL;
    }
    memset(repl, 0, sizeof(repl));
    for (i = FIRST_TABLE; i <= LAST_TABLE; i++) {
        if (parts & part_of(i)) {
            repl[i].b = d + sec_start(d, dn, i);
            repl[i].n = sec_end(d, dn, i) - sec_start(d, dn, i);
        }
    }
    if (repl[FIRST_TABLE].b != NULL) {
        long ml = sec_end(m, mn, FIRST_TABLE) - sec_start(m, mn, FIRST_TABLE);
        if (ml < PARAM_KEEP || repl[FIRST_TABLE].n < PARAM_KEEP || (param = malloc((size_t)ml)) == NULL) {
            return NULL;
        }
        memcpy(param, m + sec_start(m, mn, FIRST_TABLE), (size_t)ml);
        memcpy(param, repl[FIRST_TABLE].b, PARAM_KEEP);
        repl[FIRST_TABLE].b = param;
        repl[FIRST_TABLE].n = ml;
    }
    o = rebuild(m, mn, repl, outLen);
    free(param);
    return o;
}

/* ---- text ---- */

/* UTF-16 units of a skill-list section: its lines. lines[k] points into u (not ended), len[k] units */
typedef struct {
    uint16_t *u;
    int n;
    int start[512], len[512], lines;
} Text;

static int text_read(const unsigned char *s, long n, Text *t) {
    long k = s != NULL && n >= 2 && s[0] == 0xFF && s[1] == 0xFE ? 2 : 0;
    int i;
    memset(t, 0, sizeof(*t));
    if (s == NULL || n < 4) {
        return 0;
    }
    t->u = malloc((size_t)(n / 2 + 1) * 2);
    if (t->u == NULL) {
        return 0;
    }
    for (; k + 1 < n; k += 2) {
        uint16_t c = (uint16_t)(s[k] | s[k + 1] << 8);
        if (c == 0) {
            break;
        }
        t->u[t->n++] = c;
    }
    t->start[0] = 0;
    t->lines = 1;
    for (i = 0; i < t->n && t->lines < 512; i++) {
        if (t->u[i] == '\n') {
            t->len[t->lines - 1] = i - t->start[t->lines - 1];
            t->start[t->lines++] = i + 1;
        }
    }
    t->len[t->lines - 1] = t->n - t->start[t->lines - 1];
    return 1;
}

static int is_move(const Text *t, int l) {
    const uint16_t *p = t->u + t->start[l];
    return l < t->lines && t->len[l] >= 6 && p[0] == '!' && p[1] == 'F' && p[2] == '*';
}

static int utf8_put(const uint16_t *u, int n, char *out, int size) {
    int o = 0, i;
    for (i = 0; i < n; i++) {
        unsigned c = u[i];
        if (c == '\r') {
            continue;
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
    return o;
}

static int utf16_of(const char *s, uint16_t *out, int max) {
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max) {
        unsigned c = *p++;
        if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63);
            p += 2;
        } else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) {
            c = (c & 31) << 6 | (p[0] & 63);
            p++;
        } else if (c >= 0x80) {
            continue;
        }
        if (c == '\n' || c == '\r' || c == 0) {
            continue;
        }
        out[n++] = (uint16_t)c;
    }
    return n;
}

/* ---- the model being edited ---- */

typedef struct {
    int section, at, size;
    unsigned char bytes[4];
} Change;

typedef struct {
    int line;
    char cost[4];
    char name[96];
    int changed;
} Move;

struct CeModel {
    char dir[600];
    unsigned char *b;
    long n;
    Change ch[512];
    int nch;
    Move mv[128];
    int nmv;
};

static long value_at(const CeModel *m, int t, int row, int col, int *sec, int *at) {
    const CeTable *tb = Ce_Table(t);
    const CeCol *c;
    long a, z;
    if (tb == NULL || row < 0 || row >= tb->rows || col < 0 || col >= tb->ncols || tb->section > pack_count(m->b, m->n)) {
        return -1;
    }
    c = &tb->cols[col];
    a = sec_start(m->b, m->n, tb->section);
    z = sec_end(m->b, m->n, tb->section);
    *sec = tb->section;
    *at = row * tb->stride + c->off + row * c->step;
    return a + *at + type_size(c->type) <= z ? a + *at : -1;
}

static double decode(const unsigned char *p, int type) {
    float f;
    switch (type) {
    case CE_U8:
        return p[0];
    case CE_S8:
        return (signed char)p[0];
    case CE_U16:
        return (uint16_t)(p[0] | p[1] << 8);
    case CE_S16:
        return (int16_t)(p[0] | p[1] << 8);
    case CE_S32:
        return (int32_t)rd32(p);
    }
    {
        uint32_t u = rd32(p);
        memcpy(&f, &u, 4);
    }
    return f;
}

CeModel *Ce_Load(const char *dir, char *why, int whyLen) {
    CeModel *m;
    char path[700];
    int s;
    if (!find_file(dir, "_1p.pak", path, sizeof(path))) {
        snprintf(why, whyLen, "This package has no model of its own (it only changes a character of the disc).");
        return NULL;
    }
    if ((m = calloc(1, sizeof(CeModel))) == NULL) {
        return NULL;
    }
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
    if ((m->b = read_all(path, MODEL_MAX * 2, &m->n)) == NULL || pack_count(m->b, m->n) < 25) {
        snprintf(why, whyLen, "Its model cannot be read.");
        Ce_Free(m);
        return NULL;
    }
    for (s = TEXT_FIRST; s <= TEXT_LAST && m->nmv == 0 && s <= pack_count(m->b, m->n); s++) {
        Text t;
        int l;
        if (!text_read(m->b + sec_start(m->b, m->n, s), sec_end(m->b, m->n, s) - sec_start(m->b, m->n, s), &t)) {
            continue;
        }
        for (l = 0; l < t.lines && m->nmv < 128; l++) {
            if (is_move(&t, l)) {
                Move *v = &m->mv[m->nmv++];
                v->line = l;
                utf8_put(t.u + t.start[l] + 3, 3, v->cost, sizeof(v->cost));
                utf8_put(t.u + t.start[l] + 6, t.len[l] - 6, v->name, sizeof(v->name));
            }
        }
        free(t.u);
    }
    return m;
}

void Ce_Free(CeModel *m) {
    if (m != NULL) {
        free(m->b);
        free(m);
    }
}

int Ce_Get(const CeModel *m, int t, int row, int col, double *v) {
    int sec, at;
    long p = value_at(m, t, row, col, &sec, &at);
    if (p < 0) {
        return 0;
    }
    *v = decode(m->b + p, Ce_Table(t)->cols[col].type);
    return 1;
}

int Ce_Set(CeModel *m, int t, int row, int col, double v) {
    int sec, at, type, k, size;
    long p = value_at(m, t, row, col, &sec, &at);
    unsigned char bytes[4];
    if (p < 0) {
        return 0;
    }
    type = Ce_Table(t)->cols[col].type;
    size = type_size(type);
    if (type == CE_F32) {
        float f = (float)v;
        uint32_t u;
        if (!isfinite(f)) {
            return 0;
        }
        memcpy(&u, &f, 4);
        wr32(bytes, u);
    } else {
        static const double kLo[] = {0, -128, 0, -32768, -2147483648.0}, kHi[] = {255, 127, 65535, 32767, 2147483647.0};
        long long x = llround(v);
        if (x < kLo[type] || x > kHi[type]) {
            return 0;
        }
        wr32(bytes, (uint32_t)x);
    }
    memcpy(m->b + p, bytes, (size_t)size);
    for (k = 0; k < m->nch; k++) {
        if (m->ch[k].section == sec && m->ch[k].at == at && m->ch[k].size == size) {
            break;
        }
    }
    if (k == m->nch) {
        if (m->nch == 512) {
            return 0;
        }
        m->nch++;
    }
    m->ch[k].section = sec;
    m->ch[k].at = at;
    m->ch[k].size = size;
    memcpy(m->ch[k].bytes, bytes, 4);
    return 1;
}

int Ce_MoveCount(const CeModel *m) { return m != NULL ? m->nmv : 0; }

int Ce_Move(const CeModel *m, int i, char *cost, char *name, int nameLen) {
    if (m == NULL || i < 0 || i >= m->nmv) {
        return 0;
    }
    snprintf(cost, 4, "%s", m->mv[i].cost);
    snprintf(name, (size_t)nameLen, "%s", m->mv[i].name);
    return 1;
}

int Ce_MoveSet(CeModel *m, int i, const char *cost, const char *name) {
    char c[4] = "000";
    int l, k;
    if (m == NULL || i < 0 || i >= m->nmv) {
        return 0;
    }
    l = (int)strlen(cost);
    for (k = 0; k < 3 && k < l; k++) { /* (right-aligned, digits) */
        char d = cost[l - 1 - k];
        c[2 - k] = isdigit((unsigned char)d) ? d : '0';
    }
    snprintf(m->mv[i].cost, sizeof(m->mv[i].cost), "%s", c);
    snprintf(m->mv[i].name, sizeof(m->mv[i].name), "%s", name);
    m->mv[i].changed = 1;
    return 1;
}

int Ce_Dirty(const CeModel *m) {
    int i;
    if (m == NULL) {
        return 0;
    }
    for (i = 0; i < m->nmv; i++) {
        if (m->mv[i].changed) {
            return 1;
        }
    }
    return m->nch > 0;
}

/* section s of b with the edited move lines; NULL when nothing of it changes */
static unsigned char *renamed_text(const CeModel *m, const unsigned char *b, long n, int s, long *outLen) {
    Text t;
    int i, l, changed = 0;
    long o = 0;
    unsigned char *out;
    if (s > pack_count(b, n) || !text_read(b + sec_start(b, n, s), sec_end(b, n, s) - sec_start(b, n, s), &t)) {
        return NULL;
    }
    out = malloc((size_t)(t.n + 128 * 110) * 2 + 64);
    if (out == NULL) {
        free(t.u);
        return NULL;
    }
    out[o++] = 0xFF;
    out[o++] = 0xFE;
    for (l = 0; l < t.lines; l++) {
        const Move *v = NULL;
        uint16_t line[200];
        int ln = 0;
        for (i = 0; i < m->nmv; i++) {
            if (m->mv[i].changed && m->mv[i].line == l) {
                v = &m->mv[i];
            }
        }
        if (v != NULL && is_move(&t, l)) {
            int cr = t.len[l] > 0 && t.u[t.start[l] + t.len[l] - 1] == '\r';
            line[ln++] = '!';
            line[ln++] = 'F';
            line[ln++] = '*';
            for (i = 0; i < 3; i++) {
                line[ln++] = (uint16_t)v->cost[i];
            }
            ln += utf16_of(v->name, line + ln, 190 - ln);
            if (cr) {
                line[ln++] = '\r';
            }
            if (ln != t.len[l] || memcmp(line, t.u + t.start[l], (size_t)ln * 2) != 0) {
                changed = 1;
            }
        } else {
            ln = t.len[l] < 200 ? t.len[l] : 200;
            memcpy(line, t.u + t.start[l], (size_t)ln * 2);
            if (t.len[l] > 200) { /* (a line too long to have been edited: kept whole) */
                for (i = 0; i < t.len[l]; i++) {
                    out[o++] = (unsigned char)t.u[t.start[l] + i];
                    out[o++] = (unsigned char)(t.u[t.start[l] + i] >> 8);
                }
                ln = 0;
            }
        }
        for (i = 0; i < ln; i++) {
            out[o++] = (unsigned char)line[i];
            out[o++] = (unsigned char)(line[i] >> 8);
        }
        if (l + 1 < t.lines) {
            out[o++] = '\n';
            out[o++] = 0;
        }
    }
    out[o++] = 0;
    out[o++] = 0;
    while (o % 16 != 0) {
        out[o++] = 0;
    }
    free(t.u);
    if (!changed) {
        free(out);
        return NULL;
    }
    *outLen = o;
    return out;
}

int Ce_Save(CeModel *m, char *why, int whyLen) {
    int nf, f, k, renames = 0;
    char (*list)[128] = costumes(m->dir, &nf);
    char names[16][128];
    for (k = 0; k < m->nmv; k++) {
        renames |= m->mv[k].changed;
    }
    memcpy(names, list, sizeof(names));
    why[0] = '\0';
    for (f = 0; f < nf; f++) {
        char path[720];
        long n, outLen = 0;
        unsigned char *b, *out;
        snprintf(path, sizeof(path), "%s/%s", m->dir, names[f]);
        if ((b = read_all(path, MODEL_MAX * 2, &n)) == NULL || pack_count(b, n) < 25) {
            free(b);
            snprintf(why, whyLen, "%s cannot be read.", names[f]);
            return 0;
        }
        for (k = 0; k < m->nch; k++) {
            const Change *c = &m->ch[k];
            long a = sec_start(b, n, c->section);
            if (c->section > pack_count(b, n) || a + c->at + c->size > sec_end(b, n, c->section)) {
                snprintf(why, whyLen, "%s has no room for that value (section %d).", names[f], c->section);
                free(b);
                return 0;
            }
            memcpy(b + a + c->at, c->bytes, (size_t)c->size);
        }
        out = b;
        outLen = n;
        if (renames) {
            static Span repl[256];
            unsigned char *texts[TEXT_LAST + 1] = {0};
            int s, any = 0;
            memset(repl, 0, sizeof(repl));
            for (s = TEXT_FIRST; s <= TEXT_LAST; s++) {
                long l;
                if ((texts[s] = renamed_text(m, b, n, s, &l)) != NULL) {
                    repl[s].b = texts[s];
                    repl[s].n = l;
                    any = 1;
                }
            }
            if (any) {
                out = rebuild(b, n, repl, &outLen);
            }
            for (s = TEXT_FIRST; s <= TEXT_LAST; s++) {
                free(texts[s]);
            }
            if (out == NULL) {
                free(b);
                snprintf(why, whyLen, "%s could not be laid out again.", names[f]);
                return 0;
            }
        }
        if (outLen > MODEL_MAX) {
            snprintf(why, whyLen, "%s would be larger than the game takes.", names[f]);
            if (out != b) {
                free(out);
            }
            free(b);
            return 0;
        }
        keep_original(path);
        k = write_all(path, out, outLen);
        if (out != b) {
            free(out);
        }
        free(b);
        if (!k) {
            snprintf(why, whyLen, "%s could not be written.", names[f]);
            return 0;
        }
    }
    m->nch = 0;
    for (k = 0; k < m->nmv; k++) {
        m->mv[k].changed = 0;
    }
    return 1;
}

/* ---- movesets ---- */

static int disc_path(const char *gamedata, int id, char *out, int size) {
    snprintf(out, size, "%s/%s/%05d.bin", gamedata, id < 0xD48 ? "pzs3us1" : "pzs3us2", id < 0xD48 ? id - 1 : id - 0xD48);
    return size_of(out) > 0;
}

int Ce_TakeMoves(const char *dir, const char *donorDir, int discId, const char *gamedata, char *why, int whyLen) {
    return Ce_TakeParts(dir, donorDir, discId, gamedata, CE_PART_ALL, why, whyLen);
}

/* As Ce_TakeMoves, but only the CE_PART_* in `parts` come from the donor; the rest of the character's own stays. */
int Ce_TakeParts(const char *dir, const char *donorDir, int discId, const char *gamedata, int parts, char *why, int whyLen) {
    char mp[700], ap[700], ep[700];
    unsigned char *model, *anm, *eff, *outs[16];
    long mn, an, en, outLen[16];
    int nf, f, ok = 1;
    char (*list)[128] = costumes(dir, &nf), names[16][128];
    memcpy(names, list, sizeof(names));
    if (nf == 0) {
        snprintf(why, whyLen, "This character has no model of its own.");
        return 0;
    }
    if (donorDir != NULL) {
        if (!find_file(donorDir, "_1p.pak", mp, sizeof(mp)) || !find_file(donorDir, "_anm.pak", ap, sizeof(ap)) ||
            !find_file(donorDir, "_eff.pak", ep, sizeof(ep))) {
            snprintf(why, whyLen, "That character has no model, animations or effects of its own.");
            return 0;
        }
    } else if (!disc_path(gamedata, 0x590 + discId * 10, mp, sizeof(mp)) || !disc_path(gamedata, 0x590 + discId * 10 + 8, ap, sizeof(ap)) ||
               !disc_path(gamedata, 0x590 + discId * 10 + 9, ep, sizeof(ep))) {
        snprintf(why, whyLen, "That character's files are not in the game's data.");
        return 0;
    }
    model = read_all(mp, MODEL_MAX * 2, &mn);
    anm = read_all(ap, ANM_MAX, &an);
    eff = read_all(ep, EFF_MAX, &en);
    if (model == NULL || anm == NULL || eff == NULL) {
        snprintf(why, whyLen, "The donor's files cannot be read, or are larger than the game takes.");
        ok = 0;
    }
    memset(outs, 0, sizeof(outs));
    for (f = 0; ok && f < nf; f++) {
        char path[720], orig[730];
        long n;
        unsigned char *own;
        snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
        snprintf(orig, sizeof(orig), "%s.orig", path);
        own = read_all(size_of(orig) > 0 ? orig : path, MODEL_MAX * 2, &n);
        if (own == NULL || (outs[f] = transplant(own, n, model, mn, parts, &outLen[f])) == NULL) {
            snprintf(why, whyLen, "%s and the donor's model have different layouts.", names[f]);
            ok = 0;
        } else if (outLen[f] > MODEL_MAX) {
            snprintf(why, whyLen, "%s would be larger than the game takes.", names[f]);
            ok = 0;
        }
        free(own);
    }
    for (f = 0; ok && f < nf; f++) {
        char path[720];
        snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
        keep_original(path);
        ok &= write_all(path, outs[f], outLen[f]);
    }
    if (ok && (parts & CE_PART_ANIM)) {
        char a[700], e[700];
        if (!find_file(dir, "_anm.pak", a, sizeof(a))) {
            snprintf(a, sizeof(a), "%s/m_anm.pak", dir);
        }
        if (!find_file(dir, "_eff.pak", e, sizeof(e))) {
            snprintf(e, sizeof(e), "%s/m_eff.pak", dir);
        }
        keep_original(a);
        keep_original(e);
        ok = write_all(a, anm, an) && write_all(e, eff, en);
        if (!ok) {
            snprintf(why, whyLen, "The animations could not be written.");
        }
    }
    for (f = 0; f < nf; f++) {
        free(outs[f]);
    }
    free(model);
    free(anm);
    free(eff);
    return ok;
}

/* ---- one move ----
   Sections 24 (the techniques of slots 2..4: BtlSuperData, 0x244 bytes, 3 slots) and 25 (the skills of slots 0..1:
   BtlSkillData, 0xC8 bytes, 2 slots) are structures of arrays: each field is an array with one element per slot, laid
   one after the other, so a slot's data is one element out of each array. A segment is a run of `groups` arrays of
   `elem`-byte elements (include/battle/btl_tech_b.h gives every field; what the header leaves unnamed is read as
   arrays of the same width, which its offsets and sizes fit exactly). */
typedef struct {
    int start, elem, groups;
} Seg;

static const Seg kSuperSegs[] = {{0x000, 4, 2}, {0x018, 2, 2}, {0x024, 4, 4},  {0x054, 4, 5},  {0x090, 1, 57},
                                 {0x13B, 1, 16}, {0x16C, 4, 13}, {0x208, 2, 4}, {0x220, 1, 4}, {0x22C, 4, 2}};
static const Seg kSkillSegs[] = {{0x00, 4, 2}, {0x10, 2, 2}, {0x18, 4, 4}, {0x38, 1, 30}, {0x74, 4, 4}, {0x94, 1, 6}, {0xA0, 4, 5}};

static void copy_slot(unsigned char *own, const unsigned char *don, const Seg *segs, int nseg, int slots, int slot, int srcSlot) {
    int s, g;
    for (s = 0; s < nseg; s++) {
        for (g = 0; g < segs[s].groups; g++) {
            int base = segs[s].start + g * slots * segs[s].elem;
            memcpy(own + base + slot * segs[s].elem, don + base + srcSlot * segs[s].elem, (size_t)segs[s].elem);
        }
    }
}

/* The names of the five moves of a character, as its skill list gives them: the first five moves before the list's second
   heading (Blast 1 A, B, Blast 2 A, B, Ultimate: 155 of 155 readable characters of the disc have exactly these). `file` is
   a model file; names is 5 strings of 48 bytes, "" for a move the list does not have. 1 when a list was found. */
int Ce_SlotNames(const char *file, char names[5][48]) {
    long n;
    unsigned char *b = read_all(file, MODEL_MAX * 2, &n);
    int s, found = 0, k;
    for (k = 0; k < 5; k++) {
        names[k][0] = '\0';
    }
    for (s = TEXT_FIRST; b != NULL && !found && s <= TEXT_LAST && s <= pack_count(b, n); s++) {
        Text t;
        int l, heads = 0, got = 0;
        if (!text_read(b + sec_start(b, n, s), sec_end(b, n, s) - sec_start(b, n, s), &t)) {
            continue;
        }
        for (l = 0; l < t.lines && got < 5; l++) {
            const uint16_t *p = t.u + t.start[l];
            if (t.len[l] >= 3 && p[0] == '!' && p[1] == 'F' && p[2] == '$' && ++heads > 1) {
                break;
            }
            if (is_move(&t, l)) {
                utf8_put(p + 6, t.len[l] - 6, names[got++], 48);
                found = 1;
            }
        }
        free(t.u);
    }
    free(b);
    return found;
}

/* The five moves' names of a package folder (dir) or, when dir is NULL, of the disc's character `discId`. */
int Ce_SlotNamesOf(const char *dir, int discId, const char *gamedata, char names[5][48]) {
    char path[700];
    int k;
    for (k = 0; k < 5; k++) {
        names[k][0] = '\0';
    }
    if (dir != NULL ? !find_file(dir, "_1p.pak", path, sizeof(path)) : !disc_path(gamedata, 0x590 + discId * 10, path, sizeof(path))) {
        return 0;
    }
    return Ce_SlotNames(path, names);
}

/* One move of the donor in place of the character's own: move 0, 1 = Blast 1 (A, B), 2, 3 = Blast 2 (A, B), 4 = Ultimate.
   It is the move's numbers (every costume's model) and its animations (the animation file: 0x103 + move for a skill; for a
   technique its block of 0x22 starting at 0x105 + (slot - 2) * 0x22, and the Ultimate's pose, 0x16B). The effects (the
   beam's and ball's pictures) are not part of this: they stay the character's own. Starts from the files as they are now,
   so moves taken one after another add up; Ce_Restore still puts the first files back. */
int Ce_TakeMove(const char *dir, const char *donorDir, int discId, const char *gamedata, int move, int srcMove, char *why, int whyLen) {
    char mp[700], ap[700], oa[700];
    unsigned char *model = NULL, *anm = NULL, *own = NULL, *newAnm = NULL, *outs[16];
    long mn = 0, an = 0, on = 0, outLen[16], newLen = 0;
    int nf, f, ok = 1, sec = move < 2 ? 25 : 24, slots = move < 2 ? 2 : 3, slot = move < 2 ? move : move - 2;
    long need = move < 2 ? 0xC8 : 0x244;
    int srcSlot = srcMove < 2 ? srcMove : srcMove - 2;
    int first = move < 2 ? 0x103 + move : 0x105 + slot * 0x22, srcFirst = srcMove < 2 ? 0x103 + srcMove : 0x105 + srcSlot * 0x22;
    int count = move < 2 ? 1 : 0x22 + (move == 4 && srcMove == 4 ? 1 : 0), i;
    char (*list)[128] = costumes(dir, &nf), names[16][128];
    static Span repl[1024];
    memcpy(names, list, sizeof(names));
    memset(outs, 0, sizeof(outs));
    if (move < 0 || move > 4 || srcMove < 0 || srcMove > 4 || (move < 2) != (srcMove < 2)) {
        snprintf(why, whyLen, "A Blast 1 move can only be taken in place of a Blast 1 move; Blast 2 and Ultimate likewise.");
        return 0;
    }
    if (nf == 0) {
        snprintf(why, whyLen, "This character has no model of its own.");
        return 0;
    }
    if (donorDir != NULL) {
        if (!find_file(donorDir, "_1p.pak", mp, sizeof(mp)) || !find_file(donorDir, "_anm.pak", ap, sizeof(ap))) {
            snprintf(why, whyLen, "That character has no model or animations of its own.");
            return 0;
        }
    } else if (!disc_path(gamedata, 0x590 + discId * 10, mp, sizeof(mp)) || !disc_path(gamedata, 0x590 + discId * 10 + 8, ap, sizeof(ap))) {
        snprintf(why, whyLen, "That character's files are not in the game's data.");
        return 0;
    }
    model = read_all(mp, MODEL_MAX * 2, &mn);
    anm = read_all(ap, ANM_MAX, &an);
    if (model == NULL || anm == NULL || pack_count(model, mn) < sec || sec_end(model, mn, sec) - sec_start(model, mn, sec) < need) {
        snprintf(why, whyLen, "The donor's files cannot be read, or have another layout.");
        ok = 0;
    }
    for (f = 0; ok && f < nf; f++) {
        char path[720];
        long n, a;
        snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
        outs[f] = read_all(path, MODEL_MAX * 2, &n);
        a = outs[f] != NULL && pack_count(outs[f], n) >= sec ? sec_start(outs[f], n, sec) : 0;
        if (a == 0 || sec_end(outs[f], n, sec) - a < need) {
            snprintf(why, whyLen, "%s has another layout than the donor's.", names[f]);
            ok = 0;
            break;
        }
        copy_slot(outs[f] + a, model + sec_start(model, mn, sec), move < 2 ? kSkillSegs : kSuperSegs, move < 2 ? 7 : 10, slots, slot, srcSlot);
        outLen[f] = n;
    }
    if (ok) {
        if (!find_file(dir, "_anm.pak", oa, sizeof(oa))) {
            snprintf(oa, sizeof(oa), "%s/m_anm.pak", dir);
        }
        own = read_all(oa, ANM_MAX, &on);
        if (own == NULL || pack_count(own, on) < first + count || pack_count(own, on) != pack_count(anm, an) || pack_count(own, on) >= 1024) {
            snprintf(why, whyLen, "Its animations and the donor's have different layouts.");
            ok = 0;
        }
    }
    if (ok) {
        memset(repl, 0, sizeof(repl));
        for (i = 1; i <= count; i++) { /* (entry n of the file is animation n - 1; the donor's block goes in place of its own) */
            long a = sec_start(anm, an, srcFirst + i);
            repl[first + i].b = anm + a;
            repl[first + i].n = sec_end(anm, an, srcFirst + i) - a;
        }
        if ((newAnm = rebuild(own, on, repl, &newLen)) == NULL || newLen > ANM_MAX) {
            snprintf(why, whyLen, "The animations would be larger than the game takes.");
            ok = 0;
        }
    }
    for (f = 0; ok && f < nf; f++) {
        char path[720];
        snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
        keep_original(path);
        ok &= write_all(path, outs[f], outLen[f]);
    }
    if (ok) {
        keep_original(oa);
        ok = write_all(oa, newAnm, newLen);
        if (!ok) {
            snprintf(why, whyLen, "The animations could not be written.");
        }
    }
    for (f = 0; f < 16; f++) {
        free(outs[f]);
    }
    free(model);
    free(anm);
    free(own);
    free(newAnm);
    return ok;
}

int Ce_HasBackup(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int yes = 0;
    if (d == NULL) {
        return 0;
    }
    while (!yes && (de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        yes = l > 5 && strcasecmp(de->d_name + l - 5, ".orig") == 0;
    }
    closedir(d);
    return yes;
}

int Ce_Restore(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    char names[64][128];
    int n = 0, i;
    if (d == NULL) {
        return 0;
    }
    while ((de = readdir(d)) != NULL && n < 64) {
        size_t l = strlen(de->d_name);
        if (l > 5 && l < 128 && strcasecmp(de->d_name + l - 5, ".orig") == 0) {
            snprintf(names[n++], 128, "%s", de->d_name);
        }
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        char orig[720], file[720];
        snprintf(orig, sizeof(orig), "%s/%s", dir, names[i]);
        snprintf(file, sizeof(file), "%s/%.*s", dir, (int)strlen(names[i]) - 5, names[i]);
        remove(file);
        if (size_of(orig) == 0) {
            remove(orig);
        } else {
            rename(orig, file);
        }
    }
    return n;
}

static int copy_file(const char *from, const char *to) {
    long n;
    unsigned char *b = read_all(from, 64L << 20, &n);
    int ok = b != NULL && write_all(to, b, n);
    free(b);
    return ok;
}

int Ce_BringDisc(const char *dir, int discId, const char *gamedata) {
    int b0 = 0x590 + discId * 10, k, made = 0;
    char from[700], to[720];
    for (k = 0; k < 4; k++) {
        if (!disc_path(gamedata, b0 + k, from, sizeof(from)) || size_of(from) <= 4096) {
            break;
        }
        snprintf(to, sizeof(to), "%s/m_%dp.pak", dir, k + 1);
        made += copy_file(from, to);
        if (disc_path(gamedata, b0 + 4 + k, from, sizeof(from)) && size_of(from) > 4096) {
            snprintf(to, sizeof(to), "%s/m_%dp_dmg.pak", dir, k + 1);
            made += copy_file(from, to);
        }
    }
    if (disc_path(gamedata, b0 + 8, from, sizeof(from))) {
        snprintf(to, sizeof(to), "%s/m_anm.pak", dir);
        made += copy_file(from, to);
    }
    if (disc_path(gamedata, b0 + 9, from, sizeof(from))) {
        snprintf(to, sizeof(to), "%s/m_eff.pak", dir);
        made += copy_file(from, to);
    }
    return made;
}
