/* The savedata crypto provider (host/savecrypt.c).
 *
 *  - Known answer: PARAM.SFO from a PSP2i save PPSSPP wrote (mode 5,
 *    tests/data/savecrypt_param.sfo), by a PPSSPP without the fuse commands.
 *    Recomputing SAVEDATA_PARAMS the same way must give its 128 bytes
 *    exactly: the 0x01 fill for the fuse hash, then the keyless mode-5 and
 *    mode-1 MACs over the file (each covers the earlier ones).
 *  - Round trips in modes 1, 3 and 5 (with and without a key, odd sizes and
 *    sizes over the 2048-byte KIRK block): decrypt(encrypt(x)) == x, with the
 *    hash checked; a wrong key or a flipped byte fails the hash. */
#include "savecrypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The data offset of SAVEDATA_PARAMS in a PSF file, or 0. */
static uint32_t params_offset(const uint8_t *s, uint32_t n) {
    if (n < 20 || rd32(s) != 0x46535000u) return 0;
    const uint32_t ko = rd32(s + 8), dof = rd32(s + 12), cnt = rd32(s + 16);
    for (uint32_t i = 0; i < cnt && 20 + i * 16 + 16 <= n; i++) {
        const uint8_t *e = s + 20 + i * 16;
        const uint32_t k = ko + (e[0] | e[1] << 8);
        if (k < n && !strcmp((const char *)s + k, "SAVEDATA_PARAMS")) return dof + rd32(e + 12);
    }
    return 0;
}

static void round_trip(const psp_savedata_crypto *c, int mode, uint32_t len, const uint8_t *key) {
    uint8_t *plain = malloc(len), *enc = NULL, *dec = NULL, hash[16], bad[16];
    uint32_t elen = 0, dlen = 0;
    for (uint32_t i = 0; i < len; i++) plain[i] = (uint8_t)(i * 7 + mode);
    CHECK(c->encrypt(mode, plain, len, key, &enc, &elen, hash) == 0);
    CHECK(elen == len + 16);
    CHECK(len < 32 || memcmp(enc + 16, plain, 32) != 0);              /* actually encrypted */
    CHECK(c->decrypt(mode, enc, elen, key, hash, &dec, &dlen) == 0);
    CHECK(dlen == len && dec && memcmp(dec, plain, len) == 0);
    free(dec); dec = NULL;
    if (key) {
        memcpy(bad, key, 16); bad[0] ^= 1;
        CHECK(c->decrypt(mode, enc, elen, bad, hash, &dec, &dlen) != 0);
        free(dec); dec = NULL;
    }
    enc[elen - 1] ^= 0x40;
    CHECK(c->decrypt(mode, enc, elen, key, hash, &dec, &dlen) != 0);
    free(dec); free(enc); free(plain);
}

int main(int argc, char **argv) {
    const psp_savedata_crypto *c = savecrypt_provider();

    if (argc < 2) { fprintf(stderr, "usage: test_savecrypt PARAM.SFO\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    uint8_t sfo[8192];
    const uint32_t n = (uint32_t)fread(sfo, 1, sizeof sfo, f);
    fclose(f);
    const uint32_t off = params_offset(sfo, n);
    CHECK(off && off + 128 <= n && (n & 15) == 0);
    uint8_t want[128];
    memcpy(want, sfo + off, 128);
    CHECK(want[0] == 0x41);
    savecrypt_set_fuse(0);
    CHECK(c->sfo_hash(sfo, n, off, 5) == 0);
    savecrypt_set_fuse(1);
    CHECK(memcmp(sfo + off, want, 128) == 0);
    if (memcmp(sfo + off, want, 128) != 0) {
        for (int i = 0; i < 128; i++) fprintf(stderr, "%02x%s", sfo[off + i], i % 16 == 15 ? "\n" : "");
    }

    const uint8_t key[16] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint32_t sizes[] = { 1, 16, 100, 2048, 2049, 5000, 301352 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        round_trip(c, 1, sizes[i], NULL);
        round_trip(c, 3, sizes[i], key);
        round_trip(c, 5, sizes[i], key);
    }

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("savecrypt: ok (PPSSPP PARAM.SFO hashes reproduced; modes 1/3/5 round-trip)");
    return 0;
}
