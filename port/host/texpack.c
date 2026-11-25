/* HD texture packs in PPSSPP's format (see texpack.h).
 *
 * The matching follows PPSSPP's GPU/Common/TextureReplacer.cpp and
 * TextureCacheCommon.cpp (hrydgard/ppsspp 5ce7f1c): ComputeHash,
 * TexCacheEntry::CacheKey, the CLUT hash, LookupWildcard and
 * StableQuickTexHash (its portable form). Swizzled textures are hashed as
 * stored, as PPSSPP does. */
#include "texpack.h"

#include <psprecomp/mem.h>
#include <psprecomp/render.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define XXH_INLINE_ALL
#include "xxhash.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_OVERRIDE
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#  include <direct.h>
#  define mkdir_(p) _mkdir(p)
#  define strcasecmp _stricmp
#else
#  define mkdir_(p) mkdir(p, 0755)
#endif

enum { HASH_QUICK, HASH_XXH32, HASH_XXH64 };

typedef struct {
    uint64_t ck;
    uint32_t h;
    char    *file;                 /* relative to the pack; "" = explicitly ignored */
    uint8_t *img;                  /* RGBA, loaded on first use */
    int      w, ht, state;         /* state: 0 not loaded, 1 loaded, -1 failed */
    int      tries;                /* failed loads so far */
} entry;

typedef struct { uint32_t addr, w, h, nw, nh; } hashrange;
typedef struct { uint32_t w, h; float v; } reducerange;

static struct {
    int active, dump;
    char dir[1024];
    int hash, ignore_addr, reduce;
    float reduce_global;
    entry *e; int n, cap;
    hashrange *hr; int nhr;
    reducerange *rr; int nrr;
    /* (ck, h) -> entry index, or -1 = nothing to replace; for [hashes] and lookups already made */
    uint64_t *mk_ck; uint32_t *mk_h; int *mv; int mcap, mn;
    unsigned replaced, dumped, seen, missed;   /* seen/missed: distinct textures the game drew */
    FILE *dump_ini;
} P;

/* ---- a small open-addressing map: (ck, h) -> int ---- */

static uint32_t mslot(uint64_t ck, uint32_t h, int cap) {
    uint64_t x = ck ^ ((uint64_t)h * 0x9E3779B97F4A7C15ull);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
    return (uint32_t)(x & (uint64_t)(cap - 1));
}
static int *mget(uint64_t ck, uint32_t h) {
    if (!P.mcap) return NULL;
    for (uint32_t i = mslot(ck, h, P.mcap);; i = (i + 1) & (uint32_t)(P.mcap - 1)) {
        if (P.mv[i] == -2) return NULL;
        if (P.mk_ck[i] == ck && P.mk_h[i] == h) return &P.mv[i];
    }
}
static void mput(uint64_t ck, uint32_t h, int v) {
    if ((P.mn + 1) * 2 > P.mcap) {
        const int ocap = P.mcap;
        uint64_t *ock = P.mk_ck; uint32_t *oh = P.mk_h; int *ov = P.mv;
        P.mcap = ocap ? ocap * 2 : 1024;
        P.mk_ck = calloc((size_t)P.mcap, sizeof *P.mk_ck);
        P.mk_h = calloc((size_t)P.mcap, sizeof *P.mk_h);
        P.mv = malloc((size_t)P.mcap * sizeof *P.mv);
        for (int i = 0; i < P.mcap; i++) P.mv[i] = -2;
        P.mn = 0;
        for (int i = 0; i < ocap; i++) if (ov[i] != -2) mput(ock[i], oh[i], ov[i]);
        free(ock); free(oh); free(ov);
    }
    int *slot = mget(ck, h);
    if (slot) { *slot = v; return; }
    for (uint32_t i = mslot(ck, h, P.mcap);; i = (i + 1) & (uint32_t)(P.mcap - 1)) {
        if (P.mv[i] == -2) { P.mk_ck[i] = ck; P.mk_h[i] = h; P.mv[i] = v; P.mn++; return; }
    }
}

/* ---- hashes ---- */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void s16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void s32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* PPSSPP's StableQuickTexHash (QuickTexHashNonSSE, equal to its SSE/NEON forms).
 * `aligned16` is whether the data starts on a 16-byte boundary. */
static uint32_t quick_hash(const uint8_t *p, uint32_t size, int aligned16) {
    uint32_t check = 0;
    if (aligned16 && (size & 0x3F) == 0) {
        static const uint16_t init2[8] = { 0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001 };
        uint8_t c[16] = { 0 }, c2[16];
        for (int j = 0; j < 8; j++) s16(c2 + 2 * j, init2[j]);
        for (uint32_t i = 0; i < size / 16; i += 4) {
            const uint8_t *b0 = p + 16 * i, *b1 = b0 + 16, *b2 = b0 + 32, *b3 = b0 + 48;
            for (int j = 0; j < 8; j++) s16(c + 2 * j, (uint16_t)(g16(c + 2 * j) + (uint16_t)(g16(b0 + 2 * j) * g16(c2 + 2 * j))));
            for (int j = 0; j < 4; j++) {
                uint32_t v = g32(c + 4 * j) ^ g32(b1 + 4 * j);
                s32(c + 4 * j, v + g32(b2 + 4 * j));
            }
            for (int j = 0; j < 8; j++) s16(c + 2 * j, (uint16_t)(g16(c + 2 * j) ^ (uint16_t)(g16(b3 + 2 * j) * g16(c2 + 2 * j))));
            for (int j = 0; j < 8; j++) s16(c2 + 2 * j, (uint16_t)(g16(c2 + 2 * j) + 0x2455));
        }
        for (int j = 0; j < 4; j++) s32(c + 4 * j, g32(c + 4 * j) + g32(c2 + 4 * j));
        check = g32(c) + g32(c + 4) + g32(c + 8) + g32(c + 12);
    } else {
        for (uint32_t i = 0; i + 8 <= size; i += 8) {
            check += g32(p + i);
            check ^= g32(p + i + 4);
        }
    }
    return check;
}

static uint32_t data_hash(const uint8_t *p, uint32_t n, int aligned16) {
    switch (P.hash) {
    case HASH_XXH32: return XXH32(p, n, 0xBACD7814);
    case HASH_XXH64: return (uint32_t)XXH64(p, n, 0xBACD7814);
    default:         return quick_hash(p, n, aligned16);
    }
}

static const uint8_t BPP[16] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0 };

/* TextureReplacer::ComputeHash (no maxSeenV: this renderer does not track it) */
static uint32_t compute_hash(uint32_t addr, uint32_t bufw, uint32_t w, uint32_t h, int fmt) {
    for (int i = 0; i < P.nhr; i++) {
        const hashrange *r = &P.hr[i];
        if (r->w == w && r->h == h && (r->addr == addr || (P.ignore_addr && r->addr == 0))) { w = r->nw; h = r->nh; break; }
    }
    float reduce = 1.0f;
    if (P.reduce) {
        reduce = P.reduce_global;
        for (int i = 0; i < P.nrr; i++) if (P.rr[i].w == w && P.rr[i].h == h) { reduce = P.rr[i].v; break; }
        if (reduce < 0) reduce = 0;
        if (reduce > 1) reduce = 1;
    }
    const uint32_t bpp = BPP[fmt & 15];
    if (!bpp) return 0;
    if (bufw <= w) {
        const uint32_t pixels = bufw * h + (w - bufw);
        const uint32_t size = (uint32_t)((float)((bpp * pixels) / 8) * reduce);
        const uint8_t *p = size ? (const uint8_t *)psp_mem_ptr(addr, size) : NULL;
        return p ? data_hash(p, size, (addr & 15) == 0) : 0;
    }
    const uint32_t line = (uint32_t)((float)((bpp * w) / 8) * reduce), stride = (bpp * bufw) / 8;
    const uint32_t span = h ? (h - 1) * stride + line : 0;
    const uint8_t *p = span ? (const uint8_t *)psp_mem_ptr(addr, span) : NULL;
    if (!p) return 0;
    uint32_t result = 0;
    for (uint32_t y = 0; y < h; y++, p += stride)
        result = (result * 11) ^ data_hash(p, line, ((addr + y * stride) & 15) == 0);
    return result;
}

/* ---- ini ---- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int add_entry(uint64_t ck, uint32_t h, const char *file) {
    if (P.n == P.cap) {
        P.cap = P.cap ? P.cap * 2 : 256;
        entry *ne = realloc(P.e, (size_t)P.cap * sizeof *P.e);
        if (!ne) return -1;
        P.e = ne;
    }
    entry *e = &P.e[P.n];
    memset(e, 0, sizeof *e);
    e->ck = ck; e->h = h;
    e->file = strdup(file);
    return P.n++;
}

static int load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024], sec[64] = "";
    int have_hash = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", s + 1); for (char *c = sec; *c; c++) *c = (char)tolower((unsigned char)*c); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(sec, "options")) {
            if (!strcasecmp(k, "hash")) {
                have_hash = 1;
                P.hash = !strcasecmp(v, "xxh32") ? HASH_XXH32 : !strcasecmp(v, "xxh64") ? HASH_XXH64 : HASH_QUICK;
            } else if (!strcasecmp(k, "ignoreAddress")) P.ignore_addr = !strcasecmp(v, "true") || !strcmp(v, "1");
            else if (!strcasecmp(k, "reduceHash")) P.reduce = !strcasecmp(v, "true") || !strcmp(v, "1");
            else if (!strcasecmp(k, "reduceHashGlobalValue")) P.reduce_global = (float)atof(v);
        } else if (!strcmp(sec, "hashes")) {
            unsigned long long ck = 0;
            unsigned h = 0;
            int level = 0;
            if (sscanf(k, "%16llx%8x_%d", &ck, &h, &level) >= 1 && level == 0 && !strstr(v, "..") && !strchr(v, '|')) {
                const int i = add_entry(ck, h, v);
                if (i >= 0) mput(ck, h, i);
            }
        } else if (!strcmp(sec, "hashranges")) {
            unsigned a, w, h, nw, nh;
            if (sscanf(k, "%x , %u , %u", &a, &w, &h) == 3 && sscanf(v, "%u , %u", &nw, &nh) == 2 && w <= 1024 && h <= 1024) {
                hashrange *nr = realloc(P.hr, (size_t)(P.nhr + 1) * sizeof *P.hr);
                if (nr) { P.hr = nr; P.hr[P.nhr++] = (hashrange){ a, w, h, nw, nh }; }
            }
        } else if (!strcmp(sec, "reducehashranges")) {
            unsigned w, h;
            if (sscanf(k, "%u , %u", &w, &h) == 2) {
                reducerange *nr = realloc(P.rr, (size_t)(P.nrr + 1) * sizeof *P.rr);
                if (nr) { P.rr = nr; P.rr[P.nrr++] = (reducerange){ w, h, (float)atof(v) }; }
            }
        }
    }
    fclose(f);
    if (!have_hash) fprintf(stderr, "texpack: %s has no [options] hash: using quick, PPSSPP's default for old packs\n", path);
    if (P.ignore_addr && P.hash == HASH_QUICK) { P.ignore_addr = 0; fprintf(stderr, "texpack: ignoreAddress needs xxh32/xxh64: off\n"); }
    if (P.reduce && P.hash == HASH_QUICK) { P.reduce = 0; fprintf(stderr, "texpack: reduceHash needs xxh32/xxh64: off\n"); }
    return 0;
}

int texpack_init(const char *dir) {
    memset(&P, 0, sizeof P);
    P.reduce_global = 0.5f;
    snprintf(P.dir, sizeof P.dir, "%s", dir);
    P.dump = getenv("PSP2I_TEXDUMP") != NULL;
    char ini[1100];
    snprintf(ini, sizeof ini, "%s/textures.ini", dir);
    const int have = load_ini(ini) == 0;
    if (!have && !P.dump) return -1;
    P.active = 1;
    fprintf(stderr, "texpack: %s: %d textures (%s hash%s%s)%s\n", dir, P.n,
            P.hash == HASH_XXH64 ? "xxh64" : P.hash == HASH_XXH32 ? "xxh32" : "quick",
            P.ignore_addr ? ", ignoreAddress" : "", P.reduce ? ", reduceHash" : "",
            P.dump ? "; dumping new textures to new/" : "");
    if (!have) fprintf(stderr, "texpack: no %s yet: dumping only\n", ini);
    return 0;
}

int texpack_active(void) { return P.active; }

/* LookupWildcard, in PPSSPP's order */
static int find(uint64_t ck, uint32_t h) {
    int *v;
    if (P.ignore_addr) ck &= 0xFFFFFFFFull;
    if ((v = mget(ck, h)) && *v >= 0) return *v;
    if ((v = mget(ck & 0xFFFFFFFFull, 0)) && *v >= 0) return *v;
    if (!P.ignore_addr && (v = mget(ck, 0)) && *v >= 0) return *v;
    if ((v = mget(ck & 0xFFFFFFFFull, h)) && *v >= 0) return *v;
    if (!P.ignore_addr && (v = mget(ck & ~0xFFFFFFFFull, h)) && *v >= 0) return *v;
    if (!P.ignore_addr && (v = mget(ck & ~0xFFFFFFFFull, 0)) && *v >= 0) return *v;
    if ((v = mget(0, h)) && *v >= 0) return *v;
    return -1;
}

static void dump(const char *name, const uint32_t *px, int w, int h) {
    char path[1200];
    snprintf(path, sizeof path, "%s/new", P.dir);
    mkdir_(P.dir);
    mkdir_(path);
    snprintf(path, sizeof path, "%s/new/%s.png", P.dir, name);
    if (!stbi_write_png(path, w, h, 4, px, w * 4)) return;
    if (!P.dump_ini) {
        snprintf(path, sizeof path, "%s/new/textures.ini", P.dir);
        struct stat sb;
        const int fresh = stat(path, &sb) != 0;
        P.dump_ini = fopen(path, "a");
        if (P.dump_ini && fresh)
            fprintf(P.dump_ini, "[options]\nversion = 1\nhash = %s\n\n[hashes]\n",
                    P.hash == HASH_XXH64 ? "xxh64" : P.hash == HASH_XXH32 ? "xxh32" : "quick");
    }
    if (P.dump_ini) { fprintf(P.dump_ini, "%s = %s.png\n", name, name); fflush(P.dump_ini); }
    P.dumped++;
}

const uint8_t *texpack_lookup(const uint32_t *orig, int *w, int *h) {
    if (!P.active) return NULL;
    psp_gpu_tex_info t;
    psp_gpu_texture_info(&t);
    if (t.psm > 10) return NULL;
    /* TexCacheEntry::CacheKey: the GE registers carry their command byte, as in PPSSPP's gstate */
    uint64_t ck = ((uint64_t)(t.addr & 0x3FFFFFFFu) << 32) | t.dim;
    if (t.psm >= 4 && t.psm <= 7) {
        const uint32_t base = ((t.clutfmt >> 16) & 0x1F) << 4;
        const uint32_t base_bytes = base * ((t.clutfmt & 3) == 3 ? 4u : 2u);
        uint32_t ext = t.clut_total + base_bytes;
        if (ext > t.clut_max) ext = t.clut_max;
        const uint32_t clut_hash = XXH32(t.clut, ext, 0xC0108888);
        ck ^= clut_hash ^ (0xC5000000u | t.clutfmt);
    }
    const uint32_t bpp = BPP[t.psm];
    const uint32_t align = 0x7FFu & ~((128u / bpp) - 1u);
    uint32_t bufw = t.bufw & align;
    if (!bufw) bufw = 128u / bpp;
    const uint32_t hash = compute_hash(t.addr, bufw, t.w, t.h, t.psm);

    int *known = mget(ck, hash);
    int idx = known ? *known : -3;
    if (idx == -3) {                                      /* first time we see this key */
        idx = find(ck, hash);
        if (idx < 0) {                                    /* or a file named by its hash */
            char name[40], path[1200];
            snprintf(name, sizeof name, "%016llx%08x", (unsigned long long)ck, hash);
            snprintf(path, sizeof path, "%s/%s.png", P.dir, name);
            struct stat sb;
            if (stat(path, &sb) == 0) {
                char rel[48];
                snprintf(rel, sizeof rel, "%s.png", name);
                idx = add_entry(ck, hash, rel);
            } else if (P.dump && orig) {
                dump(name, orig, (int)t.w, (int)t.h);
            }
        }
        mput(ck, hash, idx < 0 ? -1 : idx);
        P.seen++;
        if (idx < 0) P.missed++;
        if (P.seen % 256 == 0)                            /* is the pack matching? */
            fprintf(stderr, "texpack: %u textures seen, %u in the pack, %u not\n", P.seen, P.seen - P.missed, P.missed);
    }
    if (idx < 0) return NULL;
    entry *e = &P.e[idx];
    if (!e->file[0]) return NULL;                         /* listed with no file: keep the original */
    if (e->state == 0) {
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", P.dir, e->file);
        int n;
        e->img = stbi_load(path, &e->w, &e->ht, &n, 4);
        if (e->img) {
            e->state = 1;
            if (P.replaced++ < 8) fprintf(stderr, "texpack: replaced %016llx%08x with %s (%dx%d)\n",
                                          (unsigned long long)ck, hash, e->file, e->w, e->ht);
        } else if (++e->tries < 3) {
            /* not yet: a pack on a network share can fail to open now and then; tried again next time */
        } else {
            e->state = -1;
            fprintf(stderr, "texpack: cannot load %s (%s)\n", path, stbi_failure_reason() ? stbi_failure_reason() : "cannot open");
        }
    }
    if (e->state != 1) return NULL;
    *w = e->w;
    *h = e->ht;
    return e->img;
}

void texpack_stats(unsigned *keys, unsigned *replaced, unsigned *dumped) {
    if (keys) *keys = (unsigned)P.n;
    if (replaced) *replaced = P.replaced;
    if (dumped) *dumped = P.dumped;
}

void texpack_shutdown(void) {
    for (int i = 0; i < P.n; i++) { free(P.e[i].file); stbi_image_free(P.e[i].img); }
    free(P.e); free(P.hr); free(P.rr); free(P.mk_ck); free(P.mk_h); free(P.mv);
    if (P.dump_ini) fclose(P.dump_ini);
    memset(&P, 0, sizeof P);
}
