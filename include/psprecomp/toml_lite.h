/* psprecomp — a reader for the TOML subset the MMO config files use.
 *
 * Supported: `key = value` lines, `[section]` headers (keys then read as
 * "section.key"), `#` comments (outside strings), and values that are
 * "basic strings" (escapes \" \\ \n \t), integers, floats (1.5, -2e3), or true/false. Arrays,
 * tables-in-values, dates and multi-line strings are rejected with an error
 * naming the line. */
#ifndef PSPRECOMP_TOML_LITE_H
#define PSPRECOMP_TOML_LITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TOML_STRING = 1, TOML_INT = 2, TOML_BOOL = 3, TOML_FLOAT = 4 };

typedef struct {
    char    key[96];
    int     type;
    char    str[256];
    int64_t i;          /* TOML_INT value, or TOML_BOOL 0/1 */
    double  f;          /* TOML_FLOAT value */
} toml_entry;

typedef struct {
    toml_entry *entries;
    int         count;
    char        error[160];   /* set when parsing fails */
} toml_doc;

/* Parse text. Returns 0 on success; on failure, -1 with doc->error set. */
int  toml_parse(const char *text, toml_doc *doc);
/* Parse a file. A missing file is -2 (doc->error says so). */
int  toml_parse_file(const char *path, toml_doc *doc);
void toml_free(toml_doc *doc);

/* Lookups: the value, or def when the key is absent or of another type. */
const char *toml_get_str(const toml_doc *doc, const char *key, const char *def);
int64_t     toml_get_int(const toml_doc *doc, const char *key, int64_t def);
int         toml_get_bool(const toml_doc *doc, const char *key, int def);
/* A float, or an integer as a float. */
double      toml_get_float(const toml_doc *doc, const char *key, double def);

#ifdef __cplusplus
}
#endif
#endif
