/* Server-side loot (see loot.h). */
#include "loot.h"

#include "sqlite3.h"

#include <psprecomp/toml_lite.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- RNG: xoshiro256** (Blackman & Vigna), splitmix64 seeding ---- */

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
void loot_rng_seed(loot_rng *r, uint64_t seed) { for (int i = 0; i < 4; i++) r->s[i] = splitmix(&seed); }
static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
uint64_t loot_rng_next(loot_rng *r) {
    uint64_t *s = r->s;
    const uint64_t out = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
    return out;
}
double loot_rng_unit(loot_rng *r) { return (double)(loot_rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }
uint32_t loot_rng_range(loot_rng *r, uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    const uint64_t span = (uint64_t)hi - lo + 1, limit = UINT64_MAX - UINT64_MAX % span;   /* no modulo bias */
    uint64_t v;
    do v = loot_rng_next(r); while (v >= limit);
    return lo + (uint32_t)(v % span);
}

/* ---- parsing (SQLite JSON functions) ---- */

typedef struct { sqlite3 *h; const char *json; char *err; size_t cap; } P;

static int perr(P *p, const char *where, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(p->err, p->cap, "%s%s%s", where ? where : "", where && *where ? ": " : "", msg);
    return -1;
}

/* json_type of `path` in `obj` ("" when absent), and its value. */
typedef struct { char type[12]; double num; char str[256]; char *text; } jval;
static void jv_free(jval *v) { sqlite3_free(v->text); v->text = NULL; }
static int jget(P *p, const char *obj, const char *path, jval *v) {
    memset(v, 0, sizeof *v);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(p->h, "SELECT json_type(?1, ?2), json_extract(?1, ?2), json_extract(?1, ?2)", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, obj, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        snprintf(v->type, sizeof v->type, "%s", (const char *)sqlite3_column_text(st, 0));
        v->num = sqlite3_column_double(st, 1);
        const char *s = (const char *)sqlite3_column_text(st, 2);
        if (s) { snprintf(v->str, sizeof v->str, "%s", s); v->text = sqlite3_mprintf("%s", s); }
    }
    sqlite3_finalize(st);
    return 0;
}

/* Every key of obj must be in `allowed` (NULL-terminated): typos are errors. */
static int keys_ok(P *p, const char *obj, const char *where, const char *const *allowed) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(p->h, "SELECT key FROM json_each(?1)", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, obj, -1, SQLITE_STATIC);
    int rc = 0;
    while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        int ok = 0;
        for (int i = 0; allowed[i] && !ok; i++) ok = !strcmp(k, allowed[i]);
        if (!ok) rc = perr(p, where, "unknown key \"%s\"", k);
    }
    sqlite3_finalize(st);
    return rc;
}

/* A number at path: required or default; integer when asked; within [lo, hi]. */
static int num(P *p, const char *obj, const char *path, const char *where, int required, int integer,
               double lo, double hi, double def, double *out) {
    jval v;
    jget(p, obj, path, &v);
    jv_free(&v);
    if (!v.type[0]) {
        if (required) return perr(p, where, "\"%s\" is missing", path + 2);
        *out = def;
        return 0;
    }
    if (strcmp(v.type, "integer") && (integer || strcmp(v.type, "real")))
        return perr(p, where, "\"%s\" must be %s", path + 2, integer ? "an integer" : "a number");
    if (!(v.num >= lo && v.num <= hi)) return perr(p, where, "\"%s\" = %g is out of range [%g, %g]", path + 2, v.num, lo, hi);
    *out = v.num;
    return 0;
}

static int name(P *p, const char *obj, const char *where, char out[LOOT_NAME_LEN]) {
    jval v;
    jget(p, obj, "$.name", &v);
    jv_free(&v);
    out[0] = 0;
    if (!v.type[0]) return 0;
    if (strcmp(v.type, "text")) return perr(p, where, "\"name\" must be a string");
    if (strlen(v.str) >= LOOT_NAME_LEN) return perr(p, where, "\"name\" is longer than %d bytes", LOOT_NAME_LEN - 1);
    snprintf(out, LOOT_NAME_LEN, "%s", v.str);
    return 0;
}

/* [min, max] Meseta */
static int meseta_range(P *p, const char *obj, const char *where, uint32_t *mn, uint32_t *mx) {
    jval v;
    jget(p, obj, "$.meseta", &v);
    jv_free(&v);
    *mn = *mx = 0;
    if (!v.type[0]) return 0;
    double a, b;
    jval len;
    jget(p, obj, "$.meseta[2]", &len);
    jv_free(&len);
    if (strcmp(v.type, "array") || len.type[0] ||
        num(p, obj, "$.meseta[0]", where, 1, 1, 0, 1e9, 0, &a) || num(p, obj, "$.meseta[1]", where, 1, 1, 0, 1e9, 0, &b))
        return perr(p, where, "\"meseta\" must be [min, max] (whole numbers, 0..1000000000)");
    if (a > b) return perr(p, where, "\"meseta\" min %g is above max %g", a, b);
    *mn = (uint32_t)a; *mx = (uint32_t)b;
    return 0;
}

/* Count an array's elements (-1: present but not an array; 0 when absent). */
static int array_len(P *p, const char *obj, const char *path) {
    jval v;
    jget(p, obj, path, &v);
    jv_free(&v);
    if (!v.type[0]) return 0;
    if (strcmp(v.type, "array")) return -1;
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(p->h, "SELECT json_array_length(?1, ?2)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, obj, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* The i-th element of the array at path, as JSON text (sqlite3_free). */
static char *element(P *p, const char *obj, const char *path, int i) {
    char full[64];
    snprintf(full, sizeof full, "%s[%d]", path, i);
    jval v;
    jget(p, obj, full, &v);
    if (strcmp(v.type, "object")) { jv_free(&v); return NULL; }
    return v.text;
}

static int parse_entries(P *p, const char *obj, const char *path, const char *where, int allow_meseta,
                         loot_entry **out, int *n_out, double *total) {
    const int n = array_len(p, obj, path);
    if (n < 0) return perr(p, where, "\"%s\" must be an array", path + 2);
    *out = n ? calloc((size_t)n, sizeof **out) : NULL;
    *n_out = n;
    *total = 0;
    static const char *const KEYS[] = { "id", "name", "weight", "meseta", NULL };
    for (int i = 0; i < n; i++) {
        char w[96];
        snprintf(w, sizeof w, "%s.%s[%d]", where, path + 2, i);
        char *e = element(p, obj, path, i);
        if (!e) return perr(p, w, "must be an object");
        loot_entry *x = &(*out)[i];
        double id = 0, weight;
        jval m;
        jget(p, e, "$.meseta", &m);
        jv_free(&m);
        int rc = keys_ok(p, e, w, KEYS) || name(p, e, w, x->name) || num(p, e, "$.weight", w, 1, 0, 1e-9, 1e9, 0, &weight);
        if (!rc && m.type[0]) {
            if (!allow_meseta || strcmp(m.type, "true")) rc = perr(p, w, allow_meseta ? "\"meseta\" must be true" : "Meseta entries are not allowed here");
            else x->is_meseta = 1;
        }
        if (!rc && !x->is_meseta) rc = num(p, e, "$.id", w, 1, 1, 1, 4294967295.0, 0, &id);
        sqlite3_free(e);
        if (rc) return -1;
        x->item_id = (uint32_t)id;
        x->weight = weight;
        *total += weight;
    }
    return 0;
}

static int parse_enemy(P *p, const char *e, const char *w, loot_enemy *x) {
    static const char *const KEYS[] = { "id", "name", "exp", "type_points", "drop_chance", "meseta", "items", "rares", NULL };
    double id, exp, tp, dc;
    if (keys_ok(p, e, w, KEYS) || num(p, e, "$.id", w, 1, 1, 1, 4294967295.0, 0, &id) || name(p, e, w, x->name) ||
        num(p, e, "$.exp", w, 0, 1, 0, 1e9, 0, &exp) || num(p, e, "$.type_points", w, 0, 1, 0, 1e9, 0, &tp) ||
        num(p, e, "$.drop_chance", w, 0, 0, 0, 1, 0, &dc) || meseta_range(p, e, w, &x->meseta_min, &x->meseta_max) ||
        parse_entries(p, e, "$.items", w, 1, &x->items, &x->n_items, &x->total_weight))
        return -1;
    x->id = (uint32_t)id; x->exp = (uint32_t)exp; x->type_points = (uint32_t)tp; x->drop_chance = dc;
    if (dc > 0 && !x->n_items) return perr(p, w, "\"drop_chance\" is above 0 but there are no \"items\"");
    for (int i = 0; i < x->n_items; i++)
        if (x->items[i].is_meseta && !x->meseta_max) return perr(p, w, "a Meseta entry needs \"meseta\": [min, max] with max > 0");
    const int nr = array_len(p, e, "$.rares");
    if (nr < 0) return perr(p, w, "\"rares\" must be an array");
    x->rares = nr ? calloc((size_t)nr, sizeof *x->rares) : NULL;
    x->n_rares = nr;
    static const char *const RKEYS[] = { "id", "name", "chance", NULL };
    for (int i = 0; i < nr; i++) {
        char rw[96];
        snprintf(rw, sizeof rw, "%s.rares[%d]", w, i);
        char *r = element(p, e, "$.rares", i);
        if (!r) return perr(p, rw, "must be an object");
        double rid, ch;
        const int rc = keys_ok(p, r, rw, RKEYS) || num(p, r, "$.id", rw, 1, 1, 1, 4294967295.0, 0, &rid) ||
                       name(p, r, rw, x->rares[i].name) || num(p, r, "$.chance", rw, 1, 0, 1e-12, 1, 0, &ch);
        sqlite3_free(r);
        if (rc) return -1;
        x->rares[i].item_id = (uint32_t)rid;
        x->rares[i].chance = ch;
        x->total_rare += ch;
    }
    if (x->total_rare > 1.0 + 1e-9) return perr(p, w, "the rare chances add up to %g (above 1)", x->total_rare);
    return 0;
}

static int parse_mission(P *p, const char *e, const char *w, loot_mission *x) {
    static const char *const KEYS[] = { "id", "name", "meseta", "rewards", NULL };
    double id;
    if (keys_ok(p, e, w, KEYS) || num(p, e, "$.id", w, 1, 1, 1, 4294967295.0, 0, &id) || name(p, e, w, x->name) ||
        meseta_range(p, e, w, &x->meseta_min, &x->meseta_max) ||
        parse_entries(p, e, "$.rewards", w, 0, &x->rewards, &x->n_rewards, &x->total_weight))
        return -1;
    x->id = (uint32_t)id;
    return 0;
}

void loot_free(loot_table *t) {
    if (!t) return;
    for (int i = 0; i < t->n_enemies; i++) { free(t->enemies[i].items); free(t->enemies[i].rares); }
    for (int i = 0; i < t->n_missions; i++) free(t->missions[i].rewards);
    free(t->enemies);
    free(t->missions);
    free(t);
}

loot_table *loot_parse(const char *json, char *err, size_t cap) {
    P p = { NULL, json, err, cap };
    err[0] = 0;
    if (sqlite3_open_v2(":memory:", &p.h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_MEMORY, NULL) != SQLITE_OK) {
        snprintf(err, cap, "cannot open SQLite");
        sqlite3_close(p.h);
        return NULL;
    }
    loot_table *t = calloc(1, sizeof *t);
    int rc = t ? 0 : perr(&p, NULL, "out of memory");

    sqlite3_stmt *st;
    /* valid JSON? if not, where. Strict RFC 8259: SQLite also takes JSON5
     * (comments, trailing commas), which other tools would not. */
    if (!rc && sqlite3_prepare_v2(p.h, "SELECT json_error_position(?1), json_valid(?1, 1)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_ROW) rc = perr(&p, NULL, "cannot check the JSON");
        else {
            const int pos = sqlite3_column_int(st, 0), strict = sqlite3_column_int(st, 1);
            if (pos > 0) {
                int line = 1, col = 1;
                for (int i = 0; i < pos - 1 && json[i]; i++) { if (json[i] == '\n') { line++; col = 1; } else col++; }
                rc = perr(&p, NULL, "not valid JSON: line %d, column %d", line, col);
            } else if (!strict) {
                rc = perr(&p, NULL, "not strict JSON (no comments, trailing commas or unquoted keys)");
            }
        }
        sqlite3_finalize(st);
    }
    if (!rc && sqlite3_prepare_v2(p.h, "SELECT json_type(?1)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_ROW || !sqlite3_column_text(st, 0) || strcmp((const char *)sqlite3_column_text(st, 0), "object"))
            rc = perr(&p, NULL, "the top level must be an object");
        sqlite3_finalize(st);
    }
    static const char *const TOP[] = { "version", "enemies", "missions", "comment", NULL };
    double ver = 1;
    if (!rc) rc = keys_ok(&p, json, "", TOP) || num(&p, json, "$.version", "", 0, 1, 1, 1000, 1, &ver);
    if (!rc) t->version = (int)ver;

    const int ne = rc ? 0 : array_len(&p, json, "$.enemies");
    if (ne < 0) rc = perr(&p, NULL, "\"enemies\" must be an array");
    if (!rc && ne) { t->enemies = calloc((size_t)ne, sizeof *t->enemies); t->n_enemies = ne; }
    for (int i = 0; !rc && i < ne; i++) {
        char w[48];
        snprintf(w, sizeof w, "enemies[%d]", i);
        char *e = element(&p, json, "$.enemies", i);
        rc = e ? parse_enemy(&p, e, w, &t->enemies[i]) : perr(&p, w, "must be an object");
        sqlite3_free(e);
        for (int k = 0; !rc && k < i; k++)
            if (t->enemies[k].id == t->enemies[i].id) rc = perr(&p, w, "id %u is also enemies[%d]", t->enemies[i].id, k);
    }
    const int nm = rc ? 0 : array_len(&p, json, "$.missions");
    if (nm < 0) rc = perr(&p, NULL, "\"missions\" must be an array");
    if (!rc && nm) { t->missions = calloc((size_t)nm, sizeof *t->missions); t->n_missions = nm; }
    for (int i = 0; !rc && i < nm; i++) {
        char w[48];
        snprintf(w, sizeof w, "missions[%d]", i);
        char *e = element(&p, json, "$.missions", i);
        rc = e ? parse_mission(&p, e, w, &t->missions[i]) : perr(&p, w, "must be an object");
        sqlite3_free(e);
        for (int k = 0; !rc && k < i; k++)
            if (t->missions[k].id == t->missions[i].id) rc = perr(&p, w, "id %u is also missions[%d]", t->missions[i].id, k);
    }
    sqlite3_close(p.h);
    if (rc) { loot_free(t); return NULL; }
    return t;
}

loot_table *loot_load(const char *path, char *err, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, cap, "%s: cannot open", path); return NULL; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64L * 1024 * 1024) { fclose(f); snprintf(err, cap, "%s: too large", path); return NULL; }
    char *buf = malloc((size_t)n + 1);
    const size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)n) { free(buf); snprintf(err, cap, "%s: cannot read", path); return NULL; }
    buf[n] = 0;
    char e2[256];
    loot_table *t = loot_parse(buf, e2, sizeof e2);
    free(buf);
    if (!t) snprintf(err, cap, "%s: %s", path, e2);
    return t;
}

const loot_enemy *loot_enemy_find(const loot_table *t, uint32_t id) {
    for (int i = 0; t && i < t->n_enemies; i++) if (t->enemies[i].id == id) return &t->enemies[i];
    return NULL;
}
const loot_mission *loot_mission_find(const loot_table *t, uint32_t id) {
    for (int i = 0; t && i < t->n_missions; i++) if (t->missions[i].id == id) return &t->missions[i];
    return NULL;
}

/* ---- rolls ---- */

static uint32_t scaled(uint32_t base, double rate) {
    const double v = floor((double)base * rate + 0.5);
    return v <= 0 ? 0 : v >= 4294967295.0 ? 4294967295u : (uint32_t)v;
}

static const loot_entry *pick(const loot_entry *e, int n, double total, loot_rng *rng) {
    double u = loot_rng_unit(rng) * total;
    for (int i = 0; i < n; i++) { if (u < e[i].weight) return &e[i]; u -= e[i].weight; }
    return n ? &e[n - 1] : NULL;                     /* rounding at the top end */
}

int loot_roll_enemy(const loot_table *t, uint32_t id, const loot_rates *r, loot_rng *rng, loot_result *out) {
    memset(out, 0, sizeof *out);
    const loot_enemy *x = loot_enemy_find(t, id);
    if (!x) return -1;
    out->exp = scaled(x->exp, r->exp);
    out->type_points = scaled(x->type_points, r->type_points);
    double p = x->total_rare * r->rare;
    if (x->n_rares && loot_rng_unit(rng) < (p > 1 ? 1 : p)) {
        double u = loot_rng_unit(rng) * x->total_rare;
        const loot_rare *pickr = &x->rares[x->n_rares - 1];
        for (int i = 0; i < x->n_rares; i++) { if (u < x->rares[i].chance) { pickr = &x->rares[i]; break; } u -= x->rares[i].chance; }
        out->has_drop = out->rare = 1;
        out->item_id = pickr->item_id;
        memcpy(out->name, pickr->name, sizeof out->name);
        return 0;
    }
    p = x->drop_chance * r->drop;
    if (x->n_items && loot_rng_unit(rng) < (p > 1 ? 1 : p)) {
        const loot_entry *e = pick(x->items, x->n_items, x->total_weight, rng);
        out->has_drop = 1;
        if (e->is_meseta) {
            out->meseta = scaled(loot_rng_range(rng, x->meseta_min, x->meseta_max), r->meseta);
            snprintf(out->name, sizeof out->name, "Meseta");
            if (!out->meseta) out->has_drop = 0;
        } else {
            out->item_id = e->item_id;
            memcpy(out->name, e->name, sizeof out->name);
        }
    }
    return 0;
}

int loot_roll_mission(const loot_table *t, uint32_t id, const loot_rates *r, loot_rng *rng, loot_result *out) {
    memset(out, 0, sizeof *out);
    const loot_mission *m = loot_mission_find(t, id);
    if (!m) return -1;
    out->meseta = scaled(loot_rng_range(rng, m->meseta_min, m->meseta_max), r->meseta);
    if (m->n_rewards) {
        const loot_entry *e = pick(m->rewards, m->n_rewards, m->total_weight, rng);
        out->has_drop = 1;
        out->item_id = e->item_id;
        memcpy(out->name, e->name, sizeof out->name);
    }
    return 0;
}

/* ---- rates ---- */

static double *rate_field(loot_rates *r, const char *name) {
    if (!strcmp(name, "exp_rate")) return &r->exp;
    if (!strcmp(name, "drop_rate")) return &r->drop;
    if (!strcmp(name, "rare_drop_rate")) return &r->rare;
    if (!strcmp(name, "meseta_rate")) return &r->meseta;
    if (!strcmp(name, "type_points_rate")) return &r->type_points;
    return NULL;
}

int loot_rates_set(loot_rates *r, const char *name, double v) {
    double *f = rate_field(r, name);
    if (!f || !(v >= 0 && v <= 100)) return -1;
    *f = v;
    return 0;
}

int loot_rates_load(const char *path, loot_rates *out, char *err, size_t cap) {
    toml_doc d;
    const int rc = toml_parse_file(path, &d);
    if (rc == -2) { toml_free(&d); snprintf(err, cap, "%s: not found", path); return -2; }
    if (rc != 0) { snprintf(err, cap, "%s: %s", path, d.error); toml_free(&d); return -1; }
    loot_rates r = LOOT_RATES_DEFAULT;
    for (int i = 0; i < d.count; i++) {
        const toml_entry *e = &d.entries[i];
        const double v = e->type == TOML_FLOAT ? e->f : e->type == TOML_INT ? (double)e->i : -1;
        if (!rate_field(&r, e->key)) { snprintf(err, cap, "%s: unknown setting \"%s\"", path, e->key); toml_free(&d); return -1; }
        if (loot_rates_set(&r, e->key, v) != 0) { snprintf(err, cap, "%s: %s must be a number from 0 to 100", path, e->key); toml_free(&d); return -1; }
    }
    toml_free(&d);
    *out = r;
    return 0;
}
