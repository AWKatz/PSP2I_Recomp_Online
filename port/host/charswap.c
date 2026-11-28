/* Character swap: see charswap.h. */
#include "charswap.h"

#include <psprecomp/mem.h>

#include <stdio.h>
#include <string.h>

#define REC_MAGIC   0x0052554Eu      /* "NUR\0" */
#define REC_SIZE    0xB0u            /* the block's size field */
#define REC_STRIDE  0xC0u
#define REC_BODY    0x10u            /* the character data starts here */
#define APP_BYTES   0x20u            /* header, word, six part slots */
#define SCAN_LO     0x08800000u
#define SCAN_HI     PSP_HOST_RAM_BASE

static char g_backup_path[1100];
static uint32_t g_table;             /* the first record, 0 = not found */
static int g_n;
static char g_name[CHARSWAP_MAX][40], g_kind[CHARSWAP_MAX][16];

void charswap_init(const char *base_dir) {
    snprintf(g_backup_path, sizeof g_backup_path, "%s/character_backup.txt", base_dir && *base_dir ? base_dir : ".");
    g_table = 0;
    g_n = 0;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* A UTF-16 name (up to n characters) as ASCII; other characters as '?'. */
static void ascii_name(const uint8_t *p, int n, char *out, size_t cap) {
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++) {
        const unsigned c = (unsigned)p[2 * i] | (unsigned)p[2 * i + 1] << 8;
        if (!c) break;
        out[k++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[k] = 0;
}

static int is_record(const uint8_t *p) {
    return rd32(p) == REC_MAGIC && rd32(p + 4) == REC_SIZE && p[REC_BODY] && !p[REC_BODY + 1];
}

/* The run of records starting at p (it must start with "Emilia", the first). */
static int run_at(const uint8_t *p, const uint8_t *end) {
    static const uint8_t EMILIA[] = { 'E', 0, 'm', 0, 'i', 0, 'l', 0, 'i', 0, 'a', 0, 0, 0 };
    if (!is_record(p) || memcmp(p + REC_BODY, EMILIA, sizeof EMILIA)) return 0;
    int n = 0;
    while (n < CHARSWAP_MAX && p + (n + 1) * REC_STRIDE <= end && is_record(p + n * REC_STRIDE)) n++;
    return n;
}

static void fill_names(void) {
    static const char *const RACE[] = { "Human", "Newman", "CAST", "Beast", "Deuman" };
    for (int i = 0; i < g_n; i++) {
        const uint8_t *b = (const uint8_t *)psp_mem_ptr(g_table + (uint32_t)i * REC_STRIDE + REC_BODY, 0xA0);
        ascii_name(b + 0x40, 32, g_name[i], sizeof g_name[i]);          /* the full name ... */
        if (!g_name[i][0]) ascii_name(b, 32, g_name[i], sizeof g_name[i]);   /* ... or the short one */
        const uint8_t race = b[CHARSWAP_APPEARANCE], sex = b[CHARSWAP_APPEARANCE + 1];
        snprintf(g_kind[i], sizeof g_kind[i], "%s %c", race < 5 ? RACE[race] : "?", sex ? 'F' : 'M');
    }
}

/* Finds the records (again, if the game has moved them). */
static int find_table(void) {
    const uint8_t *lo = (const uint8_t *)psp_mem_ptr(SCAN_LO, SCAN_HI - SCAN_LO);
    if (!lo) return 0;
    const uint8_t *end = lo + (SCAN_HI - SCAN_LO);
    if (g_table && g_n && run_at(lo + (g_table - SCAN_LO), end) == g_n) return g_n;
    g_table = 0; g_n = 0;
    for (const uint8_t *p = lo; p + REC_STRIDE <= end; p += 4) {
        if (rd32(p) != REC_MAGIC) continue;
        const int n = run_at(p, end);
        if (n > g_n) { g_n = n; g_table = SCAN_LO + (uint32_t)(p - lo); }
    }
    if (g_n) fill_names();
    return g_n;
}

/* Your character data, or 0 when none is loaded. */
static uint32_t chardata(void) {
    const uint32_t save = psp_read32(CHARSWAP_SAVE_PTR);
    if (!save || !psp_mem_ptr(save + CHARSWAP_CHARDATA, CHARSWAP_APPEARANCE + APP_BYTES)) return 0;
    return save + CHARSWAP_CHARDATA;
}

int charswap_count(void) { return chardata() ? find_table() : 0; }
const char *charswap_name(int i) { return i >= 0 && i < g_n ? g_name[i] : ""; }
const char *charswap_kind(int i) { return i >= 0 && i < g_n ? g_kind[i] : ""; }

/* The bytes a swap writes: race and sex (header +0, +1) and the six part slots (24).
 * Header +2 is the class (0 Hunter, 1 Ranger, 2 Force, 3 Vanguard) and +3 is
 * not the look either: both stay yours, or your weapons follow the other
 * character's class. */
static int same_look(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 2) && !memcmp(a + 8, b + 8, 24); }

int charswap_current(void) {
    const uint32_t cd = chardata();
    if (!cd || !find_table()) return -1;
    const uint8_t *mine = (const uint8_t *)psp_mem_ptr(cd + CHARSWAP_APPEARANCE, APP_BYTES);
    for (int i = 0; i < g_n; i++) {
        const uint8_t *r = (const uint8_t *)psp_mem_ptr(g_table + (uint32_t)i * REC_STRIDE + REC_BODY + CHARSWAP_APPEARANCE, APP_BYTES);
        if (same_look(mine, r)) return i;
    }
    return -1;
}

/* ---- the backup of your own look -------------------------------------------------- */

static void my_name(uint32_t cd, char *out, size_t cap) { ascii_name((const uint8_t *)psp_mem_ptr(cd, 64), 32, out, cap); }

static int read_backup(char *name, size_t ncap, uint8_t app[APP_BYTES]) {
    FILE *f = fopen(g_backup_path, "r");
    if (!f) return 0;
    char line[256];
    int got_name = 0, got_app = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "name=", 5)) { snprintf(name, ncap, "%s", line + 5); got_name = 1; }
        else if (!strncmp(line, "appearance=", 11)) {
            const char *h = line + 11;
            int k = 0;
            unsigned v;
            while (k < (int)APP_BYTES && sscanf(h, "%2x", &v) == 1) { app[k++] = (uint8_t)v; h += 2; while (*h == ' ') h++; }
            got_app = k == (int)APP_BYTES;
        }
    }
    fclose(f);
    return got_name && got_app;
}

static int write_backup(const char *name, const uint8_t app[APP_BYTES]) {
    FILE *f = fopen(g_backup_path, "w");
    if (!f) return 0;
    fprintf(f, "# PSP2i: your own character's look, put back by F2 > CHARACTER > MY OWN CHARACTER\n");
    fprintf(f, "name=%s\nappearance=", name);
    for (unsigned k = 0; k < APP_BYTES; k++) fprintf(f, "%02x%s", app[k], k + 1 < APP_BYTES ? " " : "\n");
    return fclose(f) == 0;
}

int charswap_have_backup(void) {
    char name[64];
    uint8_t app[APP_BYTES];
    return read_backup(name, sizeof name, app);
}

int charswap_apply(int i, char *msg, size_t cap) {
    const uint32_t cd = chardata();
    if (!cd) { snprintf(msg, cap, "No character is loaded yet."); return -1; }
    if (!find_table()) { snprintf(msg, cap, "The partner data isn't loaded here: try in the lobby."); return -1; }
    if (i < 0 || i >= g_n) { snprintf(msg, cap, "No such character."); return -1; }
    uint8_t *mine = (uint8_t *)psp_mem_ptr(cd + CHARSWAP_APPEARANCE, APP_BYTES);
    const uint8_t *r = (const uint8_t *)psp_mem_ptr(g_table + (uint32_t)i * REC_STRIDE + REC_BODY + CHARSWAP_APPEARANCE, APP_BYTES);
    if (charswap_current() < 0) {   /* wearing your own look: keep it first */
        char name[64];
        my_name(cd, name, sizeof name);
        if (!write_backup(name, mine)) { snprintf(msg, cap, "Couldn't save your own look to character_backup.txt: nothing changed."); return -1; }
    }
    {   /* your class and header +3 from the backup: a swap made before they were kept may have changed them */
        char name[64], now[64];
        uint8_t own[APP_BYTES];
        my_name(cd, now, sizeof now);
        if (read_backup(name, sizeof name, own) && !strcmp(name, now)) { mine[2] = own[2]; mine[3] = own[3]; }
    }
    memcpy(mine, r, 2);             /* race, sex */
    memcpy(mine + 8, r + 8, 24);    /* the six part slots; your class (+2, +3) and +4 stay yours */
    snprintf(msg, cap, "Now %s: it shows when you next enter an area.", g_name[i]);
    fprintf(stderr, "charswap: wearing %s (record %d at %08X)\n", g_name[i], i, g_table + (uint32_t)i * REC_STRIDE);
    return 0;
}

int charswap_restore(char *msg, size_t cap) {
    const uint32_t cd = chardata();
    if (!cd) { snprintf(msg, cap, "No character is loaded yet."); return -1; }
    char name[64], now[64];
    uint8_t app[APP_BYTES];
    if (!read_backup(name, sizeof name, app)) {
        snprintf(msg, cap, charswap_current() >= 0 ? "Your own look wasn't saved (character_backup.txt is missing)."
                                                    : "You're already wearing your own look.");
        return -1;
    }
    my_name(cd, now, sizeof now);
    if (strcmp(name, now)) { snprintf(msg, cap, "The saved look is %s's, not this character's.", name); return -1; }
    memcpy(psp_mem_ptr(cd + CHARSWAP_APPEARANCE, APP_BYTES), app, APP_BYTES);
    remove(g_backup_path);
    snprintf(msg, cap, "Your own look again: it shows when you next enter an area.");
    fprintf(stderr, "charswap: own look restored\n");
    return 0;
}
