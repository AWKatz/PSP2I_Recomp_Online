/* Tests for the character swap (host/charswap.c), against synthetic partner
 * records and character data in the emulated RAM (no game data needed).
 *
 *   cmake --build <build dir> --target test_charswap && <build dir>/test_charswap */

#include "charswap.h"

#include <psprecomp/mem.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_run;
#define CHECK(c) do { g_run++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define TABLE 0x0937ABE0u
#define SAVE  0x0900C3C0u
#define CD    (SAVE + CHARSWAP_CHARDATA)

static void put_name(uint32_t a, const char *s) {
    for (int i = 0; s[i]; i++) { psp_write8(a + 2u * (uint32_t)i, (uint8_t)s[i]); psp_write8(a + 2u * (uint32_t)i + 1, 0); }
}

/* Record i: "NUR\0", size 0xB0, then a character-data body. */
static void put_record(int i, const char *shortn, const char *full, uint8_t race, uint8_t sex, uint16_t part) {
    const uint32_t r = TABLE + (uint32_t)i * 0xC0u, b = r + 0x10u;
    psp_write32(r, 0x0052554Eu);
    psp_write32(r + 4, 0xB0u);
    put_name(b, shortn);
    if (full) put_name(b + 0x40, full);
    psp_write8(b + 0x80, race); psp_write8(b + 0x81, sex); psp_write8(b + 0x82, 3); psp_write8(b + 0x83, 0x69);
    psp_write32(b + 0x84, 1);
    for (int k = 0; k < 6; k++) psp_write32(b + 0x88 + 4u * (uint32_t)k, (uint32_t)(0x10 + k) << 24 | (uint32_t)part << 8);
}

static void put_me(void) {
    psp_write32(CHARSWAP_SAVE_PTR, SAVE);
    put_name(CD, "Client A");
    static const uint8_t MINE[32] = { 0x00, 0x00, 0x01, 0x0D, 0x7F, 0, 0, 0, 0x00, 0x18, 0x00, 0x00, 0x00, 0x18, 0x00, 0x01,
                                      0x00, 0x18, 0x00, 0x02, 0x00, 0x32, 0x00, 0x03, 0x02, 0x70, 0x00, 0x04, 0x00, 0x96, 0x00, 0x05 };
    psp_mem_write_block(CD + CHARSWAP_APPEARANCE, MINE, sizeof MINE);
    psp_write32(CD + 0xA0, 0x00040404u);   /* colours: never touched */
}

int main(void) {
    if (psp_mem_init() != 0) { fprintf(stderr, "psp_mem_init failed\n"); return 1; }
    char path[64];
    snprintf(path, sizeof path, "%s/character_backup.txt", ".");
    remove(path);
    charswap_init(".");

    CHECK(charswap_count() == 0);                         /* nothing loaded: the title screen */
    put_me();
    CHECK(charswap_count() == 0);                         /* a character, but no partner records */

    put_record(0, "Emilia", "Emilia Percival", 0, 1, 0x13A7);
    put_record(1, "Yut", "Yut Jun Yunkers", 1, 0, 0x03A5);
    put_record(2, "GH410", NULL, 2, 1, 0x13CA);
    CHECK(charswap_count() == 3);
    CHECK(!strcmp(charswap_name(0), "Emilia Percival"));
    CHECK(!strcmp(charswap_name(2), "GH410"));            /* no full name: the short one */
    CHECK(!strcmp(charswap_kind(0), "Human F"));
    CHECK(!strcmp(charswap_kind(1), "Newman M"));
    CHECK(!strcmp(charswap_kind(2), "CAST F"));
    CHECK(charswap_current() == -1);
    CHECK(!charswap_have_backup());

    /* other copies of the character (the game keeps several): same name and look -> changed with
     * it; another name, or another look -> left alone */
    const uint32_t COPY = 0x09A00000u, OTHER = 0x09A01000u, STALE = 0x09A02000u;
    uint8_t look[32];
    psp_mem_read_block(look, CD + CHARSWAP_APPEARANCE, 32);
    put_name(COPY, "Client A");  psp_mem_write_block(COPY + 0x80, look, 32);
    put_name(OTHER, "Client B"); psp_mem_write_block(OTHER + 0x80, look, 32);
    put_name(STALE, "Client A"); psp_mem_write_block(STALE + 0x80, look, 32); psp_write8(STALE + 0x9F, 0x77);

    char msg[128];
    uint8_t before[32];
    psp_mem_read_block(before, CD + CHARSWAP_APPEARANCE, 32);
    CHECK(charswap_apply(0, msg, sizeof msg) == 0);
    CHECK(charswap_current() == 0);
    CHECK(charswap_have_backup());
    CHECK(psp_read8(CD + 0x80) == 0 && psp_read8(CD + 0x81) == 1);   /* race and sex: hers */
    CHECK(psp_read8(CD + 0x82) == 1 && psp_read8(CD + 0x83) == 0x0D);   /* class (Ranger) and +3: yours */
    CHECK(psp_read32(CD + 0x84) == 0x7F);                 /* the word stays yours */
    CHECK(psp_read32(CD + 0x88) == 0x1013A700u && psp_read32(CD + 0x9C) == 0x1513A700u);
    CHECK(psp_read32(CD + 0xA0) == 0x00040404u);          /* colours stay yours */
    {
        uint8_t now[32], c[32];
        psp_mem_read_block(now, CD + CHARSWAP_APPEARANCE, 32);
        psp_mem_read_block(c, COPY + 0x80, 32);  CHECK(!memcmp(c, now, 32));    /* the copy follows */
        psp_mem_read_block(c, OTHER + 0x80, 32); CHECK(!memcmp(c, look, 32));   /* another character's: not */
        psp_mem_read_block(c, STALE + 0x80, 31); CHECK(!memcmp(c, look, 31) && psp_read8(STALE + 0x9F) == 0x77);
    }

    CHECK(charswap_apply(2, msg, sizeof msg) == 0);       /* a second swap keeps the first backup */
    CHECK(charswap_current() == 2);
    CHECK(charswap_apply(9, msg, sizeof msg) != 0);
    /* a swap from before the class was kept: the next one takes yours from the backup */
    psp_write8(CD + 0x82, 0); psp_write8(CD + 0x83, 0x74);
    CHECK(charswap_apply(1, msg, sizeof msg) == 0);
    CHECK(psp_read8(CD + 0x82) == 1 && psp_read8(CD + 0x83) == 0x0D);
    CHECK(charswap_current() == 1);

    /* the game moved the records: found again */
    for (uint32_t a = TABLE; a < TABLE + 3 * 0xC0u; a += 4) psp_write32(a, 0);
    CHECK(charswap_count() == 0);

    /* a restart: the swapped look was saved; the backup still brings yours back */
    charswap_init(".");
    CHECK(charswap_restore(msg, sizeof msg) == 0);
    uint8_t after[32];
    psp_mem_read_block(after, CD + CHARSWAP_APPEARANCE, 32);
    CHECK(!memcmp(before, after, 32));
    CHECK(charswap_have_backup());                        /* kept: a restart may bring the saved swap back */
    {   /* the copy has the own look back too */
        uint8_t c[32];
        psp_mem_read_block(c, COPY + 0x80, 32);
        CHECK(!memcmp(c, before, 32));
    }
    CHECK(charswap_restore(msg, sizeof msg) != 0 && strstr(msg, "already"));   /* already your own look */

    /* the backup belongs to another character: refused */
    put_record(0, "Emilia", "Emilia Percival", 0, 1, 0x13A7);
    CHECK(charswap_apply(0, msg, sizeof msg) == 0);
    put_name(CD, "Client B");
    CHECK(charswap_restore(msg, sizeof msg) != 0);
    CHECK(strstr(msg, "Client A") != NULL);
    remove(path);

    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
