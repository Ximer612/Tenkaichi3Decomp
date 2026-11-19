/* A 3D model file made into a fighter's model (port/include/plat_modelimport.h).
 *
 * What a fighter's model is (model file section 3, "pmdl"; see src/sys/gfxm_d_c.c and gfxm_e_c.c):
 *   - a chain of node records, parent first (`pop` closes branches). +0x10 the node's place in the rest pose,
 *     +0x20 its parent's, +0x30 the difference. A record longer than 0x40 carries a mesh: +0x40 centre,
 *     +0x50 half sizes and radius, +0x60 a VIF chain.
 *   - a mesh is drawn with TWO matrices only: its node's and that node's parent's, mixed by the weight in each
 *     vertex's w. So a vertex is "bone b, share w; the rest its parent", and a triangle can only hold vertices of
 *     one node and vertices rigid to that node's parent. rig_fix() makes every triangle fit that.
 *   - a batch is one triangle strip of at most 40 vertices: 5 quadwords (two TEX0, two primitive tags), then 3 per
 *     vertex (position + weight, normal, s t 1), then MSCNT.
 *   - textures (section 12) are 4 bits with 16 colours each, stored as the 32-bit upload that gives those nibbles.
 * The skeleton, the tables and every other section stay the base fighter's. Space: Y down, front -Z, +X the left.
 *
 * Read with ufbx (FBX, OBJ), cgltf (glTF, GLB), a reader of its own (COLLADA) and stb_image (the pictures).
 */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/plat_modelimport.h"
#include "../../third_party/ufbx/ufbx.h"
#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#include "../../third_party/stb/stb_image.h"

#define MODEL_MAX 0x180000L /* the game's model buffer */
#define MI_LOAD 64          /* materials read from a file, before they are packed into pages */
#define MI_MATS 12          /* a texture has one of 16 alpha slots; the face and spare ones keep theirs */
#define MI_STRIP 36         /* vertices in a batch (the game's own models: 40 at most) */
#define MI_TRIS 7000        /* ponytail: triangle budget before the size check; a larger model is thinned */
#define NODES 256

typedef float V3[3];
typedef struct { V3 p, n; float uv[2]; int jb[4]; float jw[4]; float vc[4]; /* vertex colour r g b, and 1 when the file has one */ } Vtx;
typedef struct { char name[64]; int parent; V3 pos; int node; } Joint;
typedef struct { unsigned char *px; int w, h; float col[3]; int used; } Mat;
typedef struct {
    Vtx *v; int nv, cv;
    int *tri, *tm; int nt, ct; /* tri: 3 vertex indices each; tm: material */
    Mat mat[MI_LOAD]; int nmat;
    const void *matKey[MI_LOAD];
    Joint *j; int nj; int paged;
    char miss[160]; /* the pictures the file names but that were not found, ", " between them */
} Scene;

/* ---- small maths ---- */
static void v_set(V3 o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void v_cp(V3 o, const V3 a) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }
static void v_sub(V3 o, const V3 a, const V3 b) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void v_mad(V3 o, const V3 a, const V3 b, float s) { o[0] = a[0] + b[0] * s; o[1] = a[1] + b[1] * s; o[2] = a[2] + b[2] * s; }
static float v_dot(const V3 a, const V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v_len(const V3 a) { return sqrtf(v_dot(a, a)); }
static int v_norm(V3 a) {
    float l = v_len(a);
    if (l < 1e-9f) { return 0; }
    a[0] /= l; a[1] /= l; a[2] /= l;
    return 1;
}
/* file space (Y up, front +Z) to the game's (Y down, front -Z): half a turn about X */
static void v_game(V3 o, const V3 a) { o[0] = a[0]; o[1] = -a[1]; o[2] = -a[2]; }
static void m_id(float m[9]) { memset(m, 0, 36); m[0] = m[4] = m[8] = 1; }
static void m_mul(V3 o, const float m[9], const V3 a) {
    V3 t;
    t[0] = m[0] * a[0] + m[1] * a[1] + m[2] * a[2];
    t[1] = m[3] * a[0] + m[4] * a[1] + m[5] * a[2];
    t[2] = m[6] * a[0] + m[7] * a[1] + m[8] * a[2];
    v_cp(o, t);
}
/* the smallest turn that takes unit a to unit b */
static void m_between(float m[9], const V3 a, const V3 b) {
    V3 v = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    float c = v_dot(a, b), k;
    if (c < -0.9999f) { /* opposite: half a turn about any axis across a */
        V3 x = {fabsf(a[0]) < 0.9f ? 1.0f : 0.0f, fabsf(a[0]) < 0.9f ? 0.0f : 1.0f, 0};
        V3 ax = {a[1] * x[2] - a[2] * x[1], a[2] * x[0] - a[0] * x[2], a[0] * x[1] - a[1] * x[0]};
        int i, j;
        v_norm(ax);
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) m[i * 3 + j] = 2 * ax[i] * ax[j] - (i == j);
        return;
    }
    k = 1.0f / (1.0f + c);
    m[0] = c + k * v[0] * v[0];    m[1] = k * v[0] * v[1] - v[2]; m[2] = k * v[0] * v[2] + v[1];
    m[3] = k * v[0] * v[1] + v[2]; m[4] = c + k * v[1] * v[1];    m[5] = k * v[1] * v[2] - v[0];
    m[6] = k * v[0] * v[2] - v[1]; m[7] = k * v[1] * v[2] + v[0]; m[8] = c + k * v[2] * v[2];
}

static uint32_t rd32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void wr32(unsigned char *b, uint32_t v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24); }
static float rdf(const unsigned char *b) { uint32_t u = rd32(b); float f; memcpy(&f, &u, 4); return f; }
static void wrf(unsigned char *b, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(b, u); }

/* a growing byte buffer */
typedef struct { unsigned char *b; long n, cap; } Buf;
static unsigned char *buf_add(Buf *o, long n) {
    if (o->n + n > o->cap) {
        o->cap = (o->n + n) * 2 + 4096;
        o->b = realloc(o->b, (size_t)o->cap);
    }
    memset(o->b + o->n, 0, (size_t)n);
    o->n += n;
    return o->b + o->n - n;
}
static void buf_w(Buf *o, uint32_t v) { wr32(buf_add(o, 4), v); }
static void buf_f(Buf *o, float f) { wrf(buf_add(o, 4), f); }

static unsigned char *read_all(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    long size;
    if (f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < (256L << 20) && (b = malloc((size_t)size)) != NULL && fread(b, 1, (size_t)size, f) != (size_t)size) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = size;
    return b;
}

/* ---- the scene ---- */
static int ends_with(const char *s, const char *e) {
    size_t a = strlen(s), b = strlen(e), i;
    if (a < b) { return 0; }
    for (i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != e[i]) { return 0; }
    return 1;
}

int Mi_IsModel(const char *path) {
    return path != NULL && (ends_with(path, ".fbx") || ends_with(path, ".obj") || ends_with(path, ".glb") || ends_with(path, ".gltf") || ends_with(path, ".dae"));
}

static void scene_tri(Scene *s, const Vtx *a, int mat) {
    if (s->nv + 3 > s->cv) {
        s->cv = s->cv * 2 + 3072;
        s->v = realloc(s->v, sizeof(Vtx) * (size_t)s->cv);
    }
    if (s->nt + 1 > s->ct) {
        s->ct = s->ct * 2 + 1024;
        s->tri = realloc(s->tri, sizeof(int) * 3 * (size_t)s->ct);
        s->tm = realloc(s->tm, sizeof(int) * (size_t)s->ct);
    }
    memcpy(&s->v[s->nv], a, sizeof(Vtx) * 3);
    s->tri[s->nt * 3] = s->nv; s->tri[s->nt * 3 + 1] = s->nv + 1; s->tri[s->nt * 3 + 2] = s->nv + 2;
    s->tm[s->nt++] = mat;
    s->nv += 3;
}

/* A DDS picture (stb_image does not read them): DXT1, DXT3, DXT5, or plain 24 / 32 bits. Its first image as RGBA
   from malloc, NULL when it is none of those. */
static unsigned char *dds_decode(const unsigned char *d, size_t n, int *w, int *h) {
    uint32_t fourcc, bits, pf;
    int W, H, x, y, i, dxt;
    unsigned char *o;
    const unsigned char *p;
    if (n < 128 || memcmp(d, "DDS ", 4) != 0) { return NULL; }
    H = (int)rd32(d + 12); W = (int)rd32(d + 16);
    pf = rd32(d + 80); fourcc = rd32(d + 84); bits = rd32(d + 88);
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { return NULL; }
    dxt = !(pf & 4) ? 0 : fourcc == 0x31545844 ? 1 : fourcc == 0x33545844 ? 3 : fourcc == 0x35545844 ? 5 : -1;
    p = d + 128;
    if (dxt < 0 || (dxt == 0 && bits != 24 && bits != 32)) { return NULL; }
    if (dxt == 0 ? (size_t)W * H * (bits / 8) > n - 128 : (size_t)((W + 3) / 4) * ((H + 3) / 4) * (dxt == 1 ? 8 : 16) > n - 128) { return NULL; }
    if ((o = malloc((size_t)W * H * 4)) == NULL) { return NULL; }
    if (dxt == 0) { /* B G R (A), masks as Direct3D's usual */
        for (i = 0; i < W * H; i++, p += bits / 8) {
            o[i * 4] = p[2]; o[i * 4 + 1] = p[1]; o[i * 4 + 2] = p[0]; o[i * 4 + 3] = bits == 32 ? p[3] : 255;
        }
    } else {
        for (y = 0; y < H; y += 4) for (x = 0; x < W; x += 4) {
            unsigned char c[4][3];
            unsigned c0, c1;
            uint32_t idx;
            if (dxt != 1) { p += 8; } /* (the alpha block: not kept, the game's textures have none) */
            c0 = p[0] | p[1] << 8; c1 = p[2] | p[3] << 8;
            idx = rd32(p + 4);
            p += 8;
            c[0][0] = (unsigned char)((c0 >> 11) * 255 / 31); c[0][1] = (unsigned char)(((c0 >> 5) & 63) * 255 / 63); c[0][2] = (unsigned char)((c0 & 31) * 255 / 31);
            c[1][0] = (unsigned char)((c1 >> 11) * 255 / 31); c[1][1] = (unsigned char)(((c1 >> 5) & 63) * 255 / 63); c[1][2] = (unsigned char)((c1 & 31) * 255 / 31);
            for (i = 0; i < 3; i++) {
                if (c0 > c1 || dxt != 1) {
                    c[2][i] = (unsigned char)((2 * c[0][i] + c[1][i]) / 3); c[3][i] = (unsigned char)((c[0][i] + 2 * c[1][i]) / 3);
                } else {
                    c[2][i] = (unsigned char)((c[0][i] + c[1][i]) / 2); c[3][i] = 0;
                }
            }
            for (i = 0; i < 16; i++) {
                int px = x + (i & 3), py = y + (i >> 2);
                if (px < W && py < H) {
                    memcpy(o + (py * W + px) * 4, c[(idx >> (i * 2)) & 3], 3);
                    o[(py * W + px) * 4 + 3] = 255;
                }
            }
        }
    }
    *w = W; *h = H;
    return o;
}

static const char *base_of(const char *path);

/* the material's place; its picture from memory or a file, else its colour */
static int scene_mat(Scene *s, const void *key, const unsigned char *mem, size_t memLen, const char *const *files, const float *col) {
    int i, c;
    Mat *m;
    for (i = 0; i < s->nmat; i++) if (s->matKey[i] == key) { return i; }
    if (s->nmat == MI_LOAD) { return MI_LOAD - 1; } /* ponytail: the 65th material and later share the 64th's texture */
    m = &s->mat[s->nmat];
    s->matKey[s->nmat] = key;
    m->col[0] = col[0]; m->col[1] = col[1]; m->col[2] = col[2];
    if (mem != NULL && memLen > 0) {
        m->px = stbi_load_from_memory(mem, (int)memLen, &m->w, &m->h, &c, 4);
        if (m->px == NULL) { m->px = dds_decode(mem, memLen, &m->w, &m->h); }
    }
    for (i = 0; m->px == NULL && files != NULL && files[i] != NULL; i++) {
        static const char *const kExt[] = {".png", ".jpg", ".tga", ".bmp"};
        char alt[900];
        const char *dot = strrchr(files[i], '.');
        int e;
        if (files[i][0] == '\0') { continue; }
        if (dot == NULL || dot < base_of(files[i])) { dot = files[i] + strlen(files[i]); } /* (a name with no extension, as Blender writes a packed picture: "Image_0") */
        m->px = stbi_load(files[i], &m->w, &m->h, &c, 4);
        if (m->px == NULL) {
            long dn = 0;
            unsigned char *dd = read_all(files[i], &dn);
            if (dd != NULL) { m->px = dds_decode(dd, (size_t)dn, &m->w, &m->h); }
            free(dd);
        }
        for (e = 0; m->px == NULL && e < 4; e++) { /* (a format this does not read, DDS, or no extension: the same name as a picture it does) */
            snprintf(alt, sizeof(alt), "%.*s%s", (int)(dot - files[i]), files[i], kExt[e]);
            m->px = stbi_load(alt, &m->w, &m->h, &c, 4);
        }
    }
    if (m->px == NULL && files != NULL) { /* a picture named but found nowhere: the owner is told its name, to put it beside the model */
        for (i = 0; files[i] != NULL && files[i][0] == '\0'; i++) {}
        if (files[i] != NULL && strstr(s->miss, base_of(files[i])) == NULL) {
            size_t n = strlen(s->miss);
            snprintf(s->miss + n, sizeof(s->miss) - n, "%s%s", n > 0 ? ", " : "", base_of(files[i]));
        }
    }
    return s->nmat++;
}

static void scene_free(Scene *s) {
    int i;
    for (i = 0; i < s->nmat; i++) { stbi_image_free(s->mat[i].px); }
    free(s->v); free(s->tri); free(s->tm); free(s->j);
    memset(s, 0, sizeof(*s));
}

/* The game's vertices carry no colour, so a file's vertex colours (console rips: a grey tunic picture and a green vertex colour) are
   folded into the picture: each material's picture is multiplied by the mean vertex colour of its triangles. */
static void scene_tint(Scene *s) {
    double sum[MI_LOAD][4];
    int i, c, k;
    memset(sum, 0, sizeof(sum));
    for (i = 0; i < s->nt; i++) for (c = 0; c < 3; c++) {
        const float *vc = s->v[s->tri[i * 3 + c]].vc;
        if (vc[3] > 0 && s->tm[i] < MI_LOAD) { for (k = 0; k < 3; k++) { sum[s->tm[i]][k] += vc[k]; } sum[s->tm[i]][3] += 1; }
    }
    for (i = 0; i < s->nmat; i++) {
        float m[3];
        long p;
        if (sum[i][3] < 1) { continue; }
        for (k = 0; k < 3; k++) { m[k] = (float)(sum[i][k] / sum[i][3]); m[k] = m[k] < 0 ? 0 : m[k] > 1 ? 1 : m[k]; }
        if (m[0] > 0.97f && m[1] > 0.97f && m[2] > 0.97f) { continue; }
        for (k = 0; k < 3; k++) { s->mat[i].col[k] *= m[k]; }
        if (s->mat[i].px != NULL) {
            for (p = 0; p < (long)s->mat[i].w * s->mat[i].h; p++) { for (k = 0; k < 3; k++) { s->mat[i].px[p * 4 + k] = (unsigned char)(s->mat[i].px[p * 4 + k] * m[k]); } }
        }
    }
}

/* More materials than the model has texture slots (a console rip has one per little picture: Link has 29): the pictures are
   packed side by side into a few pages (the smallest page size that holds each picture as it is, at most MI_MATS pages), and
   every triangle's texture place moves into its picture's rectangle. A place outside 0..1 (a tiling picture) is taken
   modulo the unit square of the triangle's first corner and held inside the rectangle. Done before vertices are welded, while
   every triangle still has corners of its own. */
typedef struct { int w, h, x, y, page; } AtRect;
static int at_cmp(const void *a, const void *b) {
    const AtRect *x = a, *y = b;
    return y->h != x->h ? y->h - x->h : y->w - x->w;
}
static void scene_atlas(Scene *s) {
    static const int kSide[3] = {64, 128, 256};
    AtRect r[MI_LOAD];
    int order[MI_LOAD], n = s->nmat, side, i, j, pages = 0, tried;
    if (n <= MI_MATS) { return; }
    for (i = 0; i < n; i++) { /* (a colour with no picture is a 4 x 4 one) */
        Mat *m = &s->mat[i];
        if (m->px == NULL) {
            m->w = m->h = 4;
            m->px = malloc(64);
            for (j = 0; j < 16; j++) {
                int c;
                for (c = 0; c < 3; c++) { float f = m->col[c] * 255; m->px[j * 4 + c] = (unsigned char)(f < 0 ? 0 : f > 255 ? 255 : f); }
                m->px[j * 4 + 3] = 255;
            }
        }
    }
    for (tried = 0; tried < 3; tried++) {
        int cx = 0, cy = 0, shelf = 0;
        side = kSide[tried];
        pages = 1;
        for (i = 0; i < n; i++) { r[i].w = s->mat[i].w; r[i].h = s->mat[i].h; r[i].x = i; r[i].y = 0; r[i].page = 0; }
        for (i = 0; i < n; i++) { /* (a picture bigger than a page is halved until it fits) */
            while (r[i].w + 2 > side || r[i].h + 2 > side) { r[i].w = (r[i].w + 1) / 2; r[i].h = (r[i].h + 1) / 2; }
            order[i] = i;
        }
        for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) { /* tallest first */
            AtRect *a = &r[order[i]], *b = &r[order[j]];
            if (b->h > a->h || (b->h == a->h && b->w > a->w)) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
        for (i = 0; i < n; i++) {
            AtRect *a = &r[order[i]];
            int pw = a->w + 2, ph = a->h + 2;
            if (cx + pw > side) { cx = 0; cy += shelf; shelf = 0; }
            if (cy + ph > side) { pages++; cx = 0; cy = 0; shelf = 0; }
            a->x = cx + 1; a->y = cy + 1; a->page = pages - 1;
            cx += pw;
            if (ph > shelf) { shelf = ph; }
        }
        if (pages <= MI_MATS) { break; }
    }
    if (pages > MI_MATS) { return; } /* (still too many: the old sharing of the last slot) */
    {
        Mat out[MI_MATS];
        int p, x, y, c;
        memset(out, 0, sizeof(out));
        for (p = 0; p < pages; p++) {
            out[p].w = out[p].h = side;
            out[p].px = calloc((size_t)side * (size_t)side, 4);
            out[p].col[0] = out[p].col[1] = out[p].col[2] = 0.5f;
        }
        for (i = 0; i < n; i++) { /* the picture, scaled down when it was halved, with its edge repeated one texel round it */
            const Mat *m = &s->mat[i];
            AtRect *a = &r[i];
            Mat *pg = &out[a->page];
            for (y = -1; y <= a->h; y++) for (x = -1; x <= a->w; x++) {
                int sx = (x < 0 ? 0 : x >= a->w ? a->w - 1 : x) * m->w / a->w, sy = (y < 0 ? 0 : y >= a->h ? a->h - 1 : y) * m->h / a->h;
                for (c = 0; c < 4; c++) { pg->px[((a->y + y) * side + a->x + x) * 4 + c] = m->px[(sy * m->w + sx) * 4 + c]; }
            }
        }
        for (i = 0; i < s->nt; i++) { /* the triangles' places */
            AtRect *a = &r[s->tm[i]];
            float tu = floorf(s->v[s->tri[i * 3]].uv[0]), tv = floorf(s->v[s->tri[i * 3]].uv[1]);
            for (c = 0; c < 3; c++) {
                float *uv = s->v[s->tri[i * 3 + c]].uv, u = uv[0] - tu, v = uv[1] - tv;
                u = u < 0 ? 0 : u > 1 ? 1 : u;
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                uv[0] = ((float)a->x + u * (float)a->w) / (float)side;
                uv[1] = ((float)a->y + v * (float)a->h) / (float)side;
            }
            s->tm[i] = a->page;
        }
        for (i = 0; i < n; i++) { stbi_image_free(s->mat[i].px); s->mat[i].px = NULL; }
        memset(s->mat, 0, sizeof(Mat) * MI_LOAD);
        memset(s->matKey, 0, sizeof(void *) * MI_LOAD);
        memcpy(s->mat, out, sizeof(out));
        s->nmat = pages;
        s->paged = 1;
    }
}

static void dir_of(const char *path, char *out, int size) {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > a) { a = b; }
    snprintf(out, (size_t)size, "%.*s", a != NULL ? (int)(a - path + 1) : 0, path);
}
static const char *base_of(const char *path) {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > a) { a = b; }
    return a != NULL ? a + 1 : path;
}

static int load_ufbx(const char *path, Scene *s, char *why, int whyLen) {
    ufbx_load_opts o;
    ufbx_error e;
    ufbx_scene *sc;
    size_t i, f;
    char dir[600];

    memset(&o, 0, sizeof(o));
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.generate_missing_normals = true;
    o.load_external_files = true; /* (an OBJ's .mtl, where its textures are named) */
    o.ignore_missing_external_files = true;
    o.obj_search_mtl_by_filename = true;
    if ((sc = ufbx_load_file(path, &o, &e)) == NULL) {
        snprintf(why, (size_t)whyLen, "Cannot read the model: %s", e.description.data);
        return 0;
    }
    dir_of(path, dir, sizeof(dir));
    s->nj = (int)sc->nodes.count;
    s->j = calloc((size_t)s->nj + 1, sizeof(Joint));
    for (i = 0; i < sc->nodes.count; i++) {
        ufbx_node *n = sc->nodes.data[i];
        snprintf(s->j[i].name, sizeof(s->j[i].name), "%s", n->name.data);
        s->j[i].parent = n->parent != NULL ? (int)n->parent->typed_id : -1;
        v_set(s->j[i].pos, (float)n->node_to_world.cols[3].x, (float)n->node_to_world.cols[3].y, (float)n->node_to_world.cols[3].z);
    }
    for (i = 0; i < sc->nodes.count; i++) {
        ufbx_node *n = sc->nodes.data[i];
        ufbx_mesh *m = n->mesh;
        ufbx_skin_deformer *skin;
        uint32_t *ti;
        if (m == NULL) { continue; }
        skin = m->skin_deformers.count > 0 ? m->skin_deformers.data[0] : NULL;
        if (skin != NULL) { s->j[i].name[0] = '\0'; } /* (a node that carries a skinned mesh is not a bone, whatever its name says) */
        ti = malloc(sizeof(uint32_t) * 3 * (m->max_face_triangles + 1));
        for (f = 0; f < m->faces.count; f++) {
            uint32_t nt = ufbx_triangulate_face(ti, m->max_face_triangles * 3, m, m->faces.data[f]), k, c;
            uint32_t mi = m->face_material.count > f ? m->face_material.data[f] : 0;
            ufbx_material *mat = n->materials.count > mi ? n->materials.data[mi] : m->materials.count > mi ? m->materials.data[mi] : NULL;
            int at;
            {
                float col[3] = {0.7f, 0.7f, 0.7f};
                char f1[900] = "", f2[900] = "", f3[900] = "", f4[900] = "";
                const char *files[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
                ufbx_texture *tex = NULL;
                if (mat != NULL) {
                    ufbx_material_map *mm = mat->pbr.base_color.texture != NULL || mat->pbr.base_color.has_value ? &mat->pbr.base_color : &mat->fbx.diffuse_color;
                    tex = mm->texture;
                    if (mm->has_value) { v_set(col, (float)mm->value_vec3.x, (float)mm->value_vec3.y, (float)mm->value_vec3.z); }
                    if (tex != NULL) {
                        snprintf(f1, sizeof(f1), "%s%s", dir, tex->relative_filename.data);
                        snprintf(f2, sizeof(f2), "%s%s", dir, base_of(tex->filename.data));
                        { /* also in a folder beside the model named as the model, or "textures" */
                            const char *mb = base_of(path), *md = strrchr(mb, '.');
                            snprintf(f3, sizeof(f3), "%s%.*s/%s", dir, (int)(md != NULL ? md - mb : (long)strlen(mb)), mb, base_of(tex->filename.data));
                            snprintf(f4, sizeof(f4), "%stextures/%s", dir, base_of(tex->filename.data));
                        }
                        files[0] = tex->filename.data; files[1] = f1; files[2] = f2; files[3] = f3; files[4] = f4;
                    }
                }
                at = scene_mat(s, mat, tex != NULL ? tex->content.data : NULL, tex != NULL ? tex->content.size : 0, files, col);
                if (getenv("BT3_MODEL_LOG") != NULL && at < MI_LOAD) { fprintf(stderr, "bt3: model import: material %d %s tex %s -> %s %dx%d\n", at, mat != NULL ? mat->name.data : "-", tex != NULL ? tex->relative_filename.data : "-", s->mat[at].px ? "loaded" : "colour", s->mat[at].w, s->mat[at].h); }
            }
            for (k = 0; k < nt; k++) {
                Vtx v[3];
                memset(v, 0, sizeof(v));
                for (c = 0; c < 3; c++) {
                    uint32_t ix = ti[k * 3 + c];
                    ufbx_vec3 p = ufbx_get_vertex_vec3(&m->vertex_position, ix), w = {0, 0, 0}, nn = {0, 1, 0};
                    double tot = 0;
                    int q, nw = 0;
                    if (skin != NULL) { /* the mesh as its bones hold it now */
                        ufbx_skin_vertex sv = skin->vertices.data[m->vertex_indices.data[ix]];
                        for (q = 0; q < (int)sv.num_weights && nw < 4; q++) {
                            ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + (uint32_t)q];
                            ufbx_skin_cluster *cl = skin->clusters.data[sw.cluster_index];
                            ufbx_vec3 t = ufbx_transform_position(&cl->geometry_to_world, p);
                            if (cl->bone_node == NULL || sw.weight <= 0) { continue; }
                            w.x += t.x * sw.weight; w.y += t.y * sw.weight; w.z += t.z * sw.weight;
                            tot += sw.weight;
                            v[c].jb[nw] = (int)cl->bone_node->typed_id;
                            v[c].jw[nw++] = (float)sw.weight;
                        }
                    }
                    if (tot > 0) {
                        w.x /= tot; w.y /= tot; w.z /= tot;
                        for (q = 0; q < nw; q++) { v[c].jw[q] /= (float)tot; }
                    } else { /* not skinned: it goes with the node it hangs on */
                        w = ufbx_transform_position(&n->geometry_to_world, p);
                        v[c].jb[0] = (int)n->typed_id;
                        v[c].jw[0] = 1;
                    }
                    if (m->vertex_normal.exists) {
                        nn = ufbx_transform_direction(&n->geometry_to_world, ufbx_get_vertex_vec3(&m->vertex_normal, ix));
                    }
                    v_set(v[c].p, (float)w.x, (float)w.y, (float)w.z);
                    v_set(v[c].n, (float)nn.x, (float)nn.y, (float)nn.z);
                    v_norm(v[c].n);
                    if (m->vertex_color.exists) {
                        ufbx_vec4 c4 = ufbx_get_vertex_vec4(&m->vertex_color, ix);
                        v[c].vc[0] = (float)c4.x; v[c].vc[1] = (float)c4.y; v[c].vc[2] = (float)c4.z; v[c].vc[3] = 1;
                    }
                    if (m->vertex_uv.exists) {
                        ufbx_vec2 t = ufbx_get_vertex_vec2(&m->vertex_uv, ix);
                        v[c].uv[0] = (float)t.x;
                        v[c].uv[1] = 1.0f - (float)t.y;
                    }
                }
                scene_tri(s, v, at);
            }
        }
        free(ti);
    }
    ufbx_free_scene(sc);
    return 1;
}

static int load_gltf(const char *path, Scene *s, char *why, int whyLen) {
    cgltf_options o;
    cgltf_data *d = NULL;
    cgltf_size i, p, k;
    char dir[600];

    memset(&o, 0, sizeof(o));
    if (cgltf_parse_file(&o, path, &d) != cgltf_result_success || cgltf_load_buffers(&o, d, path) != cgltf_result_success) {
        snprintf(why, (size_t)whyLen, "Cannot read the model (glTF)");
        cgltf_free(d);
        return 0;
    }
    dir_of(path, dir, sizeof(dir));
    s->nj = (int)d->nodes_count;
    s->j = calloc((size_t)s->nj + 1, sizeof(Joint));
    for (i = 0; i < d->nodes_count; i++) {
        float m[16];
        cgltf_node_transform_world(&d->nodes[i], m);
        snprintf(s->j[i].name, sizeof(s->j[i].name), "%s", d->nodes[i].name != NULL ? d->nodes[i].name : "");
        s->j[i].parent = d->nodes[i].parent != NULL ? (int)(d->nodes[i].parent - d->nodes) : -1;
        v_set(s->j[i].pos, m[12], m[13], m[14]);
    }
    for (i = 0; i < d->nodes_count; i++) {
        cgltf_node *n = &d->nodes[i];
        float m[16];
        if (n->mesh == NULL) { continue; }
        if (n->skin != NULL) { s->j[i].name[0] = '\0'; } /* (a node that carries a skinned mesh is not a bone, whatever its name says) */
        cgltf_node_transform_world(n, m);
        for (p = 0; p < n->mesh->primitives_count; p++) {
            cgltf_primitive *pr = &n->mesh->primitives[p];
            const cgltf_accessor *ap = NULL, *an = NULL, *at = NULL, *aj = NULL, *aw = NULL;
            /* the picture's view: which texture coordinates, and KHR_texture_transform (a file with quantized
               coordinates keeps them in a corner and scales them up there: without it the picture's corner is all
               that shows) */
            const cgltf_texture_view *tv = pr->material != NULL ? &pr->material->pbr_metallic_roughness.base_color_texture : NULL;
            int uvSet = tv == NULL ? 0 : tv->has_transform && tv->transform.has_texcoord ? tv->transform.texcoord : tv->texcoord;
            cgltf_size count;
            int mat;
            if (pr->type != cgltf_primitive_type_triangles) { continue; }
            for (k = 0; k < pr->attributes_count; k++) {
                cgltf_attribute *a = &pr->attributes[k];
                if (a->type == cgltf_attribute_type_position) { ap = a->data; }
                else if (a->type == cgltf_attribute_type_normal) { an = a->data; }
                else if (a->type == cgltf_attribute_type_texcoord && a->index == uvSet) { at = a->data; }
                else if (a->type == cgltf_attribute_type_joints && a->index == 0) { aj = a->data; }
                else if (a->type == cgltf_attribute_type_weights && a->index == 0) { aw = a->data; }
            }
            if (ap == NULL) { continue; }
            {
                float col[3] = {0.7f, 0.7f, 0.7f};
                char f1[900] = "";
                const char *files[2] = {NULL, NULL};
                const unsigned char *mem = NULL;
                size_t memLen = 0;
                if (pr->material != NULL) {
                    cgltf_texture *t = pr->material->pbr_metallic_roughness.base_color_texture.texture;
                    memcpy(col, pr->material->pbr_metallic_roughness.base_color_factor, sizeof(col));
                    if (t != NULL && t->image != NULL) {
                        if (t->image->buffer_view != NULL && t->image->buffer_view->buffer->data != NULL) {
                            mem = (const unsigned char *)t->image->buffer_view->buffer->data + t->image->buffer_view->offset;
                            memLen = t->image->buffer_view->size;
                        } else if (t->image->uri != NULL && strncmp(t->image->uri, "data:", 5) != 0) {
                            snprintf(f1, sizeof(f1), "%s%s", dir, t->image->uri);
                            files[0] = f1;
                        }
                    }
                }
                mat = scene_mat(s, pr->material, mem, memLen, files, col);
            }
            count = pr->indices != NULL ? pr->indices->count : ap->count;
            for (k = 0; k + 2 < count; k += 3) {
                Vtx v[3];
                int c, q;
                memset(v, 0, sizeof(v));
                for (c = 0; c < 3; c++) {
                    cgltf_size ix = pr->indices != NULL ? cgltf_accessor_read_index(pr->indices, k + (cgltf_size)c) : k + (cgltf_size)c;
                    float t[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
                    cgltf_uint jn[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_float(ap, ix, t, 3);
                    if (n->skin != NULL && aj != NULL && aw != NULL) { /* (a skinned mesh lies where its bind pose is) */
                        float tot;
                        v_set(v[c].p, t[0], t[1], t[2]);
                        cgltf_accessor_read_uint(aj, ix, jn, 4);
                        cgltf_accessor_read_float(aw, ix, w, 4);
                        tot = w[0] + w[1] + w[2] + w[3];
                        for (q = 0; q < 4; q++) {
                            v[c].jb[q] = jn[q] < n->skin->joints_count ? (int)(n->skin->joints[jn[q]] - d->nodes) : 0;
                            v[c].jw[q] = tot > 0 ? w[q] / tot : (q == 0);
                        }
                    } else {
                        v_set(v[c].p, m[0] * t[0] + m[4] * t[1] + m[8] * t[2] + m[12], m[1] * t[0] + m[5] * t[1] + m[9] * t[2] + m[13],
                              m[2] * t[0] + m[6] * t[1] + m[10] * t[2] + m[14]);
                        v[c].jb[0] = (int)i;
                        v[c].jw[0] = 1;
                    }
                    v_set(v[c].n, 0, 1, 0);
                    if (an != NULL) {
                        cgltf_accessor_read_float(an, ix, t, 3);
                        v_set(v[c].n, m[0] * t[0] + m[4] * t[1] + m[8] * t[2], m[1] * t[0] + m[5] * t[1] + m[9] * t[2], m[2] * t[0] + m[6] * t[1] + m[10] * t[2]);
                        v_norm(v[c].n);
                    }
                    if (at != NULL) {
                        cgltf_accessor_read_float(at, ix, t, 2);
                        if (tv != NULL && tv->has_transform) { /* offset, then rotation, then scale, as the extension's matrix */
                            const cgltf_texture_transform *x = &tv->transform;
                            float cs = cosf(x->rotation), sn = sinf(x->rotation), su = t[0] * x->scale[0], sv = t[1] * x->scale[1];
                            t[0] = x->offset[0] + cs * su + sn * sv;
                            t[1] = x->offset[1] - sn * su + cs * sv;
                        }
                        v[c].uv[0] = t[0]; v[c].uv[1] = t[1];
                    }
                }
                scene_tri(s, v, mat);
            }
        }
    }
    cgltf_free(d);
    return 1;
}

/* ---- COLLADA (.dae) ----
   A small reader of its own: the XML as a tree, then geometries (triangles, polylist), skins (controllers), the
   scene's nodes with their matrices, materials by way of effects to image files. ponytail: no <polygons>, no
   morphs, no instanced node libraries, one UV set, textures from files only; a real COLLADA library does more. */
typedef struct { const char *name, *attr, *text; int nameLen, attrLen, textLen, parent, child, next, last; } XNode;
typedef struct { XNode *n; int count, cap; } XDoc;

static int x_new(XDoc *d, int parent) {
    XNode *x;
    if (d->count == d->cap) {
        d->cap = d->cap * 2 + 1024;
        d->n = realloc(d->n, sizeof(XNode) * (size_t)d->cap);
    }
    x = &d->n[d->count];
    memset(x, 0, sizeof(*x));
    x->parent = parent; x->child = x->next = x->last = -1;
    if (parent >= 0) {
        if (d->n[parent].last >= 0) { d->n[d->n[parent].last].next = d->count; } else { d->n[parent].child = d->count; }
        d->n[parent].last = d->count;
    }
    return d->count++;
}

/* the text s (kept by the caller as long as the tree) */
static int x_parse(XDoc *d, const char *s, long n) {
    const char *p = s, *e = s + n;
    int cur = x_new(d, -1);
    while (p < e) {
        const char *lt = memchr(p, '<', (size_t)(e - p));
        if (lt == NULL) { break; }
        if (d->n[cur].text == NULL && lt > p) { d->n[cur].text = p; d->n[cur].textLen = (int)(lt - p); }
        p = lt + 1;
        if (p < e && (*p == '?' || *p == '!')) {
            const char *gt = memchr(p, '>', (size_t)(e - p));
            if (e - p > 3 && p[0] == '!' && p[1] == '-' && p[2] == '-') {
                for (gt = p + 3; gt + 2 < e && !(gt[0] == '-' && gt[1] == '-' && gt[2] == '>'); gt++) {}
                gt += 2;
            }
            if (gt == NULL || gt >= e) { break; }
            p = gt + 1;
        } else if (p < e && *p == '/') {
            const char *gt = memchr(p, '>', (size_t)(e - p));
            if (gt == NULL) { break; }
            if (d->n[cur].parent >= 0) { cur = d->n[cur].parent; }
            p = gt + 1;
        } else {
            const char *q = p, *gt;
            int me = x_new(d, cur), quote = 0;
            while (q < e && !isspace((unsigned char)*q) && *q != '>' && *q != '/') { q++; }
            d->n[me].name = p; d->n[me].nameLen = (int)(q - p);
            for (gt = q; gt < e && (quote || *gt != '>'); gt++) {
                if (*gt == '"' || *gt == '\'') { quote = quote == 0 ? *gt : quote == *gt ? 0 : quote; }
            }
            if (gt >= e) { return 0; }
            d->n[me].attr = q; d->n[me].attrLen = (int)(gt - q);
            if (gt[-1] != '/') { cur = me; }
            p = gt + 1;
        }
    }
    return d->count > 1;
}

static int x_is(const XDoc *d, int i, const char *name) {
    return i >= 0 && d->n[i].nameLen == (int)strlen(name) && memcmp(d->n[i].name, name, (size_t)d->n[i].nameLen) == 0;
}
/* the first child of i called name, or the next one after child `after` */
static int x_child(const XDoc *d, int i, const char *name, int after) {
    int c = i < 0 ? -1 : after >= 0 ? d->n[after].next : d->n[i].child;
    for (; c >= 0; c = d->n[c].next) if (x_is(d, c, name)) { return c; }
    return -1;
}
/* the first element called name anywhere below i */
static int x_find(const XDoc *d, int i, const char *name) {
    int c, r;
    for (c = i < 0 ? -1 : d->n[i].child; c >= 0; c = d->n[c].next) {
        if (x_is(d, c, name)) { return c; }
        if ((r = x_find(d, c, name)) >= 0) { return r; }
    }
    return -1;
}
static const char *x_attr(const XDoc *d, int i, const char *name, char *out, int size) {
    const char *p, *e;
    size_t l = strlen(name);
    out[0] = '\0';
    if (i < 0) { return out; }
    for (p = d->n[i].attr, e = p + d->n[i].attrLen; p + l + 2 < e; p++) {
        if (isspace((unsigned char)p[0]) && memcmp(p + 1, name, l) == 0 && p[l + 1] == '=' && (p[l + 2] == '"' || p[l + 2] == '\'')) {
            const char *v = p + l + 3, *z = memchr(v, p[l + 2], (size_t)(e - v));
            if (z != NULL) { snprintf(out, (size_t)size, "%.*s", (int)(z - v), v); }
            return out;
        }
    }
    return out;
}
static const char *x_text(const XDoc *d, int i, char *out, int size) {
    const char *p, *e;
    out[0] = '\0';
    if (i < 0 || d->n[i].text == NULL) { return out; }
    p = d->n[i].text; e = p + d->n[i].textLen;
    while (p < e && isspace((unsigned char)*p)) { p++; }
    while (e > p && isspace((unsigned char)e[-1])) { e--; }
    snprintf(out, (size_t)size, "%.*s", (int)(e - p), p);
    return out;
}
/* the numbers of an element's text; *n how many */
static double *x_nums(const XDoc *d, int i, int *n) {
    double *o = NULL;
    int cap = 0, count = 0;
    char *end, *buf;
    const char *p;
    *n = 0;
    if (i < 0 || d->n[i].text == NULL) { return NULL; }
    buf = malloc((size_t)d->n[i].textLen + 1);
    memcpy(buf, d->n[i].text, (size_t)d->n[i].textLen);
    buf[d->n[i].textLen] = '\0';
    for (p = buf;; p = end) {
        double v = strtod(p, &end);
        if (end == p) { break; }
        if (count == cap) { cap = cap * 2 + 256; o = realloc(o, sizeof(double) * (size_t)cap); }
        o[count++] = v;
    }
    free(buf);
    *n = count;
    return o;
}
/* the element with this id below i ("#id" too) */
static int x_by_id(const XDoc *d, const char *id) {
    char a[200];
    int i;
    if (id[0] == '#') { id++; }
    for (i = 1; i < d->count; i++) if (strcmp(x_attr(d, i, "id", a, sizeof(a)), id) == 0) { return i; }
    return -1;
}

static void m4_mul(double o[16], const double a[16], const double b[16]) { /* row-major, column vectors */
    double t[16];
    int r, c;
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) { t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c]; }
    memcpy(o, t, sizeof(t));
}
static void m4_id(double m[16]) { memset(m, 0, 128); m[0] = m[5] = m[10] = m[15] = 1; }

typedef struct { double world[16]; char sid[64], id[96]; int xml; } DaeNode;
typedef struct { const XDoc *d; Scene *s; DaeNode *n; int count, cap, zUp; const char *dir; } Dae;

static void dae_out(const Dae *a, V3 o, double x, double y, double z) { /* to Y up, front +Z */
    if (a->zUp) { v_set(o, (float)x, (float)z, (float)-y); } else { v_set(o, (float)x, (float)y, (float)z); }
}

/* the scene's nodes, parents first: each one a joint of the scene */
static void dae_nodes(Dae *a, int x, int parent, const double pw[16]) {
    const XDoc *d = a->d;
    double m[16], t[16];
    char buf[100];
    int c, me, k;
    m4_id(m);
    for (c = d->n[x].child; c >= 0; c = d->n[c].next) { /* its transforms, in order */
        double *v = NULL;
        m4_id(t);
        if (x_is(d, c, "matrix") && (v = x_nums(d, c, &k)) != NULL && k >= 16) {
            for (k = 0; k < 16; k++) { t[k] = v[k]; }
        } else if (x_is(d, c, "translate") && (v = x_nums(d, c, &k)) != NULL && k >= 3) {
            t[3] = v[0]; t[7] = v[1]; t[11] = v[2];
        } else if (x_is(d, c, "scale") && (v = x_nums(d, c, &k)) != NULL && k >= 3) {
            t[0] = v[0]; t[5] = v[1]; t[10] = v[2];
        } else if (x_is(d, c, "rotate") && (v = x_nums(d, c, &k)) != NULL && k >= 4) {
            double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), ang = v[3] * 3.14159265358979323846 / 180, cs = cos(ang), sn = sin(ang);
            double ax = l > 0 ? v[0] / l : 0, ay = l > 0 ? v[1] / l : 0, az = l > 0 ? v[2] / l : 1;
            t[0] = cs + ax * ax * (1 - cs);      t[1] = ax * ay * (1 - cs) - az * sn; t[2] = ax * az * (1 - cs) + ay * sn;
            t[4] = ay * ax * (1 - cs) + az * sn; t[5] = cs + ay * ay * (1 - cs);      t[6] = ay * az * (1 - cs) - ax * sn;
            t[8] = az * ax * (1 - cs) - ay * sn; t[9] = az * ay * (1 - cs) + ax * sn; t[10] = cs + az * az * (1 - cs);
        } else {
            free(v);
            continue;
        }
        free(v);
        m4_mul(m, m, t);
    }
    if (a->count == a->cap) {
        a->cap = a->cap * 2 + 256;
        a->n = realloc(a->n, sizeof(DaeNode) * (size_t)a->cap);
        a->s->j = realloc(a->s->j, sizeof(Joint) * (size_t)a->cap);
    }
    me = a->count++;
    m4_mul(a->n[me].world, pw, m);
    a->n[me].xml = x;
    x_attr(d, x, "sid", a->n[me].sid, sizeof(a->n[me].sid));
    x_attr(d, x, "id", a->n[me].id, sizeof(a->n[me].id));
    memset(&a->s->j[me], 0, sizeof(Joint));
    x_attr(d, x, "name", buf, sizeof(buf));
    snprintf(a->s->j[me].name, sizeof(a->s->j[me].name), "%s", buf[0] != '\0' ? buf : a->n[me].sid);
    a->s->j[me].parent = parent;
    dae_out(a, a->s->j[me].pos, a->n[me].world[3], a->n[me].world[7], a->n[me].world[11]);
    a->s->nj = a->count;
    for (c = x_child(d, x, "node", -1); c >= 0; c = x_child(d, x, "node", c)) {
        double w[16];
        memcpy(w, a->n[me].world, sizeof(w)); /* (a->n may move) */
        dae_nodes(a, c, me, w);
    }
}

/* a <source>'s numbers and how many make one element */
static double *dae_source(const XDoc *d, const char *url, int *n, int *stride) {
    int src = x_by_id(d, url), acc;
    char buf[40];
    *n = 0; *stride = 1;
    if (src >= 0 && x_is(d, src, "vertices")) { /* (VERTEX: its POSITION input) */
        char u[200];
        int in;
        for (in = x_child(d, src, "input", -1); in >= 0; in = x_child(d, src, "input", in)) {
            if (strcmp(x_attr(d, in, "semantic", buf, sizeof(buf)), "POSITION") == 0) { return dae_source(d, x_attr(d, in, "source", u, sizeof(u)), n, stride); }
        }
        return NULL;
    }
    if (src < 0) { return NULL; }
    acc = x_find(d, src, "accessor");
    if (acc >= 0 && atoi(x_attr(d, acc, "stride", buf, sizeof(buf))) > 0) { *stride = atoi(buf); }
    return x_nums(d, x_child(d, src, "float_array", -1), n);
}

/* the picture file and colour of a material, by way of its effect */
static int dae_material(Dae *a, const char *target) {
    const XDoc *d = a->d;
    char u[200], img[200], file[600], f1[900] = "", f2[900] = "";
    const char *files[3] = {NULL, NULL, NULL};
    float col[3] = {0.7f, 0.7f, 0.7f};
    int mat = x_by_id(d, target), fx, k, c;
    file[0] = '\0';
    if (mat >= 0 && (fx = x_by_id(d, x_attr(d, x_find(d, mat, "instance_effect"), "url", u, sizeof(u)))) >= 0) {
        int dif = x_find(d, fx, "diffuse"), surf = x_find(d, fx, "surface"), image;
        double *v = x_nums(d, x_child(d, dif, "color", -1), &k);
        if (v != NULL && k >= 3) { v_set(col, (float)v[0], (float)v[1], (float)v[2]); }
        free(v);
        x_text(d, x_child(d, surf, "init_from", -1), img, sizeof(img));
        if (img[0] == '\0') { x_attr(d, x_child(d, dif, "texture", -1), "texture", img, sizeof(img)); } /* (the image named directly) */
        if (x_child(d, dif, "texture", -1) >= 0 && (image = x_by_id(d, img)) >= 0) {
            x_text(d, x_find(d, image, "init_from"), file, sizeof(file));
            if (x_find(d, image, "ref") >= 0 && file[0] == '\0') { x_text(d, x_find(d, image, "ref"), file, sizeof(file)); }
        }
    }
    if (file[0] != '\0') {
        const char *p = strncmp(file, "file://", 7) == 0 ? file + 7 : file;
        for (c = 0; file[c] != '\0'; c++) if (file[c] == '%' && file[c + 1] == '2' && file[c + 2] == '0') { file[c] = ' '; memmove(file + c + 1, file + c + 3, strlen(file + c + 3) + 1); }
        if (p[0] == '/' && p[2] == ':') { p++; }
        snprintf(f1, sizeof(f1), "%s%s", a->dir, p);
        snprintf(f2, sizeof(f2), "%s%s", a->dir, base_of(p));
        files[0] = f1; files[1] = f2;
    }
    return scene_mat(a->s, d->n + (mat >= 0 ? mat : 0), NULL, 0, files, col);
}

/* One instance of a geometry on node `node`; skin: its <skin>, or -1. */
static void dae_mesh(Dae *a, int node, int inst, int geom, int skin) {
    const XDoc *d = a->d;
    Scene *s = a->s;
    int mesh = x_child(d, geom, "mesh", -1), prim, c, k;
    /* the skin: for every vertex its joints and weights, and the matrix each joint carries it with */
    double bsm[16], *inv = NULL, *wts = NULL, *vc = NULL, *vv = NULL, (*jm)[16] = NULL;
    int *jidx = NULL, nj = 0, ninv = 0, nw = 0, nvc = 0, nvv = 0, jOff = 0, wOff = 1, perV = 2, *vstart = NULL;
    char buf[200], u[200];

    m4_id(bsm);
    if (skin >= 0) {
        double *t = x_nums(d, x_child(d, skin, "bind_shape_matrix", -1), &k);
        int jn = x_child(d, skin, "joints", -1), vw = x_child(d, skin, "vertex_weights", -1), in, st;
        if (t != NULL && k >= 16) { memcpy(bsm, t, sizeof(bsm)); }
        free(t);
        for (in = x_child(d, jn, "input", -1); in >= 0; in = x_child(d, jn, "input", in)) {
            x_attr(d, in, "semantic", buf, sizeof(buf));
            x_attr(d, in, "source", u, sizeof(u));
            if (strcmp(buf, "INV_BIND_MATRIX") == 0) {
                inv = dae_source(d, u, &ninv, &st);
            } else if (strcmp(buf, "JOINT") == 0) { /* names: the nodes' sids (or ids) */
                int src = x_by_id(d, u), arr = x_child(d, src, "Name_array", -1);
                char *names, *tok;
                if (arr < 0) { arr = x_child(d, src, "IDREF_array", -1); }
                if (arr >= 0 && d->n[arr].text != NULL) {
                    names = malloc((size_t)d->n[arr].textLen + 1);
                    memcpy(names, d->n[arr].text, (size_t)d->n[arr].textLen);
                    names[d->n[arr].textLen] = '\0';
                    for (tok = strtok(names, " \t\r\n"); tok != NULL; tok = strtok(NULL, " \t\r\n")) {
                        int found = -1;
                        for (k = 0; k < a->count && found < 0; k++) if (strcmp(a->n[k].sid, tok) == 0 || strcmp(a->n[k].id, tok) == 0) { found = k; }
                        jidx = realloc(jidx, sizeof(int) * (size_t)(nj + 1));
                        jidx[nj++] = found;
                    }
                    free(names);
                }
            }
        }
        perV = 0;
        for (in = x_child(d, vw, "input", -1); in >= 0; in = x_child(d, vw, "input", in)) {
            int off = atoi(x_attr(d, in, "offset", buf, sizeof(buf)));
            x_attr(d, in, "semantic", buf, sizeof(buf));
            if (strcmp(buf, "JOINT") == 0) { jOff = off; }
            if (strcmp(buf, "WEIGHT") == 0) { wOff = off; wts = dae_source(d, x_attr(d, in, "source", u, sizeof(u)), &nw, &st); }
            if (off + 1 > perV) { perV = off + 1; }
        }
        vc = x_nums(d, x_child(d, vw, "vcount", -1), &nvc);
        vv = x_nums(d, x_child(d, vw, "v", -1), &nvv);
        vstart = malloc(sizeof(int) * (size_t)(nvc + 1));
        for (k = 0, c = 0; k < nvc; k++) { vstart[k] = c; c += (int)vc[k] * perV; }
        jm = malloc(sizeof(*jm) * (size_t)(nj + 1));
        for (k = 0; k < nj; k++) {
            m4_id(jm[k]);
            if (jidx[k] >= 0 && (k + 1) * 16 <= ninv) {
                m4_mul(jm[k], a->n[jidx[k]].world, inv + k * 16);
                m4_mul(jm[k], jm[k], bsm);
            }
        }
    }
    for (prim = mesh < 0 ? -1 : d->n[mesh].child; prim >= 0; prim = d->n[prim].next) {
        double *pos = NULL, *nrm = NULL, *uv = NULL, *p, *pv = NULL;
        int npos = 0, nnrm = 0, nuv = 0, sp = 3, sn = 3, su = 2, oP = -1, oN = -1, oU = -1, per = 0, np = 0, npv = 0, in, at = 0, f, mat, poly = x_is(d, prim, "polylist");
        if (!poly && !x_is(d, prim, "triangles")) { continue; }
        for (in = x_child(d, prim, "input", -1); in >= 0; in = x_child(d, prim, "input", in)) {
            int off = atoi(x_attr(d, in, "offset", buf, sizeof(buf)));
            x_attr(d, in, "semantic", buf, sizeof(buf));
            x_attr(d, in, "source", u, sizeof(u));
            if (strcmp(buf, "VERTEX") == 0) { oP = off; pos = dae_source(d, u, &npos, &sp); }
            else if (strcmp(buf, "NORMAL") == 0) { oN = off; nrm = dae_source(d, u, &nnrm, &sn); }
            else if (strcmp(buf, "TEXCOORD") == 0 && uv == NULL) { oU = off; uv = dae_source(d, u, &nuv, &su); }
            if (off + 1 > per) { per = off + 1; }
        }
        p = x_nums(d, x_child(d, prim, "p", -1), &np);
        if (poly) { pv = x_nums(d, x_child(d, prim, "vcount", -1), &npv); }
        /* its material: the symbol, bound on the instance */
        {
            char sym[200], target[200];
            int im;
            x_attr(d, prim, "material", sym, sizeof(sym));
            snprintf(target, sizeof(target), "%s", sym);
            for (im = x_find(d, inst, "instance_material"); im >= 0; im = d->n[im].next) {
                if (x_is(d, im, "instance_material") && strcmp(x_attr(d, im, "symbol", buf, sizeof(buf)), sym) == 0) { x_attr(d, im, "target", target, sizeof(target)); }
            }
            mat = dae_material(a, target);
        }
        for (f = 0; pos != NULL && p != NULL && per > 0 && (poly ? f < npv : at + 3 * per <= np); f++) {
            int corners = poly ? (int)pv[f] : 3, t;
            if (at + corners * per > np) { break; }
            for (t = 1; t + 1 < corners; t++) { /* a fan */
                Vtx v[3];
                int cs[3] = {0, t, t + 1};
                memset(v, 0, sizeof(v));
                for (c = 0; c < 3; c++) {
                    const double *e = p + at + cs[c] * per;
                    int ip = (int)e[oP], q, nwv = 0;
                    double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 1, wx = 0, wy = 0, wz = 0, wnx = 0, wny = 0, wnz = 0, tot = 0;
                    const double *M;
                    if (ip < 0 || (ip + 1) * sp > npos) { continue; }
                    x = pos[ip * sp]; y = pos[ip * sp + 1]; z = pos[ip * sp + 2];
                    if (oN >= 0 && nrm != NULL && ((int)e[oN] + 1) * sn <= nnrm) { nx = nrm[(int)e[oN] * sn]; ny = nrm[(int)e[oN] * sn + 1]; nz = nrm[(int)e[oN] * sn + 2]; }
                    if (oU >= 0 && uv != NULL && ((int)e[oU] + 1) * su <= nuv) { v[c].uv[0] = (float)uv[(int)e[oU] * su]; v[c].uv[1] = 1.0f - (float)uv[(int)e[oU] * su + 1]; }
                    if (skin >= 0 && ip < nvc) {
                        /* (its four heaviest joints) */
                        for (q = 0; q < (int)vc[ip]; q++) {
                            int o = vstart[ip] + q * perV, j, wi, low = 0, r;
                            double w;
                            if (o + perV > nvv) { break; }
                            j = (int)vv[o + jOff]; wi = (int)vv[o + wOff];
                            w = wi >= 0 && wi < nw ? wts[wi] : 0;
                            if (j < 0 || j >= nj || jidx[j] < 0 || w <= 0) { continue; }
                            if (nwv < 4) {
                                r = nwv++;
                            } else {
                                for (r = 1; r < 4; r++) if (v[c].jw[r] < v[c].jw[low]) { low = r; }
                                if (w <= v[c].jw[low]) { continue; }
                                r = low;
                            }
                            v[c].jb[r] = j; /* (the skin's joint for now) */
                            v[c].jw[r] = (float)w;
                        }
                        for (q = 0; q < nwv; q++) {
                            M = jm[v[c].jb[q]];
                            wx += v[c].jw[q] * (M[0] * x + M[1] * y + M[2] * z + M[3]);
                            wy += v[c].jw[q] * (M[4] * x + M[5] * y + M[6] * z + M[7]);
                            wz += v[c].jw[q] * (M[8] * x + M[9] * y + M[10] * z + M[11]);
                            wnx += v[c].jw[q] * (M[0] * nx + M[1] * ny + M[2] * nz);
                            wny += v[c].jw[q] * (M[4] * nx + M[5] * ny + M[6] * nz);
                            wnz += v[c].jw[q] * (M[8] * nx + M[9] * ny + M[10] * nz);
                            tot += v[c].jw[q];
                        }
                        for (q = 0; q < nwv; q++) { v[c].jb[q] = jidx[v[c].jb[q]]; v[c].jw[q] /= (float)tot; }
                    }
                    if (tot > 0) {
                        wx /= tot; wy /= tot; wz /= tot;
                    } else { /* not skinned: with the node it hangs on */
                        M = a->n[node].world;
                        wx = M[0] * x + M[1] * y + M[2] * z + M[3]; wy = M[4] * x + M[5] * y + M[6] * z + M[7]; wz = M[8] * x + M[9] * y + M[10] * z + M[11];
                        wnx = M[0] * nx + M[1] * ny + M[2] * nz; wny = M[4] * nx + M[5] * ny + M[6] * nz; wnz = M[8] * nx + M[9] * ny + M[10] * nz;
                        memset(v[c].jb, 0, sizeof(v[c].jb)); memset(v[c].jw, 0, sizeof(v[c].jw));
                        v[c].jb[0] = node; v[c].jw[0] = 1;
                    }
                    dae_out(a, v[c].p, wx, wy, wz);
                    dae_out(a, v[c].n, wnx, wny, wnz);
                    if (!v_norm(v[c].n)) { v_set(v[c].n, 0, 1, 0); }
                }
                scene_tri(s, v, mat);
            }
            at += corners * per;
        }
        free(pos); free(nrm); free(uv); free(p); free(pv);
    }
    free(inv); free(wts); free(vc); free(vv); free(jm); free(jidx); free(vstart);
}

static int load_dae(const char *path, Scene *s, char *why, int whyLen) {
    XDoc d;
    Dae a;
    long n;
    char *text = (char *)read_all(path, &n), dir[600], buf[200], u[200];
    double id[16];
    int root, vs, c, i;

    memset(&d, 0, sizeof(d));
    memset(&a, 0, sizeof(a));
    if (text == NULL || !x_parse(&d, text, n) || (root = x_child(&d, 0, "COLLADA", -1)) < 0) {
        snprintf(why, (size_t)whyLen, "Cannot read the model (COLLADA)");
        free(text); free(d.n);
        return 0;
    }
    dir_of(path, dir, sizeof(dir));
    a.d = &d; a.s = s; a.dir = dir;
    a.zUp = strcmp(x_text(&d, x_find(&d, x_child(&d, root, "asset", -1), "up_axis"), buf, sizeof(buf)), "Z_UP") == 0;
    m4_id(id);
    vs = x_find(&d, x_child(&d, root, "library_visual_scenes", -1), "visual_scene");
    for (c = x_child(&d, vs, "node", -1); c >= 0; c = x_child(&d, vs, "node", c)) { dae_nodes(&a, c, -1, id); }
    for (i = 0; i < a.count; i++) {
        int x = a.n[i].xml, in;
        for (in = d.n[x].child; in >= 0; in = d.n[in].next) {
            if (x_is(&d, in, "instance_geometry")) {
                dae_mesh(&a, i, in, x_by_id(&d, x_attr(&d, in, "url", u, sizeof(u))), -1);
            } else if (x_is(&d, in, "instance_controller")) {
                int skin = x_child(&d, x_by_id(&d, x_attr(&d, in, "url", u, sizeof(u))), "skin", -1);
                if (skin >= 0) {
                    dae_mesh(&a, i, in, x_by_id(&d, x_attr(&d, skin, "source", u, sizeof(u))), skin);
                    s->j[i].name[0] = '\0'; /* (a node that carries a skinned mesh is not a bone, whatever its name says) */
                }
            }
        }
    }
    free(a.n); free(d.n); free(text);
    return 1;
}

/* ---- welding and thinning ---- */
static uint32_t hash_bytes(const void *p, size_t n) {
    const unsigned char *b = p;
    uint32_t h = 2166136261u;
    while (n--) { h = (h ^ *b++) * 16777619u; }
    return h;
}

/* Vertices with the same key become one (the first; its place the mean when `mean`). keyOf fills 32 bytes. */
static void scene_merge(Scene *s, void (*keyOf)(const Vtx *, float, unsigned char *), float cell, int mean, size_t klen) {
    int cap = 1, i, nv = 0, nt = 0;
    int *slot, *map = malloc(sizeof(int) * (size_t)s->nv), *cnt = calloc((size_t)s->nv, sizeof(int));
    unsigned char (*keys)[32] = malloc(32 * (size_t)s->nv);
    while (cap < s->nv * 2) { cap *= 2; }
    slot = malloc(sizeof(int) * (size_t)cap);
    memset(slot, 0xFF, sizeof(int) * (size_t)cap);
    for (i = 0; i < s->nv; i++) {
        unsigned char key[32];
        uint32_t h;
        keyOf(&s->v[i], cell, key);
        h = hash_bytes(key, klen) & (uint32_t)(cap - 1);
        while (slot[h] >= 0 && memcmp(keys[slot[h]], key, klen) != 0) { h = (h + 1) & (uint32_t)(cap - 1); }
        if (slot[h] < 0) {
            slot[h] = nv;
            memcpy(keys[nv], key, 32);
            s->v[nv] = s->v[i];
            cnt[nv] = 0;
            if (mean) { v_set(s->v[nv].p, 0, 0, 0); }
            nv++;
        }
        map[i] = slot[h];
        if (mean) {
            Vtx *t = &s->v[map[i]];
            /* (its place is read from the key: v[i] itself may be a kept vertex by now) */
            float px, py, pz;
            memcpy(&px, key + 20, 4); memcpy(&py, key + 24, 4); memcpy(&pz, key + 28, 4);
            t->p[0] += px; t->p[1] += py; t->p[2] += pz;
        }
        cnt[map[i]]++;
    }
    if (mean) {
        for (i = 0; i < nv; i++) { s->v[i].p[0] /= (float)cnt[i]; s->v[i].p[1] /= (float)cnt[i]; s->v[i].p[2] /= (float)cnt[i]; }
    }
    for (i = 0; i < s->nt; i++) {
        int a = map[s->tri[i * 3]], b = map[s->tri[i * 3 + 1]], c = map[s->tri[i * 3 + 2]];
        if (a == b || b == c || a == c) { continue; }
        s->tri[nt * 3] = a; s->tri[nt * 3 + 1] = b; s->tri[nt * 3 + 2] = c;
        s->tm[nt++] = s->tm[i];
    }
    s->nv = nv;
    s->nt = nt;
    free(slot); free(map); free(cnt); free(keys);
}

static void key_exact(const Vtx *v, float cell, unsigned char *key) {
    (void)cell;
    memcpy(key, v->p, 12); memcpy(key + 12, v->n, 12); memcpy(key + 24, v->uv, 8);
}
/* ponytail: thinning by a grid (vertices of a cell become one, texture seams kept by a coarse cell of the
   texture too). It rounds shapes off evenly; an edge-collapse simplifier would keep silhouettes better. */
static void key_cell(const Vtx *v, float cell, unsigned char *key) {
    int32_t q[5] = {(int32_t)floorf(v->p[0] / cell), (int32_t)floorf(v->p[1] / cell), (int32_t)floorf(v->p[2] / cell), (int32_t)floorf(v->uv[0] * 6),
                    (int32_t)floorf(v->uv[1] * 6)};
    memcpy(key, q, 20);
    memcpy(key + 20, v->p, 12); /* (not part of the key, which is 20 bytes: the place, for the mean) */
}

/* One thinning pass with grid cells of size `cell`. Two levels, so the surface stays closed:
   1. a cluster = a cell of space; ALL its vertices get one shared position (the mean), whatever their texture place;
   2. inside a cluster, vertices of different texture places (cells of 1/UV_CELLS of the texture) stay separate vertices at
      that same position, each with the mean texture place and normal of its own.
   Before, the position was the mean of (cell, texture cell) only: neighbours of different texture cells then sat at
   different places, triangles fell apart into patches with holes between them (a scanned model with 580,000 triangles
   thinned to 7,000 showed it). The texture seam itself is still a seam, but now it doesn't open. */
#define UV_CELLS 8
static void thin_pass(Scene *s, float cell) {
    int nv = s->nv, i, cap = 1, ncl = 0, nvar = 0, nt = 0;
    int *cslot, *vslot, *cid = malloc(sizeof(int) * (size_t)nv), *vid = malloc(sizeof(int) * (size_t)nv);
    int32_t (*ck)[3] = malloc(sizeof(int32_t) * 3 * (size_t)nv), (*vk)[3] = malloc(sizeof(int32_t) * 3 * (size_t)nv);
    double (*cp)[3] = calloc((size_t)nv, sizeof(double) * 3), (*vn)[3] = calloc((size_t)nv, sizeof(double) * 3), (*vu)[2] = calloc((size_t)nv, sizeof(double) * 2);
    int *cc = calloc((size_t)nv, sizeof(int)), *vc = calloc((size_t)nv, sizeof(int)), *vfirst = malloc(sizeof(int) * (size_t)nv);
    Vtx *nvx = malloc(sizeof(Vtx) * (size_t)nv);
    while (cap < nv * 2) { cap *= 2; }
    cslot = malloc(sizeof(int) * (size_t)cap);
    vslot = malloc(sizeof(int) * (size_t)cap);
    memset(cslot, 0xFF, sizeof(int) * (size_t)cap);
    memset(vslot, 0xFF, sizeof(int) * (size_t)cap);
    for (i = 0; i < nv; i++) {
        const Vtx *v = &s->v[i];
        int32_t q[3] = {(int32_t)floorf(v->p[0] / cell), (int32_t)floorf(v->p[1] / cell), (int32_t)floorf(v->p[2] / cell)};
        int32_t w[3];
        uint32_t h = hash_bytes(q, 12) & (uint32_t)(cap - 1);
        int k;
        while (cslot[h] >= 0 && memcmp(ck[cslot[h]], q, 12) != 0) { h = (h + 1) & (uint32_t)(cap - 1); }
        if (cslot[h] < 0) { cslot[h] = ncl; memcpy(ck[ncl], q, 12); ncl++; }
        cid[i] = cslot[h];
        for (k = 0; k < 3; k++) { cp[cid[i]][k] += v->p[k]; }
        cc[cid[i]]++;
        w[0] = cid[i];
        w[1] = (int32_t)floorf(v->uv[0] * UV_CELLS);
        w[2] = (int32_t)floorf(v->uv[1] * UV_CELLS);
        h = hash_bytes(w, 12) & (uint32_t)(cap - 1);
        while (vslot[h] >= 0 && memcmp(vk[vslot[h]], w, 12) != 0) { h = (h + 1) & (uint32_t)(cap - 1); }
        if (vslot[h] < 0) { vslot[h] = nvar; memcpy(vk[nvar], w, 12); vfirst[nvar] = i; nvar++; }
        vid[i] = vslot[h];
        for (k = 0; k < 3; k++) { vn[vid[i]][k] += v->n[k]; }
        vu[vid[i]][0] += v->uv[0];
        vu[vid[i]][1] += v->uv[1];
        vc[vid[i]]++;
    }
    for (i = 0; i < nvar; i++) {
        Vtx *o = &nvx[i];
        int c = cid[vfirst[i]], k;
        *o = s->v[vfirst[i]];
        for (k = 0; k < 3; k++) { o->p[k] = (float)(cp[c][k] / cc[c]); o->n[k] = (float)(vn[i][k] / vc[i]); }
        v_norm(o->n);
        o->uv[0] = (float)(vu[i][0] / vc[i]);
        o->uv[1] = (float)(vu[i][1] / vc[i]);
    }
    for (i = 0; i < s->nt; i++) {
        int a = vid[s->tri[i * 3]], b = vid[s->tri[i * 3 + 1]], c = vid[s->tri[i * 3 + 2]];
        /* (a corner pair in the same cluster is the same place: that triangle has no area) */
        if (cid[s->tri[i * 3]] == cid[s->tri[i * 3 + 1]] || cid[s->tri[i * 3 + 1]] == cid[s->tri[i * 3 + 2]] || cid[s->tri[i * 3]] == cid[s->tri[i * 3 + 2]]) { continue; }
        s->tri[nt * 3] = a; s->tri[nt * 3 + 1] = b; s->tri[nt * 3 + 2] = c;
        s->tm[nt++] = s->tm[i];
    }
    memcpy(s->v, nvx, sizeof(Vtx) * (size_t)nvar);
    s->nv = nvar;
    s->nt = nt;
    free(cslot); free(vslot); free(cid); free(vid); free(ck); free(vk); free(cp); free(vn); free(vu); free(cc); free(vc); free(vfirst); free(nvx);
}

/* at most `budget` triangles */
static void scene_thin(Scene *s, int budget) {
    Scene t;
    float lo, hi, size = 0;
    V3 mn = {1e30f, 1e30f, 1e30f}, mx = {-1e30f, -1e30f, -1e30f};
    int i, k, it;
    if (s->nt <= budget) { return; }
    for (i = 0; i < s->nv; i++) for (k = 0; k < 3; k++) {
        if (s->v[i].p[k] < mn[k]) { mn[k] = s->v[i].p[k]; }
        if (s->v[i].p[k] > mx[k]) { mx[k] = s->v[i].p[k]; }
    }
    for (k = 0; k < 3; k++) if (mx[k] - mn[k] > size) { size = mx[k] - mn[k]; }
    lo = size / 4096; hi = size / 8;
    t = *s;
    t.v = malloc(sizeof(Vtx) * (size_t)s->nv);
    t.tri = malloc(sizeof(int) * 3 * (size_t)s->nt);
    t.tm = malloc(sizeof(int) * (size_t)s->nt);
    for (it = 0; it < 14; it++) { /* the smallest cell that fits */
        float mid = it == 13 ? hi : sqrtf(lo * hi);
        memcpy(t.v, s->v, sizeof(Vtx) * (size_t)s->nv);
        memcpy(t.tri, s->tri, sizeof(int) * 3 * (size_t)s->nt);
        memcpy(t.tm, s->tm, sizeof(int) * (size_t)s->nt);
        t.nv = s->nv; t.nt = s->nt;
        thin_pass(&t, mid);
        if (t.nt > budget) { lo = mid; } else { hi = mid; }
    }
    free(s->v); free(s->tri); free(s->tm);
    s->v = t.v; s->tri = t.tri; s->tm = t.tm; s->nv = t.nv; s->nt = t.nt;
}

/* ---- the base fighter's skeleton ---- */
typedef struct {
    int count;
    int id[NODES], parent[NODES], depth[NODES]; /* by place in the chain; parent: a place, -1 the root */
    long at[NODES];                             /* the record, from the section's start */
    V3 origin[NODES];
    int place[NODES];                           /* by node id: its place, -1 none */
} Skel;

static int skel_read(const unsigned char *m, long n, Skel *k) {
    long o;
    int stack[NODES], sp = 0, i;
    memset(k, 0, sizeof(*k));
    memset(k->place, 0xFF, sizeof(k->place));
    if (n < 0x74 || memcmp(m, "pmdl", 4) != 0) { return 0; }
    o = (long)rd32(m + 0x6C);
    for (i = 0; i < NODES; i++) {
        int32_t next;
        int pop, last, id;
        if (o < 0x70 || o + 0x40 > n) { return 0; }
        next = (int32_t)rd32(m + o);
        pop = m[o + 4] | m[o + 5] << 8; last = m[o + 6] | m[o + 7] << 8; id = m[o + 10] | m[o + 11] << 8;
        k->id[i] = id; k->at[i] = o;
        k->parent[i] = sp > 0 ? stack[sp - 1] : -1;
        k->depth[i] = sp;
        v_set(k->origin[i], rdf(m + o + 0x10), rdf(m + o + 0x14), rdf(m + o + 0x18));
        if (id < NODES && k->place[id] < 0) { k->place[id] = i; }
        stack[sp++] = i;
        sp -= pop <= sp ? pop : sp;
        k->count = i + 1;
        if (last) { return 1; }
        if (next < 0x40) { return 0; }
        o += next;
    }
    return 0;
}

/* The humanoid nodes of every fighter (the animations go by these numbers). */
enum { N_PELVIS = 3, N_RTHIGH = 9, N_RSHIN = 10, N_RFOOT = 11, N_LTHIGH = 13, N_LSHIN = 14, N_LFOOT = 15, N_SPINE = 16, N_CHEST = 17,
       N_RSHO = 18, N_RUARM = 19, N_RFARM = 20, N_RHAND = 21, N_LSHO = 32, N_LUARM = 33, N_LFARM = 34, N_LHAND = 35, N_NECK = 46, N_HEAD = 47 };
static const struct { int node, segChild, up; } kHum[] = { /* parent first; up: the humanoid node it hangs on */
    {N_PELVIS, -1, -1},        {N_SPINE, N_CHEST, N_PELVIS}, {N_CHEST, N_NECK, N_SPINE},   {N_NECK, N_HEAD, N_CHEST},   {N_HEAD, -1, N_NECK},
    {N_RSHO, N_RUARM, N_CHEST}, {N_RUARM, N_RFARM, N_RSHO},   {N_RFARM, N_RHAND, N_RUARM},  {N_RHAND, -1, N_RFARM},
    {N_LSHO, N_LUARM, N_CHEST}, {N_LUARM, N_LFARM, N_LSHO},   {N_LFARM, N_LHAND, N_LUARM},  {N_LHAND, -1, N_LFARM},
    {N_RTHIGH, N_RSHIN, N_PELVIS}, {N_RSHIN, N_RFOOT, N_RTHIGH}, {N_RFOOT, -1, N_RSHIN},
    {N_LTHIGH, N_LSHIN, N_PELVIS}, {N_LSHIN, N_LFOOT, N_LTHIGH}, {N_LFOOT, -1, N_LSHIN}};
#define HUM (int)(sizeof(kHum) / sizeof(kHum[0]))

/* 1 when the name has this word between marks ("l_te_r" has "te") */
static int name_word(const char *raw, const char *w) {
    size_t l = strlen(w), i;
    for (i = 0; raw[i] != '\0'; i++) {
        if ((i == 0 || !isalnum((unsigned char)raw[i - 1])) && strlen(raw + i) >= l && !isalnum((unsigned char)raw[i + l])) {
            size_t k;
            for (k = 0; k < l && tolower((unsigned char)raw[i + k]) == w[k]; k++) {}
            if (k == l) { return 1; }
        }
    }
    return 0;
}

/* Which humanoid node a bone's name means (-1 none): Mixamo, 3ds Max biped, Unreal, VRM, Blender names, and the
   Japanese words of arcade and console rips (kosi, hara, mune, kubi, kao, waki, kata, hiji, te, momo, hiza, asi). */
static int name_node(const char *raw) {
    char s[64];
    const char *colon = strrchr(raw, ':');
    int n = 0, i, side = 0;
    if (colon != NULL) { raw = colon + 1; }
    for (i = 0; raw[i] != '\0'; i++) { /* a lone l / r between marks, or before a capital: the side */
        int c = tolower((unsigned char)raw[i]);
        if ((c == 'l' || c == 'r') && (i == 0 || !isalpha((unsigned char)raw[i - 1])) &&
            (!isalpha((unsigned char)raw[i + 1]) || (i == 0 && isupper((unsigned char)raw[i + 1]) && islower((unsigned char)raw[0])))) {
            side = c == 'l' ? 1 : 2;
        }
        if (isalnum((unsigned char)raw[i]) && n < 63) { s[n++] = (char)c; }
    }
    s[n] = '\0';
    if (strstr(s, "left") != NULL) { side = 1; } else if (strstr(s, "right") != NULL) { side = 2; }
#define HAS(w) (strstr(s, w) != NULL)
    if (HAS("thumb") || HAS("index") || HAS("middle") || HAS("ring") || HAS("pinky") || HAS("little") || HAS("finger") || HAS("toe") || HAS("end") ||
        HAS("nub") || HAS("headtop") || HAS("twist") || HAS("roll") || HAS("ik") || HAS("eye") || HAS("jaw")) {
        return -1;
    }
    if (side != 0) {
        int r = side == 2;
        if (HAS("forearm") || HAS("lowerarm") || HAS("elbow") || name_word(raw, "hiji")) { return r ? N_RFARM : N_LFARM; }
        if (HAS("shoulder") || HAS("clavicle") || HAS("collar") || name_word(raw, "waki")) { return r ? N_RSHO : N_LSHO; }
        if (HAS("hand") || HAS("wrist") || name_word(raw, "te")) { return r ? N_RHAND : N_LHAND; }
        if (name_word(raw, "kata")) { return r ? N_RUARM : N_LUARM; }
        if (name_word(raw, "momo")) { return r ? N_RTHIGH : N_LTHIGH; }
        if (name_word(raw, "hiza")) { return r ? N_RSHIN : N_LSHIN; }
        if (name_word(raw, "asi") || name_word(raw, "ashi")) { return r ? N_RFOOT : N_LFOOT; }
        if (HAS("arm")) { return r ? N_RUARM : N_LUARM; }
        if (HAS("upleg") || HAS("upperleg") || HAS("thigh") || HAS("hip")) { return r ? N_RTHIGH : N_LTHIGH; }
        if (HAS("leg") || HAS("calf") || HAS("shin") || HAS("knee")) { return r ? N_RSHIN : N_LSHIN; }
        if (HAS("foot") || HAS("ankle")) { return r ? N_RFOOT : N_LFOOT; }
        return -1;
    }
    if (HAS("hips") || HAS("pelvis") || name_word(raw, "kosi") || name_word(raw, "koshi")) { return N_PELVIS; }
    if (HAS("chest") || name_word(raw, "mune")) { return N_CHEST; }
    if (HAS("spine") || name_word(raw, "hara")) { return N_SPINE; }
    if (HAS("neck") || name_word(raw, "kubi")) { return N_NECK; }
    if (HAS("head") || name_word(raw, "kao") || name_word(raw, "atama")) { return N_HEAD; }
#undef HAS
    return -1;
}

/* ---- the rig ---- */
typedef struct {
    V3 *pos, *nrm;   /* every vertex in the game's space, in the base fighter's rest pose */
    int *bone;       /* node id */
    float *w;        /* share of that node; the rest is its parent's */
    V3 tgt[NODES];   /* by node id: where the node is in the result (fitSkeleton: the new skeleton) */
    int moved[NODES];
} Rig;

static int joint_depth(const Scene *s, int j) {
    int d = 0;
    while (j >= 0 && d < 1000) { j = s->j[j].parent; d++; }
    return d;
}

static int hum_at(int node) {
    int i;
    for (i = 0; i < HUM; i++) if (kHum[i].node == node) { return i; }
    return -1;
}

/* ---- the owner's choices, orientation, and joints for a file that has none ---- */
typedef struct { V3 *p; int n; V3 mn, mx; } Pts; /* points on the surface, and the whole mesh's box */

static void pts_make(const Scene *s, Pts *P) {
    int stride = s->nt > 6000 ? s->nt / 6000 : 1, i, k;
    P->p = malloc(sizeof(V3) * (size_t)(s->nt / stride + 2) * 4);
    P->n = 0;
    v_set(P->mn, 1e30f, 1e30f, 1e30f); v_set(P->mx, -1e30f, -1e30f, -1e30f);
    for (i = 0; i < s->nv; i++) for (k = 0; k < 3; k++) {
        if (s->v[i].p[k] < P->mn[k]) { P->mn[k] = s->v[i].p[k]; }
        if (s->v[i].p[k] > P->mx[k]) { P->mx[k] = s->v[i].p[k]; }
    }
    for (i = 0; i < s->nt; i += stride) { /* the centre of a triangle and three points towards its corners */
        const float *a = s->v[s->tri[i * 3]].p, *b = s->v[s->tri[i * 3 + 1]].p, *c = s->v[s->tri[i * 3 + 2]].p;
        for (k = 0; k < 3; k++) {
            P->p[P->n][k] = (a[k] + b[k] + c[k]) / 3;
            P->p[P->n + 1][k] = (2 * a[k] + b[k] + c[k]) / 4;
            P->p[P->n + 2][k] = (a[k] + 2 * b[k] + c[k]) / 4;
            P->p[P->n + 3][k] = (a[k] + b[k] + 2 * c[k]) / 4;
        }
        P->n += 4;
    }
}

/* How much a standing person the mesh looks along axis a (sign sg is up): narrow at the bottom (legs), wide in the
   upper half (shoulders, arms), narrow at the top (head). The footprint of each slice is counted in a 12 x 12 grid. */
static float up_score(const Pts *P, int a, int sg) {
    static unsigned char occ[20][144];
    int B = (a + 1) % 3, C = (a + 2) % 3, i, j;
    float ext = P->mx[a] - P->mn[a], wb = P->mx[B] - P->mn[B], wc = P->mx[C] - P->mn[C], cnt[20], lo = 0, hi = 0, top;
    if (ext < 1e-9f) { return -9; }
    memset(occ, 0, sizeof(occ));
    for (i = 0; i < P->n; i++) {
        float t = (P->p[i][a] - P->mn[a]) / ext;
        int sl, gx = wb > 1e-9f ? (int)((P->p[i][B] - P->mn[B]) / wb * 11.99f) : 0, gy = wc > 1e-9f ? (int)((P->p[i][C] - P->mn[C]) / wc * 11.99f) : 0;
        if (sg < 0) { t = 1 - t; }
        sl = (int)(t * 20);
        occ[sl < 0 ? 0 : sl > 19 ? 19 : sl][gy * 12 + gx] = 1;
    }
    for (i = 0; i < 20; i++) { for (cnt[i] = 0, j = 0; j < 144; j++) { cnt[i] += occ[i][j]; } }
    for (i = 0; i < 7; i++) { lo += cnt[i] / 7; }
    for (i = 10; i < 18; i++) { if (cnt[i] > hi) { hi = cnt[i]; } }
    top = (cnt[18] + cnt[19]) / 2;
    return hi < 1 ? -9 : (hi - lo) / hi + 0.5f * (hi - top) / hi;
}

/* How much of the mesh has no mirror image across the middle plane perpendicular to axis a (14 x 14 x 14 cells).
   Across a person (left, right) there is almost none; along the height, and front to back, there is a lot. */
static float axis_asym(const Pts *P, int a) {
    static unsigned char g[14 * 14 * 14];
    int i, x, y, z, occ = 0, miss = 0;
    memset(g, 0, sizeof(g));
    for (i = 0; i < P->n; i++) {
        int c[3], k;
        for (k = 0; k < 3; k++) {
            float w = P->mx[k] - P->mn[k];
            c[k] = w > 1e-9f ? (int)((P->p[i][k] - P->mn[k]) / w * 13.99f) : 0;
            c[k] = c[k] < 0 ? 0 : c[k] > 13 ? 13 : c[k];
        }
        g[(c[0] * 14 + c[1]) * 14 + c[2]] = 1;
    }
    for (x = 0; x < 14; x++) for (y = 0; y < 14; y++) for (z = 0; z < 14; z++) {
        int c[3] = {x, y, z};
        if (!g[(x * 14 + y) * 14 + z]) { continue; }
        occ++;
        c[a] = 13 - c[a];
        if (!g[(c[0] * 14 + c[1]) * 14 + c[2]]) { miss++; }
    }
    return occ > 0 ? (float)miss / (float)occ : 1;
}

/* Which axis is up (1 +Y, 2 +Z, 3 +X, 4 -Y, 5 -Z, 6 -X). A person is the same left and right, so the axis the mesh is most
   alike along is across; of the other two the longer is the height; the sign is where the legs are narrow and the shoulders
   wide. A mesh with no such symmetry keeps +Y unless another axis looks clearly like a standing person. */
static int auto_up(const Pts *P) {
    float as[3], second = 9, ext[3], top;
    int L = 0, a, sg, best = 1;
    for (a = 0; a < 3; a++) { as[a] = axis_asym(P, a); ext[a] = P->mx[a] - P->mn[a]; if (as[a] < as[L]) { L = a; } }
    for (a = 0; a < 3; a++) { if (a != L && as[a] < second) { second = as[a]; } }
    if (as[L] < 0.3f && as[L] < 0.6f * second) {
        int A = (L + 1) % 3, B = (L + 2) % 3, U = ext[A] >= ext[B] ? A : B;
        float lo = ext[A] >= ext[B] ? ext[B] : ext[A], hi = ext[U];
        if (lo > 1e-9f && hi / lo >= 1.25f) {
            float sp = up_score(P, U, 1), sn = up_score(P, U, -1);
            sg = sn > sp + 0.04f ? -1 : 1;
            return U == 1 ? (sg > 0 ? 1 : 4) : U == 2 ? (sg > 0 ? 2 : 5) : (sg > 0 ? 3 : 6);
        }
    }
    top = up_score(P, 1, 1);
    for (a = 0; a < 3; a++) for (sg = 1; sg >= -1; sg -= 2) {
        float sc = up_score(P, a, sg);
        if (sc > top + 0.15f) { top = sc; best = a == 1 ? (sg > 0 ? 1 : 4) : a == 2 ? (sg > 0 ? 2 : 5) : (sg > 0 ? 3 : 6); }
    }
    return best;
}

/* the matrix that turns the chosen up axis (1 +Y, 2 +Z, 3 +X, 4 -Y, 5 -Z, 6 -X) into +Y */
static void up_matrix(int up, float M[9]) {
    static const float k[7][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 0, -1, 0}, {0, -1, 0, 1, 0, 0, 0, 0, 1},
                                  {-1, 0, 0, 0, -1, 0, 0, 0, 1}, {1, 0, 0, 0, 0, -1, 0, 1, 0}, {0, 1, 0, -1, 0, 0, 0, 0, 1}};
    memcpy(M, k[up < 1 || up > 6 ? 0 : up], 36);
}
/* the turn about Y by 0, 90, 180, 270 degrees (turn 1..4) */
static void yaw_matrix(int turn, float M[9]) {
    static const float c[4] = {1, 0, -1, 0}, s[4] = {0, 1, 0, -1};
    int t = (turn - 1) & 3;
    M[0] = c[t]; M[1] = 0; M[2] = s[t]; M[3] = 0; M[4] = 1; M[5] = 0; M[6] = -s[t]; M[7] = 0; M[8] = c[t];
}

static void scene_xform(Scene *s, const float M[9]) {
    int i;
    for (i = 0; i < s->nv; i++) { m_mul(s->v[i].p, M, s->v[i].p); m_mul(s->v[i].n, M, s->v[i].n); }
    for (i = 0; i < s->nj; i++) { m_mul(s->j[i].pos, M, s->j[i].pos); }
}

/* The turn (1..4, see yaw_matrix) that puts a standing mesh's front towards +Z; mesh in Y up. Which of the two floor axes
   is across the body: the arms' reach, else the gap between the legs. The front: the feet reach forward of the shins. */
static int auto_front(const Pts *P, int *sure) {
    float H = P->mx[1] - P->mn[1], ext[2][2] = {{1e30f, -1e30f}, {1e30f, -1e30f}}, bins[2][24], foot = 0, shin = 0, d;
    int i, a, lat, fwd, nf = 0, ns = 0, k;
    *sure = 0;
    if (H < 1e-9f) { return 1; }
    memset(bins, 0, sizeof(bins));
    for (i = 0; i < P->n; i++) {
        float t = (P->p[i][1] - P->mn[1]) / H;
        for (a = 0; a < 2; a++) {
            float x = P->p[i][a * 2];
            if (t >= 0.55f && t <= 0.9f) {
                if (x < ext[a][0]) { ext[a][0] = x; }
                if (x > ext[a][1]) { ext[a][1] = x; }
            }
            if (t >= 0.08f && t <= 0.3f) {
                float w = P->mx[a * 2] - P->mn[a * 2];
                int b = w > 1e-9f ? (int)((x - P->mn[a * 2]) / w * 23.99f) : 0;
                bins[a][b < 0 ? 0 : b > 23 ? 23 : b]++;
            }
        }
    }
    {
        float ex = ext[0][1] - ext[0][0], ez = ext[1][1] - ext[1][0];
        if (ex > 1.4f * ez) { lat = 0; } else if (ez > 1.4f * ex) { lat = 1; } else { /* the deeper valley between two legs */
            float v[2];
            for (a = 0; a < 2; a++) {
                float mid = 0, side = 0;
                for (k = 9; k < 15; k++) { mid += bins[a][k]; }
                for (k = 0; k < 8; k++) { side += bins[a][k] + bins[a][23 - k]; }
                v[a] = (mid / 6) / (side / 16 + 1e-6f);
            }
            lat = v[0] <= v[1] ? 0 : 1;
        }
    }
    fwd = 1 - lat; /* 0 = the X axis, 1 = the Z axis */
    for (i = 0; i < P->n; i++) {
        float t = (P->p[i][1] - P->mn[1]) / H, f = P->p[i][fwd * 2];
        if (t < 0.07f) { foot += f; nf++; } else if (t > 0.13f && t < 0.33f) { shin += f; ns++; }
    }
    if (nf == 0 || ns == 0) { return fwd == 1 ? 1 : 4; }
    d = foot / (float)nf - shin / (float)ns;
    *sure = fabsf(d) > 0.012f * H;
    /* front +Z: no turn; -Z: 180; +X: 270; -X: 90 */
    if (fwd == 1) { return d >= 0 ? 1 : 3; }
    return d >= 0 ? 4 : 2;
}

/* the file's own skeleton, when it names the pelvis, the head and a left and right pair: the turn that stands it
   (head over pelvis, left on +X); 0 when it has none */
static int skel_basis(const Scene *s, float M[9]) {
    int pel = -1, head = -1, l = -1, r = -1, ls = -1, rs = -1, i;
    V3 u, lat, f;
    float d;
    for (i = 0; i < s->nj; i++) {
        int node = name_node(s->j[i].name);
        int *slot = node == N_PELVIS ? &pel : node == N_HEAD ? &head : node == N_LTHIGH ? &l : node == N_RTHIGH ? &r : node == N_LUARM ? &ls : node == N_RUARM ? &rs : NULL;
        if (slot != NULL && (*slot < 0 || joint_depth(s, i) < joint_depth(s, *slot))) { *slot = i; }
    }
    if (pel < 0 || head < 0 || (!(l >= 0 && r >= 0) && !(ls >= 0 && rs >= 0))) { return 0; }
    v_sub(u, s->j[head].pos, s->j[pel].pos);
    if (l >= 0 && r >= 0) { v_sub(lat, s->j[l].pos, s->j[r].pos); } else { v_sub(lat, s->j[ls].pos, s->j[rs].pos); }
    if (!v_norm(u) || !v_norm(lat)) { return 0; }
    d = v_dot(lat, u);
    v_mad(lat, lat, u, -d);
    if (!v_norm(lat)) { return 0; }
    f[0] = lat[1] * u[2] - lat[2] * u[1]; f[1] = lat[2] * u[0] - lat[0] * u[2]; f[2] = lat[0] * u[1] - lat[1] * u[0];
    if (lat[0] > 0.9f && u[1] > 0.9f && f[2] > 0.9f) { m_id(M); return 1; } /* (already so) */
    memcpy(M, lat, 12); memcpy(M + 3, u, 12); memcpy(M + 6, f, 12);
    return 1;
}

static void axis_name(int up, char *o) {
    static const char *const n[7] = {"+Y", "+Y", "+Z", "+X", "-Y", "-Z", "-X"};
    strcpy(o, n[up < 0 || up > 6 ? 0 : up]);
}

/* the mesh and its joints put upright, front towards +Z, left on +X; `found` says what was decided */
static void scene_orient(Scene *s, const MiOpts *o, char *found, int foundLen) {
    float M[9], T[9];
    Pts P;
    int up = o->up, turn = o->facing, sure = 1;
    char a[16], b[64];
    strcpy(b, "SKELETON OF THE FILE");
    m_id(M);
    if (up == 0 && turn == 0 && skel_basis(s, M)) {
        scene_xform(s, M);
    } else {
        int best = 1;
        if (up == 0) {
            pts_make(s, &P);
            best = auto_up(&P);
            free(P.p);
        } else {
            best = up;
        }
        up_matrix(best, M);
        scene_xform(s, M);
        axis_name(best, a);
        if (turn == 0) {
            pts_make(s, &P);
            turn = auto_front(&P, &sure);
            free(P.p);
        }
        yaw_matrix(turn, T);
        scene_xform(s, T);
        snprintf(b, sizeof(b), "UP %s%s  TURN %d%s", a, o->up ? "" : " (auto)", (turn - 1) * 90, o->facing ? "" : sure ? " (auto)" : " (unsure)");
    }
    if (o->mirror) { /* a left-handed file: x the other way, and the triangles the other way round */
        int i;
        for (i = 0; i < s->nv; i++) { s->v[i].p[0] = -s->v[i].p[0]; s->v[i].n[0] = -s->v[i].n[0]; }
        for (i = 0; i < s->nj; i++) { s->j[i].pos[0] = -s->j[i].pos[0]; }
        for (i = 0; i < s->nt; i++) { int t = s->tri[i * 3 + 1]; s->tri[i * 3 + 1] = s->tri[i * 3 + 2]; s->tri[i * 3 + 2] = t; }
    }
    if (found != NULL && foundLen > 0) { snprintf(found, (size_t)foundLen, "%s%s", b, o->mirror ? "  MIRRORED" : ""); }
}

/* ---- joints for a file that has none ----
 * The mesh is upright, front +Z, left +X (scene_orient). Joints are placed with the base fighter's own proportions,
 * measured where the mesh says otherwise (arms' reach, torso and leg width), and then every vertex is bound to its
 * nearest bone (a vertex on the other side of the body than a limb does not take that limb), smoothed along the mesh so
 * that a sleeve or a skirt moves as one piece. The joints get the names a Mixamo skeleton has, so everything after this
 * is what a file with a skeleton gets: each bone turned to lie as the fighter's does. */
enum { J_HIPS, J_SPINE, J_CHEST, J_NECK, J_HEAD, J_LSHO, J_LARM, J_LFARM, J_LHAND, J_RSHO, J_RARM, J_RFARM, J_RHAND, J_LTHIGH, J_LSHIN, J_LFOOT, J_RTHIGH, J_RSHIN,
       J_RFOOT, J_N };
static const char *const kJName[J_N] = {"Hips", "Spine", "Chest", "Neck", "Head", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm",
                                        "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot"};
static const int kJParent[J_N] = {-1, 0, 1, 2, 3, 2, 5, 6, 7, 2, 9, 10, 11, 0, 13, 14, 0, 16, 17};
static const int kJChild[J_N] = {1, 2, 3, 4, -1, 6, 7, 8, -1, 10, 11, 12, -1, 14, 15, -1, 17, 18, -1};
static const int kJSide[J_N] = {0, 0, 0, 0, 0, 1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, -1, -1, -1};
static const int kJNode[J_N] = {N_PELVIS, N_SPINE, N_CHEST, N_NECK, N_HEAD, N_LSHO, N_LUARM, N_LFARM, N_LHAND, N_RSHO, N_RUARM, N_RFARM, N_RHAND, N_LTHIGH, N_LSHIN,
                                N_LFOOT, N_RTHIGH, N_RSHIN, N_RFOOT};

/* what was measured, in percent of the mesh's height from its feet (and the arms' angle in degrees) */
typedef struct { int waist, shoulder, neck, arms; } Fit;

/* the base fighter's height of a node as a fraction of a fighter whose head's centre is at 87% */
static float tmpl_y(const Skel *k, int node) {
    float head = -k->origin[k->place[N_HEAD]][1];
    return k->place[node] >= 0 && head > 1e-6f ? -k->origin[k->place[node]][1] / head * 0.87f : 0;
}
static float tmpl_x(const Skel *k, int node) {
    float head = -k->origin[k->place[N_HEAD]][1];
    return k->place[node] >= 0 && head > 1e-6f ? fabsf(k->origin[k->place[node]][0]) / head * 0.87f : 0;
}

/* the joint list and each vertex's weights; 1 done, 0 not a mesh this can do (then the old binding is used) */
static int autorig_make(Scene *s, const Skel *k, const MiOpts *o, Fit *fit) {
    Pts P;
    V3 jp[J_N], seg[J_N][2];
    float H, y0, cx = 0, zc = 0, wy, sy, ny, hy, th, sx, armL, legX, hipX, sigma;
    float (*cost)[J_N];
    int *label, *deg, *adj, *start, i, j, n, it, q;
    int band;
    if (s->nv < 12 || s->nt < 8) { return 0; }
    for (j = 0; j < J_N; j++) { if (k->place[kJNode[j]] < 0) { return 0; } }
    pts_make(s, &P);
    y0 = P.mn[1];
    H = P.mx[1] - y0;
    if (H < 1e-9f) { free(P.p); return 0; }
    {   /* the middle of the body: the legs' and hips' band */
        double sxs = 0, szs = 0;
        int c = 0;
        for (i = 0; i < P.n; i++) {
            float t = (P.p[i][1] - y0) / H;
            if (t > 0.08f && t < 0.45f) { sxs += P.p[i][0]; c++; }
            if (t > 0.45f && t < 0.9f) { szs += P.p[i][2]; }
        }
        cx = c > 0 ? (float)(sxs / c) : (P.mn[0] + P.mx[0]) / 2;
        { int c2 = 0; for (i = 0; i < P.n; i++) { float t = (P.p[i][1] - y0) / H; if (t > 0.45f && t < 0.9f) { c2++; } } zc = c2 > 0 ? (float)(szs / c2) : (P.mn[2] + P.mx[2]) / 2; }
    }
    wy = (o->waist > 0 ? o->waist / 100.0f : tmpl_y(k, N_PELVIS));
    sy = (o->shoulder > 0 ? o->shoulder / 100.0f : tmpl_y(k, N_RSHO));
    ny = (o->neck > 0 ? o->neck / 100.0f : tmpl_y(k, N_NECK));
    if (o->neck <= 0) { /* the neck is where the width is least between the shoulders and the head's top, if the head above is wider */
        float wmin[100], wmax[100], best = 1e30f, hd = 0;
        int bi = -1, sl;
        for (sl = 0; sl < 100; sl++) { wmin[sl] = 1e30f; wmax[sl] = -1e30f; }
        for (i = 0; i < P.n; i++) {
            float t = (P.p[i][1] - y0) / H;
            sl = (int)(t * 100);
            if (sl >= 0 && sl < 100) { wmin[sl] = fminf(wmin[sl], P.p[i][0]); wmax[sl] = fmaxf(wmax[sl], P.p[i][0]); }
        }
        for (sl = (int)((sy + 0.03f) * 100); sl < 92 && sl < 99; sl++) {
            float w = wmax[sl] - wmin[sl] + wmax[sl + 1] - wmin[sl + 1];
            if (wmax[sl] > wmin[sl] && wmax[sl + 1] > wmin[sl + 1] && w < best) { best = w; bi = sl; }
        }
        for (sl = bi + 3; bi >= 0 && sl < 98; sl++) { if (wmax[sl] - wmin[sl] > hd) { hd = wmax[sl] - wmin[sl]; } }
        if (bi >= 0 && hd > 1.25f * best / 2) { ny = (float)bi / 100.0f + 0.01f; }
    }
    hy = ny + (tmpl_y(k, N_HEAD) - tmpl_y(k, N_NECK));
    if (sy > ny - 0.01f) { sy = ny - 0.01f; }
    if (wy > sy - 0.05f) { wy = sy - 0.05f; }
    {   /* the torso's half width just under the shoulders (arms out: the torso alone; arms down: with them) */
        float lim[2] = {sy - 0.16f, sy - 0.06f}, w = 0, tmplw = tmpl_x(k, N_RUARM);
        int c = 0;
        float *ws = malloc(sizeof(float) * (size_t)(P.n + 1));
        for (i = 0; i < P.n; i++) {
            float t = (P.p[i][1] - y0) / H;
            if (t >= lim[0] && t <= lim[1]) { ws[c++] = fabsf(P.p[i][0] - cx); }
        }
        if (c > 8) { /* the 85th percentile */
            for (i = 0; i < c; i++) for (j = i + 1; j < c; j++) if (ws[j] < ws[i]) { float t = ws[i]; ws[i] = ws[j]; ws[j] = t; }
            w = ws[(int)((float)c * 0.85f)] / H;
        }
        free(ws);
        sx = (w > 0 ? w * 0.95f : tmplw) * H;
        if (sx < 0.5f * tmplw * H) { sx = 0.5f * tmplw * H; }
        if (sx > 1.6f * tmplw * H) { sx = 1.6f * tmplw * H; }
    }
    {   /* the arms' angle below horizontal: from the farthest point out to the side, seen from the shoulder */
        float ang = 0;
        if (o->arms > 0) {
            ang = (float)(o->arms - 1) * 5;
        } else {
            float bx = 0, mx2 = 0, my2 = 0;
            int c3 = 0;
            for (i = 0; i < P.n; i++) {
                float t = (P.p[i][1] - y0) / H, dx = fabsf(P.p[i][0] - cx);
                if (t > 0.3f && dx > bx) { bx = dx; }
            }
            for (i = 0; i < P.n; i++) { /* the outer third of the reach: where the arm's hand end is */
                float t = (P.p[i][1] - y0) / H, dx = fabsf(P.p[i][0] - cx);
                if (t > 0.3f && dx > sx + 0.66f * (bx - sx) && bx > sx) { mx2 += dx; my2 += P.p[i][1]; c3++; }
            }
            if (c3 > 0) { mx2 /= (float)c3; my2 /= (float)c3; } else { mx2 = bx; my2 = y0 + sy * H; }
            ang = atan2f((y0 + sy * H) - my2, mx2 - sx > 0.02f * H ? mx2 - sx : 0.02f * H) * 57.29578f;
            if (bx > 0.38f * H && ang > 25) { ang = 25; } /* (reaching that far out: arms out, with sleeves or hands hanging) */
            ang = ang < 0 ? 0 : ang > 90 ? 90 : ang;
            ang = floorf(ang / 5 + 0.5f) * 5;
        }
        fit->arms = (int)ang;
    }
    th = (float)fit->arms * 0.0174533f;
    {   /* arm length from the shoulder joint to the hand: the farthest surface point near the arm's axis */
        float tl = (tmpl_x(k, N_RHAND) - tmpl_x(k, N_RUARM)) * H, best = 0;
        armL = tl;
        if (fit->arms <= 50) {
            V3 d = {cosf(th), -sinf(th), 0}, S = {cx + sx, y0 + sy * H, zc};
            for (i = 0; i < P.n; i++) {
                V3 r, perp;
                float a;
                v_sub(r, P.p[i], S);
                r[0] = fabsf(P.p[i][0] - cx) - sx;
                a = v_dot(r, d);
                v_mad(perp, r, d, -a);
                if (a > best && v_len(perp) < 0.07f * H) { best = a; }
            }
            if (best > 0.6f * tl && best < 1.9f * tl) { armL = best * 0.92f; }
        }
    }
    {   /* the legs' middle: where the mesh's own legs stand */
        double sum[2] = {0, 0};
        int c[2] = {0, 0};
        for (i = 0; i < P.n; i++) {
            float t = (P.p[i][1] - y0) / H, dx = P.p[i][0] - cx;
            if (t > 0.10f && t < 0.30f) { int sd = dx > 0 ? 0 : 1; sum[sd] += fabsf(dx); c[sd]++; }
        }
        legX = c[0] > 3 && c[1] > 3 ? (float)((sum[0] / c[0] + sum[1] / c[1]) / 2) : tmpl_x(k, N_RSHIN) * H;
        if (legX < 0.03f * H) { legX = 0.03f * H; }
        if (legX > 0.2f * H) { legX = 0.2f * H; }
        hipX = legX * 0.6f;
    }
    fit->waist = (int)(wy * 100 + 0.5f); fit->shoulder = (int)(sy * 100 + 0.5f); fit->neck = (int)(ny * 100 + 0.5f);
    {   /* the joints, left (+X) first, in the mesh's own pose */
        int sd;
        float cl = tmpl_x(k, N_RSHO) * H, kz = tmpl_y(k, N_HEAD) / (-k->origin[k->place[N_HEAD]][1]) * H;
        /* front to back the joints lie as the fighter's do, so a mesh that stands as it does is not turned at all */
#define ZF(jn) (zc + kz * (-k->origin[k->place[kJNode[jn]]][2] + k->origin[k->place[N_PELVIS]][2]))
        v_set(jp[J_HIPS], cx, y0 + wy * H, ZF(J_HIPS));
        v_set(jp[J_SPINE], cx, y0 + (wy + (sy - wy) * 0.28f) * H, ZF(J_SPINE));
        v_set(jp[J_CHEST], cx, y0 + (wy + (sy - wy) * 0.63f) * H, ZF(J_CHEST));
        v_set(jp[J_NECK], cx, y0 + ny * H, ZF(J_NECK));
        v_set(jp[J_HEAD], cx, y0 + hy * H, ZF(J_HEAD));
        for (sd = 0; sd < 2; sd++) {
            float sg = sd == 0 ? 1.0f : -1.0f;
            int b = sd == 0 ? J_LSHO : J_RSHO, l = sd == 0 ? J_LTHIGH : J_RTHIGH;
            V3 d = {sg * cosf(th), -sinf(th), 0};
            v_set(jp[b], cx + sg * cl, y0 + sy * H, ZF(b));
            v_set(jp[b + 1], cx + sg * sx, y0 + sy * H, ZF(b + 1));
            v_mad(jp[b + 2], jp[b + 1], d, armL * 0.515f);
            jp[b + 2][2] += ZF(b + 2) - ZF(b + 1);
            v_mad(jp[b + 3], jp[b + 1], d, armL);
            jp[b + 3][2] += ZF(b + 3) - ZF(b + 1);
            v_set(jp[l], cx + sg * hipX, y0 + (wy - 0.015f) * H, ZF(l));
            v_set(jp[l + 1], cx + sg * legX, y0 + tmpl_y(k, N_RSHIN) * H, ZF(l + 1));
            v_set(jp[l + 2], cx + sg * legX, y0 + tmpl_y(k, N_RFOOT) * H, ZF(l + 2));
        }
    }
    for (j = 0; j < J_N; j++) { /* each bone as a segment: to its child, or past its end */
        v_cp(seg[j][0], jp[j]);
        if (kJChild[j] >= 0) { v_cp(seg[j][1], jp[kJChild[j]]); } else if (j == J_HEAD) { v_set(seg[j][1], cx, P.mx[1], zc); }
        else if (j == J_LFOOT || j == J_RFOOT) { v_set(seg[j][1], jp[j][0], P.mn[1] + 0.01f * H, jp[j][2] + 0.1f * H); }
        else { V3 d; v_sub(d, jp[j], jp[kJParent[j]]); v_mad(seg[j][1], jp[j], d, 0.4f); }
    }
    /* adjacency of the mesh */
    n = s->nv;
    deg = calloc((size_t)n + 1, sizeof(int));
    start = malloc(sizeof(int) * ((size_t)n + 2));
    adj = malloc(sizeof(int) * 6 * (size_t)(s->nt + 1));
    for (i = 0; i < s->nt; i++) for (q = 0; q < 3; q++) { deg[s->tri[i * 3 + q]] += 2; }
    for (start[0] = 0, i = 0; i < n; i++) { start[i + 1] = start[i] + deg[i]; deg[i] = 0; }
    for (i = 0; i < s->nt; i++) for (q = 0; q < 3; q++) {
        int a = s->tri[i * 3 + q], b = s->tri[i * 3 + (q + 1) % 3];
        adj[start[a] + deg[a]++] = b;
        adj[start[b] + deg[b]++] = a;
    }
    cost = malloc(sizeof(*cost) * (size_t)n);
    label = malloc(sizeof(int) * (size_t)n);
    sigma = 0.05f * H;
    for (i = 0; i < n; i++) {
        const float *p = s->v[i].p;
        float side = (p[0] - cx), bestC = 1e30f;
        label[i] = 0;
        for (j = 0; j < J_N; j++) {
            V3 ab, ap, c;
            float l2, t, d;
            v_sub(ab, seg[j][1], seg[j][0]);
            v_sub(ap, p, seg[j][0]);
            l2 = v_dot(ab, ab);
            t = l2 > 1e-12f ? v_dot(ap, ab) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            v_mad(c, seg[j][0], ab, t);
            v_sub(c, p, c);
            d = v_len(c);
            if (kJSide[j] != 0) { /* a limb of the other side of the body is a long way off */
                float over = -side * (float)kJSide[j] - 0.01f * H;
                if (over > 0) { d += over * 4; }
            }
            if (j == J_HEAD && fabsf(side) < 0.28f * H) { /* the head is what is above the neck, in the middle */
                float over = p[1] - jp[J_NECK][1];
                d = over >= 0 ? d * 0.3f : over > -0.03f * H ? d : d + (-over) * 2;
            } else if (p[1] >= jp[J_NECK][1] && fabsf(side) < 0.28f * H) {
                d += (p[1] - jp[J_NECK][1]) * 2 + 0.05f * H;
            }
            cost[i][j] = d;
            if (d < bestC) { bestC = d; label[i] = j; }
        }
    }
    for (it = 0; it < 6; it++) { /* along the mesh: a vertex goes with most of its neighbours unless a bone is much nearer */
        for (i = 0; i < n; i++) {
            float bestE = 1e30f;
            int votes[J_N], d = deg[i], best = label[i];
            if (d == 0) { continue; }
            memset(votes, 0, sizeof(votes));
            for (q = 0; q < d; q++) { votes[label[adj[start[i] + q]]]++; }
            for (j = 0; j < J_N; j++) {
                float e = cost[i][j] / sigma - 1.2f * (float)votes[j] / (float)d;
                if (e < bestE) { bestE = e; best = j; }
            }
            label[i] = best;
        }
    }
    band = 0;
    /* the joints become the scene's own */
    free(s->j);
    s->nj = J_N;
    s->j = calloc(J_N + 1, sizeof(Joint));
    for (j = 0; j < J_N; j++) {
        snprintf(s->j[j].name, sizeof(s->j[j].name), "%s", kJName[j]);
        s->j[j].parent = kJParent[j];
        v_cp(s->j[j].pos, jp[j]);
    }
    for (i = 0; i < n; i++) { /* the bone, and near a joint a share of its neighbour */
        Vtx *v = &s->v[i];
        const float *p = v->p;
        V3 ab, ap;
        float l2, t;
        j = label[i];
        v_sub(ab, seg[j][1], seg[j][0]);
        v_sub(ap, p, seg[j][0]);
        l2 = v_dot(ab, ab);
        t = l2 > 1e-12f ? v_dot(ap, ab) / l2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        memset(v->jb, 0xFF, sizeof(v->jb));
        memset(v->jw, 0, sizeof(v->jw));
        v->jb[0] = j; v->jw[0] = 1;
        if (t < 0.35f && kJParent[j] >= 0) {
            v->jb[1] = kJParent[j]; v->jw[1] = 0.5f * (1 - t / 0.35f); v->jw[0] = 1 - v->jw[1];
        } else if (t > 0.65f && kJChild[j] >= 0) {
            v->jb[1] = kJChild[j]; v->jw[1] = 0.5f * ((t - 0.65f) / 0.35f); v->jw[0] = 1 - v->jw[1];
        }
    }
    (void)band;
    free(cost); free(label); free(deg); free(adj); free(start); free(P.p);
    return 1;
}

/* Returns 2 when the file's own skeleton was used, 1 when the mesh was bound by distance alone. */
static int rig_make(Scene *s, const Skel *k, int fitSkeleton, Rig *r) {
    int primary[NODES]; /* by node: the joint that stands for it */
    int *jnode = malloc(sizeof(int) * (size_t)(s->nj + 1));
    int i, h, q, rigged = 1, spines = 0, named = 0;
    V3 mn = {1e30f, 1e30f, 1e30f}, mx = {-1e30f, -1e30f, -1e30f};
    float scale, headY;
    int headPlace = k->place[N_HEAD];

    memset(primary, 0xFF, sizeof(primary));
    r->pos = malloc(sizeof(V3) * (size_t)s->nv); r->nrm = malloc(sizeof(V3) * (size_t)s->nv);
    r->bone = malloc(sizeof(int) * (size_t)s->nv); r->w = malloc(sizeof(float) * (size_t)s->nv);
    memset(r->moved, 0, sizeof(r->moved));
    for (i = 0; i < s->nv; i++) for (q = 0; q < 3; q++) {
        if (s->v[i].p[q] < mn[q]) { mn[q] = s->v[i].p[q]; }
        if (s->v[i].p[q] > mx[q]) { mx[q] = s->v[i].p[q]; }
    }
    headY = headPlace >= 0 ? -k->origin[headPlace][1] : 16.25f;

    /* the file's bones by name; of several with one meaning the one nearest the root (spines: the first is the
       waist, the others the chest, unless a bone is called chest) */
    for (i = 0; i < s->nj; i++) {
        int node = name_node(s->j[i].name);
        s->j[i].node = node >= 0 && k->place[node] >= 0 ? node : -1;
    }
    for (i = 0; i < s->nj; i++) if (s->j[i].node == N_CHEST) { named = 1; }
    for (i = 0; i < s->nj; i++) {
        int node = s->j[i].node;
        if (node == N_SPINE) {
            spines++;
            if (!named && primary[N_SPINE] >= 0 && joint_depth(s, i) > joint_depth(s, primary[N_SPINE]) && k->place[N_CHEST] >= 0) {
                node = s->j[i].node = N_CHEST;
            }
        }
        if (node >= 0 && (primary[node] < 0 || joint_depth(s, i) < joint_depth(s, primary[node]))) { primary[node] = i; }
    }
    /* Skeletons named by their joints: with a collar and a shoulder on one side, the deeper one is the upper arm;
       with no bone called hips or pelvis, the bone both thighs hang on is the pelvis. */
    for (q = 0; q < 2; q++) {
        int sho = q ? N_LSHO : N_RSHO, arm = q ? N_LUARM : N_RUARM, deep = -1;
        if (primary[arm] >= 0 || primary[sho] < 0 || k->place[arm] < 0) { continue; }
        for (i = 0; i < s->nj; i++) if (s->j[i].node == sho && i != primary[sho] && (deep < 0 || joint_depth(s, i) > joint_depth(s, deep))) { deep = i; }
        if (deep >= 0) {
            for (i = 0; i < s->nj; i++) if (s->j[i].node == sho && i != primary[sho]) { s->j[i].node = arm; }
            primary[arm] = deep;
            for (i = 0; i < s->nj; i++) if (s->j[i].node == arm && joint_depth(s, i) < joint_depth(s, primary[arm])) { primary[arm] = i; }
        }
    }
    if (primary[N_PELVIS] < 0 && primary[N_LTHIGH] >= 0 && primary[N_RTHIGH] >= 0) {
        int p = s->j[primary[N_LTHIGH]].parent;
        if (p >= 0 && p == s->j[primary[N_RTHIGH]].parent && s->j[p].node < 0) {
            s->j[p].node = N_PELVIS;
            primary[N_PELVIS] = p;
        }
    }
    if (getenv("BT3_MODEL_LOG") != NULL) { /* what the names were taken for */
        fprintf(stderr, "bt3: model import: mesh %g..%g, %g..%g, %g..%g\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
        for (i = 0; i < s->nj; i++) {
            fprintf(stderr, "bt3: model import: bone %s (%g, %g, %g) -> node %d%s\n", s->j[i].name, s->j[i].pos[0], s->j[i].pos[1], s->j[i].pos[2], s->j[i].node,
                    s->j[i].node >= 0 && primary[s->j[i].node] == i ? " *" : "");
        }
    }
    {
        static const int need[] = {N_PELVIS, N_HEAD, N_RUARM, N_RFARM, N_LUARM, N_LFARM, N_RTHIGH, N_RSHIN, N_LTHIGH, N_LSHIN};
        for (i = 0; i < (int)(sizeof(need) / sizeof(need[0])); i++) if (primary[need[i]] < 0) { rigged = 0; }
        if (primary[N_SPINE] < 0 && primary[N_CHEST] < 0) { rigged = 0; }
    }

    if (rigged) {
        float R[HUM][9], along[HUM];
        V3 din[HUM];
        int hasDir[HUM], on[HUM], up[HUM];
        const float *hips = s->j[primary[N_PELVIS]].pos;
        V3 base = {hips[0], mn[1], hips[2]};
        scale = headY / (s->j[primary[N_HEAD]].pos[1] - mn[1] > 1e-6f ? s->j[primary[N_HEAD]].pos[1] - mn[1] : 1.0f);
        for (h = 0; h < HUM; h++) {
            int node = kHum[h].node, c = kHum[h].segChild;
            const float *jp, *dp = k->origin[k->place[node] >= 0 ? k->place[node] : 0];
            V3 dd;
            on[h] = primary[node] >= 0;
            up[h] = hum_at(kHum[h].up);
            while (up[h] >= 0 && !on[up[h]]) { up[h] = hum_at(kHum[up[h]].up); }
            m_id(R[h]);
            along[h] = scale;
            hasDir[h] = 0;
            if (!on[h]) { continue; }
            jp = s->j[primary[node]].pos;
            if (c == N_NECK && primary[c] < 0) { c = N_HEAD; }
            if (c >= 0 && primary[c] >= 0) { /* the bone runs to its child: turned to lie as the fighter's does */
                V3 di, t;
                float li;
                v_sub(t, s->j[primary[c]].pos, jp);
                li = v_len(t);
                v_game(di, t);
                v_sub(dd, k->origin[k->place[c]], dp);
                if (v_norm(di) && v_norm(dd)) {
                    m_between(R[h], di, dd);
                    v_cp(din[h], di);
                    hasDir[h] = 1;
                    v_sub(t, k->origin[k->place[c]], dp);
                    along[h] = fitSkeleton ? scale : v_len(t) / li;
                }
            } else if (up[h] >= 0) {
                memcpy(R[h], R[up[h]], sizeof(R[h]));
            }
            /* where the node ends up */
            if (!fitSkeleton) {
                v_cp(r->tgt[node], dp);
            } else if (up[h] < 0) {
                V3 t;
                v_sub(t, jp, base);
                v_game(r->tgt[node], t);
                r->tgt[node][0] *= scale; r->tgt[node][1] *= scale; r->tgt[node][2] *= scale;
            } else {
                int pn = kHum[up[h]].node;
                V3 t, g;
                v_sub(t, jp, s->j[primary[pn]].pos);
                if (kHum[up[h]].segChild == node && hasDir[up[h]]) { /* along the parent's bone, the file's length */
                    V3 pd;
                    v_sub(pd, k->origin[k->place[node]], k->origin[k->place[pn]]);
                    v_norm(pd);
                    v_mad(r->tgt[node], r->tgt[pn], pd, v_len(t) * scale);
                } else {
                    v_game(g, t);
                    m_mul(g, R[up[h]], g);
                    v_mad(r->tgt[node], r->tgt[pn], g, scale);
                }
            }
            r->moved[node] = fitSkeleton;
        }
        /* every bone of the file: the humanoid node it, or the nearest bone above it, means */
        for (i = 0; i < s->nj; i++) {
            int j = i, d = 0;
            while (j >= 0 && s->j[j].node < 0 && d++ < 1000) { j = s->j[j].parent; }
            jnode[i] = j >= 0 && s->j[j].node >= 0 ? s->j[j].node : N_PELVIS;
        }
        for (i = 0; i < s->nv; i++) {
            const Vtx *v = &s->v[i];
            int nodes[4], nn = 0, best = -1;
            float ws[4], bestScore = -1;
            V3 P = {0, 0, 0}, Nn = {0, 0, 0};
            for (q = 0; q < 4; q++) {
                int node, e;
                if (v->jw[q] <= 0 || v->jb[q] < 0 || v->jb[q] >= s->nj) { continue; }
                node = jnode[v->jb[q]];
                for (e = 0; e < nn && nodes[e] != node; e++) {}
                if (e == nn) { nodes[nn] = node; ws[nn++] = 0; }
                ws[e] += v->jw[q];
            }
            if (nn == 0) { nodes[0] = N_PELVIS; ws[0] = 1; nn = 1; }
            for (q = 0; q < nn; q++) { /* the vertex as each of its nodes carries it, mixed */
                V3 t, g, o;
                h = hum_at(nodes[q]);
                v_sub(t, v->p, s->j[primary[nodes[q]]].pos);
                v_game(g, t);
                if (hasDir[h]) {
                    float a = v_dot(g, din[h]);
                    V3 perp;
                    v_mad(perp, g, din[h], -a);
                    v_set(g, din[h][0] * a * along[h] + perp[0] * scale, din[h][1] * a * along[h] + perp[1] * scale, din[h][2] * a * along[h] + perp[2] * scale);
                } else {
                    g[0] *= scale; g[1] *= scale; g[2] *= scale;
                }
                m_mul(g, R[h], g);
                v_mad(o, r->tgt[nodes[q]], g, 1);
                v_mad(P, P, o, ws[q]);
                v_game(g, v->n);
                m_mul(g, R[h], g);
                v_mad(Nn, Nn, g, ws[q]);
            }
            v_cp(r->pos[i], P);
            if (!v_norm(Nn)) { v_set(Nn, 0, -1, 0); }
            v_cp(r->nrm[i], Nn);
            for (q = 0; q < nn; q++) { /* the pair (node, its parent in the fighter's chain) that holds the most */
                int par = k->parent[k->place[nodes[q]]], e;
                float score = ws[q];
                for (e = 0; e < nn; e++) if (par >= 0 && nodes[e] == k->id[par]) { score += ws[e]; }
                if (score > bestScore + 1e-6f || (fabsf(score - bestScore) <= 1e-6f && ws[q] > ws[best])) { bestScore = score; best = q; }
            }
            r->bone[i] = nodes[best];
            r->w[i] = ws[best] / bestScore;
        }
    } else {
        /* ponytail: no skeleton in the file. The mesh is scaled to the fighter's height and each vertex goes to the
           nearest bone of the rest pose (arms out to the sides). A mesh standing otherwise binds badly; the fix
           is finding the mesh's own joints, which this does not try. */
        V3 base = {(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2};
        V3 a[HUM], b[HUM];
        int on[HUM];
        float unit = headY / 16.25f;
        scale = headY / 0.87f / (mx[1] - mn[1] > 1e-6f ? mx[1] - mn[1] : 1.0f);
        for (h = 0; h < HUM; h++) {
            int node = kHum[h].node, c = kHum[h].segChild;
            on[h] = k->place[node] >= 0;
            if (!on[h]) { continue; }
            v_cp(a[h], k->origin[k->place[node]]);
            v_cp(b[h], a[h]);
            if (node == N_PELVIS) { c = N_SPINE; }
            if (c >= 0 && k->place[c] >= 0) {
                v_cp(b[h], k->origin[k->place[c]]);
            } else if (node == N_HEAD) {
                b[h][1] -= 2.5f * unit;
            } else if (node == N_RFOOT || node == N_LFOOT) {
                b[h][1] += 1.0f * unit; b[h][2] -= 1.5f * unit;
            } else if ((node == N_RHAND || node == N_LHAND) && k->place[node - 1] >= 0) {
                V3 d;
                v_sub(d, a[h], k->origin[k->place[node - 1]]);
                if (v_norm(d)) { v_mad(b[h], a[h], d, 1.2f * unit); }
            }
        }
        for (i = 0; i < s->nv; i++) {
            V3 t, p;
            float bestD = 1e30f;
            v_sub(t, s->v[i].p, base);
            v_game(p, t);
            p[0] *= scale; p[1] *= scale; p[2] *= scale;
            v_cp(r->pos[i], p);
            v_game(r->nrm[i], s->v[i].n);
            r->bone[i] = N_PELVIS;
            r->w[i] = 1;
            for (h = 0; h < HUM; h++) {
                V3 ab, ap, c;
                float l2, tt, d;
                if (!on[h]) { continue; }
                v_sub(ab, b[h], a[h]);
                v_sub(ap, p, a[h]);
                l2 = v_dot(ab, ab);
                tt = l2 > 1e-9f ? v_dot(ap, ab) / l2 : 0;
                tt = tt < 0 ? 0 : tt > 1 ? 1 : tt;
                v_mad(c, a[h], ab, tt);
                v_sub(c, p, c);
                d = v_dot(c, c);
                if (d < bestD) {
                    bestD = d;
                    r->bone[i] = kHum[h].node;
                    r->w[i] = kHum[h].node == N_PELVIS ? 1 : tt / 0.3f > 1 ? 1 : tt / 0.3f; /* the joint bends over the bone's first third */
                }
            }
        }
        for (h = 0; h < HUM; h++) if (on[h]) { v_cp(r->tgt[kHum[h].node], k->origin[k->place[kHum[h].node]]); }
    }
    free(jnode);
    return rigged ? 2 : 1;
}

/* Every triangle made drawable: its vertices on one node, the others rigid to that node's parent. A vertex that
   does not fit is moved up the chain (rigid to the parent), until the triangle's vertices meet. */
static void rig_fix(const Scene *s, const Skel *k, Rig *r) {
    int changed = 1, i, c, guard = 0;
    for (i = 0; i < s->nv; i++) { /* (a share of nothing: the parent's) */
        int par = k->parent[k->place[r->bone[i]]];
        if (r->w[i] < 0.02f && par >= 0) { r->bone[i] = k->id[par]; r->w[i] = 1; }
    }
    while (changed && guard++ < 256) {
        changed = 0;
        for (i = 0; i < s->nt; i++) {
            const int *t = &s->tri[i * 3];
            int deep = r->bone[t[0]], par;
            for (c = 1; c < 3; c++) if (k->depth[k->place[r->bone[t[c]]]] > k->depth[k->place[deep]]) { deep = r->bone[t[c]]; }
            par = k->parent[k->place[deep]];
            par = par >= 0 ? k->id[par] : -1;
            for (c = 0; c < 3; c++) {
                int b = r->bone[t[c]];
                if (b == deep) { continue; }
                if (b == par) {
                    if (r->w[t[c]] < 1) { r->w[t[c]] = 1; changed = 1; }
                } else { /* strangers: the deepest ones step up */
                    int e;
                    for (e = 0; e < 3; e++) if (r->bone[t[e]] == deep && par >= 0) { r->bone[t[e]] = par; r->w[t[e]] = 1; }
                    changed = 1;
                    break;
                }
            }
        }
    }
}

/* ---- textures ---- */
static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol4First[4][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63}};

/* where pixel (x, y) of a 4-bit texture lies in GS memory, in nibbles; and of a 32-bit one, in words */
static uint32_t gs_at4(int x, int y, int pagesWide) {
    int col = (y & 15) >> 2, sx = (col & 1) ? ((x & 31) ^ 4) : (x & 31);
    return (uint32_t)((((y >> 2) & ~0x1F) * pagesWide + ((x >> 2) & ~0x1F) + kBlock16[(y >> 4) & 7][(x >> 5) & 3]) << 9) + kCol4First[y & 3][sx] + (uint32_t)col * 128;
}
static uint32_t gs_at32(int x, int y, int pagesWide) {
    return (uint32_t)(((y & ~0x1F) * pagesWide + ((x >> 1) & ~0x1F) + kBlock32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kCol32[y & 7][x & 7];
}

/* A square 4-bit texture (side 64, 128 or 256; one index per byte in idx) as the 32-bit upload the game stores:
   side/2 x side/4 words. back != 0: the other way (data -> idx). */
static void tex_swizzle(unsigned char *idx, unsigned char *data, int side, int back) {
    int pages = side > 128 ? side / 128 : 1, x, y;
    uint8_t *mem = calloc(1, (size_t)side * (size_t)side / 2 + 8192);
    if (back) {
        for (y = 0; y < side / 4; y++) for (x = 0; x < side / 2; x++) { memcpy(mem + gs_at32(x, y, pages) * 4, data + (y * (side / 2) + x) * 4, 4); }
        for (y = 0; y < side; y++) for (x = 0; x < side; x++) {
            uint32_t a = gs_at4(x, y, pages);
            idx[y * side + x] = (mem[a >> 1] >> ((a & 1) * 4)) & 15;
        }
    } else {
        for (y = 0; y < side; y++) for (x = 0; x < side; x++) {
            uint32_t a = gs_at4(x, y, pages);
            mem[a >> 1] |= (uint8_t)((idx[y * side + x] & 15) << ((a & 1) * 4));
        }
        for (y = 0; y < side / 4; y++) for (x = 0; x < side / 2; x++) { memcpy(data + (y * (side / 2) + x) * 4, mem + gs_at32(x, y, pages) * 4, 4); }
    }
    free(mem);
}

/* an upload packet as the texture files hold it: a w x h rectangle of 32-bit words */
static void tex_packet(Buf *o, int w, int h, const unsigned char *data) {
    long n = (long)w * h * 4, qw = n / 16;
    unsigned char *p = buf_add(o, 0x60 + n + 0x20);
    wr32(p + 0x0C, 0x50000000u | (uint32_t)(qw + 7));
    wr32(p + 0x10, 0x8003); wr32(p + 0x14, 0x10000000); wr32(p + 0x18, 0xE);
    wr32(p + 0x28, 0x51);
    wr32(p + 0x30, (uint32_t)w); wr32(p + 0x34, (uint32_t)h); wr32(p + 0x38, 0x52);
    wr32(p + 0x48, 0x53);
    wr32(p + 0x50, 0x8000u | (uint32_t)qw); wr32(p + 0x54, 0x08000000);
    memcpy(p + 0x60, data, (size_t)n);
    wr32(p + 0x60 + n, 0x8001); wr32(p + 0x64 + n, 0x10000000); wr32(p + 0x68 + n, 0xE);
    wr32(p + 0x78 + n, 0x3F);
}

/* 16 colours for a picture (the game's format has no more): the median cut of the colours the mesh really shows (the
   texels some triangle's texture place covers, not the unused rest of an atlas), a few rounds of k-means on them, and
   small contrasting details (an eye, a jewel) counted more than their share of pixels so they keep a colour of their own.
   dither: the picture's smooth shading is spread with error diffusion instead of banding. */
static int sCutAxis;
static int cut_cmp(const void *a, const void *b) { return (int)((const unsigned char *)a)[sCutAxis] - (int)((const unsigned char *)b)[sCutAxis]; }
static int col_dist(const unsigned char *a, const int *b) {
    int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return 2 * dr * dr + 4 * dg * dg + 3 * db * db;
}
static void quantize(const unsigned char *rgb, int n, const unsigned char *cover, int dither, int side, unsigned char pal[16][3], unsigned char *idx) {
    unsigned char *px = malloc((size_t)n * 4);
    int lo[16], hi[16], boxes = 1, i, b, c, m = 0, it, mean[3] = {0, 0, 0};
    long sum3[3] = {0, 0, 0};
    for (i = 0; i < n; i++) {
        if (cover == NULL || cover[i]) { px[m * 4] = rgb[i * 3]; px[m * 4 + 1] = rgb[i * 3 + 1]; px[m * 4 + 2] = rgb[i * 3 + 2]; px[m * 4 + 3] = 1; m++; }
    }
    if (m < 32) { /* (almost nothing is shown: the whole picture) */
        for (m = 0, i = 0; i < n; i++) { memcpy(px + m * 4, rgb + i * 3, 3); px[m * 4 + 3] = 1; m++; }
    }
    for (i = 0; i < m; i++) for (c = 0; c < 3; c++) { sum3[c] += px[i * 4 + c]; }
    for (c = 0; c < 3; c++) { mean[c] = (int)(sum3[c] / (m > 0 ? m : 1)); }
    for (i = 0; i < m; i++) { /* weight 1..4 by how far the colour is from the picture's mean */
        int d = col_dist(px + i * 4, mean), w = 1 + d / 4000;
        px[i * 4 + 3] = (unsigned char)(w > 4 ? 4 : w);
    }
    lo[0] = 0; hi[0] = m;
    while (boxes < 16) {
        int best = -1, bestAxis = 0;
        long bestScore = 0;
        for (b = 0; b < boxes; b++) {
            int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
            long wsum = 0;
            if (hi[b] - lo[b] < 2) { continue; }
            for (i = lo[b]; i < hi[b]; i++) {
                wsum += px[i * 4 + 3];
                for (c = 0; c < 3; c++) {
                    if (px[i * 4 + c] < mn[c]) { mn[c] = px[i * 4 + c]; }
                    if (px[i * 4 + c] > mx[c]) { mx[c] = px[i * 4 + c]; }
                }
            }
            for (c = 0; c < 3; c++) {
                long score = (long)(mx[c] - mn[c]) * (c == 1 ? 6 : c == 0 ? 4 : 3) * 1024 + wsum / 16;
                if (mx[c] > mn[c] && score > bestScore) { bestScore = score; best = b; bestAxis = c; }
            }
        }
        if (best < 0) { break; }
        sCutAxis = bestAxis;
        qsort(px + lo[best] * 4, (size_t)(hi[best] - lo[best]), 4, cut_cmp);
        {
            int mid = (lo[best] + hi[best]) / 2;
            /* (not through a run of one value) */
            while (mid < hi[best] - 1 && px[mid * 4 + bestAxis] == px[(mid - 1) * 4 + bestAxis]) { mid++; }
            if (px[mid * 4 + bestAxis] == px[(mid - 1) * 4 + bestAxis]) {
                mid = (lo[best] + hi[best]) / 2;
                while (mid > lo[best] + 1 && px[mid * 4 + bestAxis] == px[(mid - 1) * 4 + bestAxis]) { mid--; }
            }
            lo[boxes] = mid; hi[boxes] = hi[best]; hi[best] = mid;
            boxes++;
        }
    }
    memset(pal, 0, 48);
    for (b = 0; b < boxes; b++) {
        long sum[3] = {0, 0, 0}, ws = 0;
        for (i = lo[b]; i < hi[b]; i++) for (c = 0; c < 3; c++) { sum[c] += (long)px[i * 4 + c] * px[i * 4 + 3]; ws += c == 0 ? px[i * 4 + 3] : 0; }
        for (c = 0; c < 3; c++) { pal[b][c] = (unsigned char)(sum[c] / (ws > 0 ? ws : 1)); }
    }
    for (it = 0; it < 5; it++) { /* k-means: every colour to its nearest entry, every entry to the mean of its colours */
        long acc[16][3], wt[16];
        memset(acc, 0, sizeof(acc)); memset(wt, 0, sizeof(wt));
        for (i = 0; i < m; i++) {
            int bi = 0, bd = 1 << 30, w = px[i * 4 + 3];
            for (b = 0; b < boxes; b++) {
                int pc[3] = {pal[b][0], pal[b][1], pal[b][2]}, d = col_dist(px + i * 4, pc);
                if (d < bd) { bd = d; bi = b; }
            }
            for (c = 0; c < 3; c++) { acc[bi][c] += (long)px[i * 4 + c] * w; }
            wt[bi] += w;
        }
        for (b = 0; b < boxes; b++) if (wt[b] > 0) { for (c = 0; c < 3; c++) { pal[b][c] = (unsigned char)(acc[b][c] / wt[b]); } }
    }
    if (dither) { /* Floyd-Steinberg, three quarters of the error */
        float *err = calloc((size_t)n * 3, sizeof(float));
        int x, y;
        for (y = 0; y < side; y++) for (x = 0; x < side; x++) {
            int p = y * side + x, bi = 0, bd = 1 << 30, v[3];
            for (c = 0; c < 3; c++) { float f = (float)rgb[p * 3 + c] + err[p * 3 + c]; v[c] = f < 0 ? 0 : f > 255 ? 255 : (int)f; }
            for (b = 0; b < boxes; b++) {
                unsigned char t[3] = {(unsigned char)v[0], (unsigned char)v[1], (unsigned char)v[2]};
                int pc[3] = {pal[b][0], pal[b][1], pal[b][2]}, d = col_dist(t, pc);
                if (d < bd) { bd = d; bi = b; }
            }
            idx[p] = (unsigned char)bi;
            for (c = 0; c < 3; c++) {
                float e = ((float)v[c] - (float)pal[bi][c]) * 0.75f;
                if (x + 1 < side) { err[(p + 1) * 3 + c] += e * 7 / 16; }
                if (y + 1 < side) {
                    if (x > 0) { err[(p + side - 1) * 3 + c] += e * 3 / 16; }
                    err[(p + side) * 3 + c] += e * 5 / 16;
                    if (x + 1 < side) { err[(p + side + 1) * 3 + c] += e * 1 / 16; }
                }
            }
        }
        free(err);
    } else {
        for (i = 0; i < n; i++) {
            int bi = 0, bd = 1 << 30, v[3] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
            for (b = 0; b < boxes; b++) {
                unsigned char t[3] = {(unsigned char)v[0], (unsigned char)v[1], (unsigned char)v[2]};
                int pc[3] = {pal[b][0], pal[b][1], pal[b][2]}, d = col_dist(t, pc);
                if (d < bd) { bd = d; bi = b; }
            }
            idx[i] = (unsigned char)bi;
        }
    }
    free(px);
}

/* the material's picture as side x side RGB (its colour when it has none). Transparent texels take the colour of the
   opaque ones around them, so they are not black or white seams in the palette. */
static unsigned char *mat_pixels(const Mat *m, int side) {
    unsigned char *o = malloc((size_t)side * (size_t)side * 3);
    int x, y, c;
    float avg[3] = {m->col[0] * 255, m->col[1] * 255, m->col[2] * 255};
    if (m->px != NULL) { /* the colour of what is opaque, for where nothing is */
        double s3[3] = {0, 0, 0};
        long cnt = 0, i;
        for (i = 0; i < (long)m->w * m->h; i++) {
            if (m->px[i * 4 + 3] >= 16) { s3[0] += m->px[i * 4]; s3[1] += m->px[i * 4 + 1]; s3[2] += m->px[i * 4 + 2]; cnt++; }
        }
        if (cnt > 0) { avg[0] = (float)(s3[0] / cnt); avg[1] = (float)(s3[1] / cnt); avg[2] = (float)(s3[2] / cnt); }
    }
    for (y = 0; y < side; y++) for (x = 0; x < side; x++) {
        unsigned char *d = o + (y * side + x) * 3;
        if (m->px == NULL) {
            for (c = 0; c < 3; c++) { float f = m->col[c] * 255; d[c] = (unsigned char)(f < 0 ? 0 : f > 255 ? 255 : f); }
        } else { /* the mean of the source rectangle, by opacity */
            int x0 = x * m->w / side, x1 = (x + 1) * m->w / side, y0 = y * m->h / side, y1 = (y + 1) * m->h / side, sx, sy;
            long sum[3] = {0, 0, 0}, wsum = 0;
            if (x1 <= x0) { x1 = x0 + 1; }
            if (y1 <= y0) { y1 = y0 + 1; }
            for (sy = y0; sy < y1; sy++) for (sx = x0; sx < x1; sx++) {
                const unsigned char *p = m->px + (sy * m->w + sx) * 4;
                sum[0] += p[0] * p[3]; sum[1] += p[1] * p[3]; sum[2] += p[2] * p[3]; wsum += p[3];
            }
            for (c = 0; c < 3; c++) { d[c] = wsum > 0 ? (unsigned char)(sum[c] / wsum) : (unsigned char)avg[c]; }
        }
    }
    return o;
}

/* 1 where some triangle of material `mat` has its texture place (a texel's centre within a texel of it) */
static unsigned char *uv_cover(const Scene *s, int mat, int side) {
    unsigned char *o = calloc((size_t)side * (size_t)side, 1);
    int i, c;
    for (i = 0; i < s->nt; i++) {
        float u[3], v[3], x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        int ix0, ix1, iy0, iy1, x, y;
        float tu, tv;
        if (s->tm[i] != mat) { continue; }
        tu = floorf(s->v[s->tri[i * 3]].uv[0]); tv = floorf(s->v[s->tri[i * 3]].uv[1]); /* (corners of a triangle that wraps stay with the first's tile) */
        for (c = 0; c < 3; c++) {
            const float *uv = s->v[s->tri[i * 3 + c]].uv;
            u[c] = (uv[0] - tu) * (float)side;
            v[c] = (uv[1] - tv) * (float)side;
            if (u[c] < x0) { x0 = u[c]; } if (u[c] > x1) { x1 = u[c]; }
            if (v[c] < y0) { y0 = v[c]; } if (v[c] > y1) { y1 = v[c]; }
        }
        ix0 = (int)floorf(x0) - 1; ix1 = (int)ceilf(x1) + 1; iy0 = (int)floorf(y0) - 1; iy1 = (int)ceilf(y1) + 1;
        if (ix1 - ix0 > side || iy1 - iy0 > side) { memset(o, 1, (size_t)side * (size_t)side); break; } /* (a tiling texture: all of it) */
        for (y = iy0; y <= iy1; y++) for (x = ix0; x <= ix1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, e[3];
            int k, in = 1;
            for (k = 0; k < 3; k++) { /* distance in texels to each edge, positive inside */
                int a = k, b2 = (k + 1) % 3;
                float ex = u[b2] - u[a], ey = v[b2] - v[a], len = sqrtf(ex * ex + ey * ey);
                float cr = ex * (py - v[a]) - ey * (px - u[a]);
                e[k] = len > 1e-6f ? cr / len : 0;
            }
            { /* the winding is either: inside when all three have one sign (within a texel of it) */
                int pos = 0, neg = 0;
                for (k = 0; k < 3; k++) { if (e[k] >= -1.0f) { pos++; } if (e[k] <= 1.0f) { neg++; } }
                in = pos == 3 || neg == 3;
            }
            if (in) {
                int wx = ((x % side) + side) % side, wy = ((y % side) + side) % side;
                o[wy * side + wx] = 1;
            }
        }
    }
    return o;
}

typedef struct { uint32_t tex0lo, tex0hi; } TexRef; /* TEX0 of a material's batches, block pointers from the file's first */

/* The texture file (section 12): the materials' textures, then the base fighter's face texture (the game puts the
   current expression there; no mesh uses it now) and its shading ramps. The header's texture fields in hdr. */
static int tex_build(const Scene *s, const unsigned char *d, long dn, const unsigned char *mdl, Buf *out, TexRef *ref, unsigned char *hdr, int used, int dither) {
    uint32_t dcount, dent;
    int face = mdl[0x4C], clutFirst = mdl[0x50], clutCount = mdl[0x51], keep[64], nkeep = 0, total, i, e, side;
    long tbp = 0, cbp = 0;
    Buf data = {0};
    unsigned char *ent;

    if (dn < 0x20) { return 0; }
    dcount = rd32(d); dent = rd32(d + 4) * 4;
    if (dcount > 64 || dent + dcount * 0x40 > (uint32_t)dn) { return 0; }
    if (face < (int)dcount) { keep[nkeep++] = face; }
    for (i = clutFirst; i < clutFirst + clutCount && i < (int)dcount; i++) { keep[nkeep++] = i; }
    total = used + nkeep;
    side = used <= 3 ? 256 : 128;
    buf_add(out, 0x20 + (long)total * 0x40);
    for (i = 0, e = 0; i < s->nmat; i++) {
        unsigned char pal[16][3], clut[64], *rgb, *idx, *sw, *cover;
        int sd = s->mat[i].px != NULL ? (s->paged ? s->mat[i].w : side) : 64, c;
        if (!s->mat[i].used) { continue; }
        rgb = mat_pixels(&s->mat[i], sd);
        idx = malloc((size_t)sd * (size_t)sd);
        sw = malloc((size_t)sd * (size_t)sd / 2);
        cover = uv_cover(s, i, sd);
        quantize(rgb, sd * sd, cover, dither, sd, pal, idx);
        free(cover);
        tex_swizzle(idx, sw, sd, 0);
        for (c = 0; c < 16; c++) { clut[c * 4] = pal[c][0]; clut[c * 4 + 1] = pal[c][1]; clut[c * 4 + 2] = pal[c][2]; clut[c * 4 + 3] = 0x80; }
        ent = out->b + 0x20 + e * 0x40;
        wr32(ent + 0x00, (uint32_t)((0x20 + total * 0x40 + data.n) / 4));
        tex_packet(&data, sd / 2, sd / 4, sw);
        wr32(ent + 0x08, (uint32_t)(sd * sd / 2 + 0x80));
        ent = out->b + 0x20 + e * 0x40;
        wr32(ent + 0x04, (uint32_t)((0x20 + total * 0x40 + data.n) / 4));
        tex_packet(&data, 8, 2, clut);
        wr32(ent + 0x0C, 0xC0);
        wr32(ent + 0x10, (uint32_t)(sd * sd / 2 / 256)); wr32(ent + 0x14, 1);
        wr32(ent + 0x18, sd == 256 ? 2 : sd == 128 ? 1 : 0);
        wr32(ent + 0x20, (uint32_t)tbp); wr32(ent + 0x24, (uint32_t)cbp);
        ent[0x2C] = (unsigned char)e; /* its alpha slot */
        {
            int lg = sd == 256 ? 8 : sd == 128 ? 7 : 6, tbw = sd == 256 ? 4 : sd == 128 ? 2 : 0;
            uint32_t lo = (uint32_t)tbw << 14 | 0x14u << 20 | (uint32_t)lg << 26 | (uint32_t)(lg & 3) << 30, hi = (uint32_t)(lg >> 2) | 0x20000000u;
            wr32(ent + 0x30, lo); wr32(ent + 0x34, hi);
            ref[i].tex0lo = lo | (uint32_t)tbp;
            ref[i].tex0hi = hi | 4u | (uint32_t)cbp << 5; /* (TCC: the texture's alpha, as the game's own batches have) */
        }
        tbp += sd * sd / 2 / 256; cbp += 1;
        free(rgb); free(idx); free(sw);
        e++;
    }
    for (i = 0; i < nkeep; i++, e++) { /* the kept ones, as they are */
        const unsigned char *de = d + dent + keep[i] * 0x40;
        uint32_t po = rd32(de) * 4, co = rd32(de + 4) * 4, ps = rd32(de + 8), cs = rd32(de + 12);
        int isRamp = keep[i] != face || i > 0;
        ent = out->b + 0x20 + e * 0x40;
        memcpy(ent, de, 0x40);
        if (po != 0 && po + ps <= (uint32_t)dn) {
            wr32(ent, (uint32_t)((0x20 + total * 0x40 + data.n) / 4));
            memcpy(buf_add(&data, ps), d + po, ps);
        }
        if (co != 0 && co + cs <= (uint32_t)dn) {
            ent = out->b + 0x20 + e * 0x40;
            wr32(ent + 4, (uint32_t)((0x20 + total * 0x40 + data.n) / 4));
            memcpy(buf_add(&data, cs), d + co, cs);
        }
        ent = out->b + 0x20 + e * 0x40;
        if (!isRamp) {
            wr32(ent + 0x20, (uint32_t)tbp); wr32(ent + 0x24, (uint32_t)cbp);
            ent[0x2C] = (unsigned char)used; ent[0x2D] = 0;
            tbp += rd32(de + 0x10); cbp += rd32(de + 0x14);
        }
    }
    memcpy(buf_add(out, data.n), data.b, (size_t)data.n);
    free(data.b);
    wr32(out->b, (uint32_t)total); wr32(out->b + 4, 8);
    wr32(out->b + 8, (uint32_t)tbp + (uint32_t)clutCount * 4); wr32(out->b + 12, (uint32_t)cbp);
    hdr[0x4C] = (unsigned char)(face < (int)dcount ? used : 0xFF);
    hdr[0x4D] = 0;
    hdr[0x4E] = 0;
    hdr[0x4F] = (unsigned char)(used + (face < (int)dcount));
    hdr[0x50] = (unsigned char)(used + (face < (int)dcount));
    hdr[0x51] = (unsigned char)clutCount;
    hdr[0x54] = 0;
    return 1;
}

/* ---- the mesh ---- */
typedef struct { uint64_t key; int tri; } Edge;
static int edge_cmp(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->tri - y->tri;
}
static uint64_t edge_key(int a, int b) { return a < b ? (uint64_t)a << 32 | (uint32_t)b : (uint64_t)b << 32 | (uint32_t)a; }

/* an unused triangle of the list on edge (a, b): its third vertex, -1 none */
static int edge_next(const Edge *e, int ne, const int *tris, unsigned char *usedT, int a, int b, int *tri) {
    uint64_t key = edge_key(a, b);
    int lo = 0, hi = ne;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (e[mid].key < key) { lo = mid + 1; } else { hi = mid; }
    }
    for (; lo < ne && e[lo].key == key; lo++) {
        const int *t = &tris[e[lo].tri * 3];
        if (!usedT[e[lo].tri]) {
            *tri = e[lo].tri;
            return t[0] != a && t[0] != b ? t[0] : t[1] != a && t[1] != b ? t[1] : t[2];
        }
    }
    return -1;
}

/* One batch: a strip of n vertices of the scene, for the part of node `node`. */
static void batch_put(Buf *o, const Scene *s, const Rig *r, const int *strip, int n, int node, TexRef ref, int first) {
    int i;
    if (!first) { buf_w(o, 0x17000000); }
    buf_w(o, 0x6C058000);
    buf_w(o, 1); buf_w(o, 0x10000000); buf_w(o, 0xE); buf_w(o, 0);
    buf_w(o, ref.tex0lo); buf_w(o, ref.tex0hi); buf_w(o, 6); buf_w(o, 0);
    buf_w(o, 0x21310000); buf_w(o, 0x2000000E); buf_w(o, 7); buf_w(o, 0); /* the shading ramp, the base fighter's first */
    buf_w(o, (uint32_t)n); buf_w(o, 0x302E4000); buf_w(o, 0x412); buf_w(o, 0);
    buf_w(o, 0x8000u | (uint32_t)n); buf_w(o, 0x312E4000); buf_w(o, 0x412); buf_w(o, 0);
    buf_w(o, 0x6C008005u | (uint32_t)(n * 3) << 16);
    for (i = 0; i < n; i++) {
        int v = strip[i];
        buf_f(o, r->pos[v][0]); buf_f(o, r->pos[v][1]); buf_f(o, r->pos[v][2]); buf_f(o, r->bone[v] == node ? r->w[v] : 0.0f);
        buf_f(o, r->nrm[v][0]); buf_f(o, r->nrm[v][1]); buf_f(o, r->nrm[v][2]); buf_w(o, 0);
        buf_f(o, s->v[v].uv[0]); buf_f(o, s->v[v].uv[1]); buf_f(o, 1.0f); buf_w(o, 0);
    }
}

/* Section 3 again: the base fighter's node records with the new meshes. */
static int mesh_build(const Scene *s, const Skel *k, const Rig *r, const TexRef *ref, const unsigned char *d, long dn, const unsigned char *hdr, int fitSkeleton,
                      Buf *out) {
    int *part = malloc(sizeof(int) * (size_t)s->nt), *list = malloc(sizeof(int) * 3 * (size_t)s->nt);
    Edge *edges = malloc(sizeof(Edge) * 3 * (size_t)s->nt);
    unsigned char *usedT = malloc((size_t)s->nt);
    long head = (long)rd32(d + 0x6C);
    V3 org[NODES];
    int i, c, p;

    (void)dn;
    memcpy(buf_add(out, head), d, (size_t)head);
    memcpy(out->b + 0x4C, hdr + 0x4C, 9);
    wr32(out->b + 0x70, 0); /* no seam section */
    for (i = 0; i < s->nt; i++) { /* a triangle is drawn with its deepest node */
        const int *t = &s->tri[i * 3];
        int deep = r->bone[t[0]];
        for (c = 1; c < 3; c++) if (k->depth[k->place[r->bone[t[c]]]] > k->depth[k->place[deep]]) { deep = r->bone[t[c]]; }
        part[i] = deep;
    }
    for (p = 0; p < k->count; p++) {
        const unsigned char *rec = d + k->at[p];
        int node = k->id[p], verts = 0, m, first = 1;
        long at = out->n, chain;
        V3 mn = {1e30f, 1e30f, 1e30f}, mx = {-1e30f, -1e30f, -1e30f};
        Buf ch = {0};

        /* the node's place: the new skeleton's, or where it was from its parent */
        if (fitSkeleton && r->moved[node] && k->place[node] == p) {
            v_cp(org[p], r->tgt[node]);
        } else if (fitSkeleton && k->parent[p] >= 0) {
            V3 rest = {rdf(rec + 0x30), rdf(rec + 0x34), rdf(rec + 0x38)};
            v_mad(org[p], org[k->parent[p]], rest, 1);
        } else {
            v_cp(org[p], k->origin[p]);
        }
        for (m = 0; m < s->nmat && k->place[node] == p; m++) {
            int n = 0, ne = 0;
            for (i = 0; i < s->nt; i++) if (part[i] == node && s->tm[i] == m) {
                memcpy(&list[n * 3], &s->tri[i * 3], sizeof(int) * 3);
                for (c = 0; c < 3; c++) { edges[ne].key = edge_key(list[n * 3 + c], list[n * 3 + (c + 1) % 3]); edges[ne++].tri = n; }
                n++;
            }
            if (n == 0) { continue; }
            qsort(edges, (size_t)ne, sizeof(Edge), edge_cmp);
            memset(usedT, 0, (size_t)n);
            for (i = 0; i < n; i++) { /* ponytail: greedy strips, forward only; a real stripifier would make them longer */
                int strip[MI_STRIP], len = 3, x, tri, rot;
                if (usedT[i]) { continue; }
                usedT[i] = 1;
                memcpy(strip, &list[i * 3], sizeof(int) * 3);
                for (rot = 0; rot < 3; rot++) { /* start so that the strip can go on */
                    if (edge_next(edges, ne, list, usedT, strip[1], strip[2], &tri) >= 0) { break; }
                    x = strip[0]; strip[0] = strip[1]; strip[1] = strip[2]; strip[2] = x;
                }
                while (len < MI_STRIP && (x = edge_next(edges, ne, list, usedT, strip[len - 2], strip[len - 1], &tri)) >= 0) {
                    usedT[tri] = 1;
                    strip[len++] = x;
                }
                if (ch.n == 0) { buf_w(&ch, 0); buf_w(&ch, 0); }
                batch_put(&ch, s, r, strip, len, node, ref[m], first);
                first = 0;
                verts += len;
                for (x = 0; x < len; x++) for (c = 0; c < 3; c++) {
                    if (r->pos[strip[x]][c] < mn[c]) { mn[c] = r->pos[strip[x]][c]; }
                    if (r->pos[strip[x]][c] > mx[c]) { mx[c] = r->pos[strip[x]][c]; }
                }
            }
        }
        {
            unsigned char *o;
            chain = 0;
            if (verts > 0) {
                buf_w(&ch, 0x17000000);
                while (ch.n % 16 != 0) { buf_w(&ch, 0); }
                wr32(ch.b, 0x60000000u | (uint32_t)(ch.n / 16 - 1));
                wr32(buf_add(&ch, 16), 0x70000000);
                chain = ch.n;
            }
            o = buf_add(out, 0x40 + (chain > 0 ? 0x20 + chain : 0));
            memcpy(o, rec, 0x40);
            wr32(o, (uint32_t)(0x40 + (chain > 0 ? 0x20 + chain : 0)));
            o[8] = (unsigned char)(verts > 0xFFFF ? 0xFF : verts); o[9] = (unsigned char)((verts > 0xFFFF ? 0xFFFF : verts) >> 8);
            o[0x0C] = o[0x0D] = o[0x0E] = o[0x0F] = 0;
            if (fitSkeleton) {
                const float *po = k->parent[p] >= 0 ? org[k->parent[p]] : org[p];
                for (c = 0; c < 3; c++) {
                    wrf(o + 0x10 + c * 4, org[p][c]);
                    wrf(o + 0x20 + c * 4, po[c]);
                    wrf(o + 0x30 + c * 4, org[p][c] - po[c]);
                }
            }
            if (chain > 0) {
                float rad = 0;
                for (c = 0; c < 3; c++) {
                    wrf(o + 0x40 + c * 4, (mn[c] + mx[c]) / 2);
                    wrf(o + 0x50 + c * 4, (mx[c] - mn[c]) / 2);
                    rad += (mx[c] - mn[c]) * (mx[c] - mn[c]) / 4;
                }
                wrf(o + 0x4C, 1.0f);
                wrf(o + 0x5C, sqrtf(rad));
                memcpy(o + 0x60, ch.b, (size_t)chain);
            }
            (void)at;
        }
        free(ch.b);
    }
    free(part); free(list); free(edges); free(usedT);
    return 1;
}

/* ---- the pack ---- */
static long sec_at(const unsigned char *b, long n, int i) {
    long a = (long)(rd32(b + 4 * i) & ~3u);
    return a < n ? a : n;
}

/* the pack with sections 3 and 12 replaced, every section on a 64-byte boundary (as charedit.c lays them out) */
static unsigned char *pack_replace(const unsigned char *b, long n, const Buf *mesh, const Buf *tex, long *outLen) {
    int count = (int)rd32(b), i;
    long head = (long)(rd32(b + 4) & ~3u), pos;
    Buf o = {0};
    buf_add(&o, head);
    wr32(o.b, (uint32_t)count);
    for (i = 1; i <= count; i++) {
        const unsigned char *src = i == 3 ? mesh->b : i == 12 ? tex->b : b + sec_at(b, n, i);
        long len = i == 3 ? mesh->n : i == 12 ? tex->n : sec_at(b, n, i + 1) - sec_at(b, n, i);
        if (len < 0) { len = 0; }
        buf_add(&o, (64 - o.n % 64) % 64);
        pos = o.n;
        memcpy(buf_add(&o, len), src, (size_t)len);
        wr32(o.b + 4 * i, (uint32_t)pos);
    }
    buf_add(&o, (64 - o.n % 64) % 64);
    wr32(o.b + 4 * (count + 1), (uint32_t)o.n);
    *outLen = o.n;
    return o.b;
}

int Mi_BuildEx(const char *file, const char *dir, int fitSkeleton, const MiOpts *opts, char *why, int whyLen, char *found, int foundLen) {
    MiOpts none;
    const MiOpts *o = opts;
    Fit fit;
    int autoRig = 0;
    char orient[100] = "";
    Scene s;
    Skel k;
    Rig r;
    char path[720], tmp[730];
    unsigned char *d, *out = NULL, hdr[0x60];
    long dn, outLen = 0;
    int ok, budget = MI_TRIS, how = 0, i, used = 0, tries;
    FILE *f;

    memset(&s, 0, sizeof(s));
    memset(&r, 0, sizeof(r));
    memset(&none, 0, sizeof(none));
    memset(&fit, 0, sizeof(fit));
    if (o == NULL) { o = &none; }
    if (whyLen > 0) { why[0] = '\0'; }
    if (found != NULL && foundLen > 0) { found[0] = '\0'; }
    snprintf(path, sizeof(path), "%s/m_1p.pak", dir);
    if ((d = read_all(path, &dn)) == NULL || dn < 0x400 || rd32(d) < 12 || (long)(rd32(d) + 2) * 4 > dn) {
        snprintf(why, (size_t)whyLen, "The base fighter's model is missing");
        free(d);
        return 0;
    }
    if (!skel_read(d + sec_at(d, dn, 3), sec_at(d, dn, 4) - sec_at(d, dn, 3), &k) || k.place[N_PELVIS] < 0) {
        snprintf(why, (size_t)whyLen, "The base fighter's model has no skeleton this can use");
        free(d);
        return 0;
    }
    ok = ends_with(file, ".glb") || ends_with(file, ".gltf") ? load_gltf(file, &s, why, whyLen) : ends_with(file, ".dae") ? load_dae(file, &s, why, whyLen) : load_ufbx(file, &s, why, whyLen);
    if (ok && s.nt == 0) {
        snprintf(why, (size_t)whyLen, "The file has no mesh");
        ok = 0;
    }
    if (ok) {
        scene_tint(&s);
        scene_atlas(&s);
        scene_merge(&s, key_exact, 0, 0, 32);
        scene_orient(&s, o, orient, sizeof(orient));
    }
    for (tries = 0; ok && tries < 8; tries++) { /* thinner until it fits the game's buffer */
        Buf mesh = {0}, tex = {0};
        TexRef ref[MI_MATS];
        scene_thin(&s, budget);
        how = rig_make(&s, &k, fitSkeleton, &r);
        autoRig = 0;
        if (how == 1 && !o->rig && autorig_make(&s, &k, o, &fit)) { /* no skeleton in the file: joints found from its shape */
            free(r.pos); free(r.nrm); free(r.bone); free(r.w);
            memset(&r, 0, sizeof(r));
            how = rig_make(&s, &k, 0, &r);
            autoRig = how == 2;
        }
        if (o->size > 0 && o->size != 100) { /* the owner's size: the mesh grows about the feet, the skeleton stays */
            for (i = 0; i < s.nv; i++) { r.pos[i][0] *= (float)o->size / 100; r.pos[i][1] *= (float)o->size / 100; r.pos[i][2] *= (float)o->size / 100; }
        }
        rig_fix(&s, &k, &r);
        for (i = 0; i < s.nmat; i++) { s.mat[i].used = 0; }
        for (i = 0; i < s.nt; i++) { s.mat[s.tm[i]].used = 1; }
        for (i = 0, used = 0; i < s.nmat; i++) { used += s.mat[i].used; }
        memset(ref, 0, sizeof(ref));
        memcpy(hdr, d + sec_at(d, dn, 3), sizeof(hdr));
        ok = tex_build(&s, d + sec_at(d, dn, 12), sec_at(d, dn, 13) - sec_at(d, dn, 12), d + sec_at(d, dn, 3), &tex, ref, hdr, used, o->tex) &&
             mesh_build(&s, &k, &r, ref, d + sec_at(d, dn, 3), sec_at(d, dn, 4) - sec_at(d, dn, 3), hdr, fitSkeleton && !autoRig && how == 2, &mesh);
        if (!ok) {
            snprintf(why, (size_t)whyLen, "The base fighter's model has no textures this can use");
        } else {
            free(out);
            out = pack_replace(d, dn, &mesh, &tex, &outLen);
        }
        free(mesh.b); free(tex.b);
        free(r.pos); free(r.nrm); free(r.bone); free(r.w);
        memset(&r, 0, sizeof(r));
        if (!ok || outLen <= MODEL_MAX) { break; }
        budget = budget * 3 / 4;
    }
    if (ok && outLen > MODEL_MAX) {
        snprintf(why, (size_t)whyLen, "The model is too large for the game even when thinned");
        ok = 0;
    }
    if (ok) {
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        ok = (f = fopen(tmp, "wb")) != NULL && fwrite(out, 1, (size_t)outLen, f) == (size_t)outLen;
        if (f != NULL && fclose(f) != 0) { ok = 0; }
        if (ok) {
            remove(path);
            ok = rename(tmp, path) == 0;
        }
        if (!ok) { snprintf(why, (size_t)whyLen, "Cannot write the model"); }
    }
    if (ok) {
        snprintf(why, (size_t)whyLen, "%d triangles, %d textures, %s", s.nt, used,
                 autoRig ? "joints found from the shape" : how == 2 ? "bound by the file's skeleton" : "no skeleton in the file: bound by distance");
        if (s.miss[0] != '\0') { /* (the manager's model page finds this mark and asks for the pictures) */
            size_t n = strlen(why);
            snprintf(why + n, (size_t)whyLen > n ? (size_t)whyLen - n : 0, MI_MISSING "%s", s.miss);
        }
        if (found != NULL && foundLen > 0) {
            if (autoRig) {
                snprintf(found, (size_t)foundLen, "%s  ARMS %d  SHOULDER %d  WAIST %d  NECK %d", orient, fit.arms, fit.shoulder, fit.waist, fit.neck);
            } else {
                snprintf(found, (size_t)foundLen, "%s", orient);
            }
        }
        fprintf(stderr, "bt3: model import: %s: %s, %ld bytes\n", file, why, outLen);
    }
    free(out); free(d);
    scene_free(&s);
    return ok;
}

int Mi_Build(const char *file, const char *dir, int fitSkeleton, char *why, int whyLen) {
    return Mi_BuildEx(file, dir, fitSkeleton, NULL, why, whyLen, NULL, 0);
}

#ifdef MI_TEST
/* The check: every 4-bit texture of a real model, taken apart and packed again, comes out as it was (the layout
   and the packet are the game's); then the import itself. modelimport_test <base m_1p.pak folder> <model> [fit] */
int main(int argc, char **argv) {
    char why[200], path[720], found[200] = "";
    MiOpts o;
    int a2;
    unsigned char *d, *tx;
    long dn;
    uint32_t count, i, bad = 0, done = 0;
    if (argc < 3) { return 2; }
    snprintf(path, sizeof(path), "%s/m_1p.pak", argv[1]);
    if ((d = read_all(path, &dn)) == NULL) { fprintf(stderr, "no %s\n", path); return 2; }
    tx = d + sec_at(d, dn, 12);
    count = rd32(tx);
    for (i = 0; i < count; i++) {
        const unsigned char *e = tx + rd32(tx + 4) * 4 + i * 0x40;
        int lg = (int)(rd32(e + 0x30) >> 26 & 15), lh = (int)((rd32(e + 0x30) >> 30) | (rd32(e + 0x34) & 3) << 2), side = 1 << lg;
        if ((rd32(e + 0x30) >> 20 & 63) == 0x14 && lg == lh && side >= 64) {
            unsigned char *idx = malloc((size_t)side * side), *sw = malloc((size_t)side * side / 2);
            Buf p = {0};
            tex_swizzle(idx, (unsigned char *)tx + rd32(e) * 4 + 0x60, side, 1);
            tex_swizzle(idx, sw, side, 0);
            tex_packet(&p, side / 2, side / 4, sw);
            bad += (uint32_t)p.n != rd32(e + 8) || memcmp(p.b, tx + rd32(e) * 4, (size_t)p.n) != 0;
            done++;
            free(idx); free(sw); free(p.b);
        }
    }
    printf("texture round trip: %u checked, %u differ\n", done, bad);
    free(d);
    memset(&o, 0, sizeof(o));
    for (a2 = 4; a2 < argc; a2++) { /* up=2 turn=3 mirror=1 size=90 arms=7 shoulder=80 waist=53 neck=85 tex=1 rig=1 */
        int v = atoi(strchr(argv[a2], '=') != NULL ? strchr(argv[a2], '=') + 1 : "0");
        if (strncmp(argv[a2], "up=", 3) == 0) { o.up = v; } else if (strncmp(argv[a2], "turn=", 5) == 0) { o.facing = v; } else if (strncmp(argv[a2], "mirror=", 7) == 0) { o.mirror = v; }
        else if (strncmp(argv[a2], "size=", 5) == 0) { o.size = v; } else if (strncmp(argv[a2], "arms=", 5) == 0) { o.arms = v; } else if (strncmp(argv[a2], "shoulder=", 9) == 0) { o.shoulder = v; }
        else if (strncmp(argv[a2], "waist=", 6) == 0) { o.waist = v; } else if (strncmp(argv[a2], "neck=", 5) == 0) { o.neck = v; } else if (strncmp(argv[a2], "tex=", 4) == 0) { o.tex = v; } else if (strncmp(argv[a2], "rig=", 4) == 0) { o.rig = v; }
    }
    if (!Mi_BuildEx(argv[2], argv[1], argc > 3 && atoi(argv[3]), &o, why, sizeof(why), found, sizeof(found))) { printf("FAILED: %s\n", why); return 1; }
    printf("ok: %s\n  %s\n", why, found);
    return bad != 0;
}
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include "../../third_party/ufbx/ufbx.c"
