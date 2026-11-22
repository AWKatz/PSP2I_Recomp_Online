/* psprecomp — TOML subset reader. See include/psprecomp/toml_lite.h. */

#include "psprecomp/toml_lite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static int is_key_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }

static int fail(toml_doc *d, int line, const char *what) {
    snprintf(d->error, sizeof d->error, "line %d: %s", line, what);
    return -1;
}

/* After a value: only whitespace or a comment may follow. */
static int rest_ok(const char *p) { p = skip_ws(p); return *p == 0 || *p == '#'; }

static int parse_line(toml_doc *d, const char *p, int line, char *section, int *cap) {
    p = skip_ws(p);
    if (*p == 0 || *p == '#') return 0;
    if (*p == '[') {
        const char *q = ++p;
        while (is_key_char(*q) || *q == '.') q++;
        if (*q != ']' || q == p || (size_t)(q - p) >= 64) return fail(d, line, "bad [section] header");
        if (!rest_ok(q + 1)) return fail(d, line, "text after [section]");
        memcpy(section, p, (size_t)(q - p)); section[q - p] = 0;
        return 0;
    }
    const char *k = p;
    while (is_key_char(*p)) p++;
    if (p == k) return fail(d, line, "expected a key");
    const size_t klen = (size_t)(p - k);
    p = skip_ws(p);
    if (*p != '=') return fail(d, line, "expected '=' after the key");
    p = skip_ws(p + 1);

    if (d->count == *cap) {
        const int ncap = *cap ? *cap * 2 : 16;
        toml_entry *n = realloc(d->entries, (size_t)ncap * sizeof *n);
        if (!n) return fail(d, line, "out of memory");
        d->entries = n; *cap = ncap;
    }
    toml_entry *e = &d->entries[d->count];
    memset(e, 0, sizeof *e);
    const size_t slen = strlen(section);
    if (slen + 1 + klen >= sizeof e->key) return fail(d, line, "key too long");
    if (slen) { memcpy(e->key, section, slen); e->key[slen] = '.'; memcpy(e->key + slen + 1, k, klen); }
    else memcpy(e->key, k, klen);

    if (*p == '"') {
        if (p[1] == '"' && p[2] == '"') return fail(d, line, "multi-line strings are not supported");
        size_t n = 0;
        for (p++; *p && *p != '"'; p++) {
            char c = *p;
            if (c == '\\') {
                p++;
                switch (*p) {
                case '"': c = '"'; break;  case '\\': c = '\\'; break;
                case 'n': c = '\n'; break; case 't': c = '\t'; break;
                default: return fail(d, line, "unsupported escape in string");
                }
            }
            if (n + 1 >= sizeof e->str) return fail(d, line, "string too long");
            e->str[n++] = c;
        }
        if (*p != '"') return fail(d, line, "unterminated string");
        if (!rest_ok(p + 1)) return fail(d, line, "text after the value");
        e->type = TOML_STRING;
    } else if (!strncmp(p, "true", 4) && rest_ok(p + 4)) {
        e->type = TOML_BOOL; e->i = 1;
    } else if (!strncmp(p, "false", 5) && rest_ok(p + 5)) {
        e->type = TOML_BOOL; e->i = 0;
    } else if (*p == '-' || *p == '+' || isdigit((unsigned char)*p)) {
        char digits[32]; size_t n = 0;
        if (*p == '-' || *p == '+') digits[n++] = *p++;
        if (!isdigit((unsigned char)*p)) return fail(d, line, "bad number");
        while (isdigit((unsigned char)*p) || *p == '_') {
            if (*p != '_') { if (n + 1 >= sizeof digits) return fail(d, line, "number too long"); digits[n++] = *p; }
            p++;
        }
        digits[n] = 0;
        if (!rest_ok(p)) return fail(d, line, "unsupported value (only strings, integers, booleans)");
        e->type = TOML_INT; e->i = strtoll(digits, NULL, 10);
    } else {
        return fail(d, line, "unsupported value (only strings, integers, booleans)");
    }
    d->count++;
    return 0;
}

int toml_parse(const char *text, toml_doc *doc) {
    memset(doc, 0, sizeof *doc);
    char section[64] = "", buf[1024];
    int cap = 0, line = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        line++;
        if (n >= sizeof buf) { fail(doc, line, "line too long"); toml_free(doc); doc->count = 0; return -1; }
        memcpy(buf, p, n); buf[n] = 0;
        if (n && buf[n - 1] == '\r') buf[n - 1] = 0;
        if (parse_line(doc, buf, line, section, &cap) != 0) {
            char err[sizeof doc->error]; memcpy(err, doc->error, sizeof err);
            toml_free(doc); memcpy(doc->error, err, sizeof err);
            return -1;
        }
        p += n + (eol ? 1 : 0);
    }
    return 0;
}

int toml_parse_file(const char *path, toml_doc *doc) {
    memset(doc, 0, sizeof *doc);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(doc->error, sizeof doc->error, "%s: not found", path); return -2; }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (1 << 20)) { fclose(f); snprintf(doc->error, sizeof doc->error, "%s: too large", path); return -1; }
    char *text = malloc((size_t)len + 1);
    if (!text) { fclose(f); return -1; }
    const size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;
    const int rc = toml_parse(text, doc);
    free(text);
    return rc;
}

void toml_free(toml_doc *doc) {
    free(doc->entries);
    doc->entries = NULL;
    doc->count = 0;
}

static const toml_entry *find(const toml_doc *d, const char *key, int type) {
    for (int i = d->count - 1; i >= 0; i--)      /* the last definition wins */
        if (!strcmp(d->entries[i].key, key)) return d->entries[i].type == type ? &d->entries[i] : NULL;
    return NULL;
}
const char *toml_get_str(const toml_doc *d, const char *key, const char *def) { const toml_entry *e = find(d, key, TOML_STRING); return e ? e->str : def; }
int64_t     toml_get_int(const toml_doc *d, const char *key, int64_t def)     { const toml_entry *e = find(d, key, TOML_INT);    return e ? e->i : def; }
int         toml_get_bool(const toml_doc *d, const char *key, int def)        { const toml_entry *e = find(d, key, TOML_BOOL);   return e ? (int)e->i : def; }
