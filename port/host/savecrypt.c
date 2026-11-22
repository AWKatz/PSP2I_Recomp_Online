/* PSP savedata encryption for the recomp: the psp_savedata_crypto provider.
 *
 * On a PSP, the savedata utility encrypts SAVEDATA.BIN and signs PARAM.SFO
 * through sceChnnlsv (sceSd*) on top of the KIRK crypto engine. This is a C
 * port of PPSSPP's model of it: Core/HLE/sceChnnlsv.cpp, plus DecryptData,
 * EncryptData, UpdateHash and BuildHash from Core/Dialog/SavedataParam.cpp,
 * at commit 5ce7f1cfeae1bfcb1a5e88fef74ce127c696cdfc. KIRK itself is
 * PPSSPP's vendored libkirk (port/third_party/libkirk).
 *
 * Saves from PPSSPP (and a PSP's, for the modes without the console's
 * fuse ID: 1, 3 and 5) load here, and saves written here load in PPSSPP.
 * The fuse commands (KIRK 5/8) use libkirk's fixed fuse ID, as current
 * PPSSPP does; only one PARAM.SFO hash depends on it, and nothing here
 * checks that hash on load. Older PPSSPP had no fuse commands and filled
 * that hash with 0x01 (savecrypt_set_fuse(0) does the same, for tests). */

#include "savecrypt.h"

#include "kirk_engine.h"

#include <psprecomp/hle.h>

#include <stdlib.h>
#include <string.h>

typedef struct { int mode; u8 result[16]; u8 key[16]; int key_len; } mac_ctx;     /* pspChnnlsvContext1 */
typedef struct { int mode; int unkn; u8 crypted[16]; } cipher_ctx;               /* pspChnnlsvContext2 */

static KirkState g_kirk;
static int g_kirk_ready;
static int g_fuse = 1;
static u8 g_buf[2048 + 20];
static u8 *const g_buf2 = g_buf + 20;

static const u8 hash198C[16] = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
static const u8 hash19BC[16] = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
static const u8 key19CC[16]  = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
static const u8 key19DC[16]  = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};
static const u8 key199C[16]  = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
static const u8 key19AC[16]  = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};

static void memxor(u8 *d, const u8 *s, size_t n) { while (n--) *d++ ^= *s++; }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static int num_from_mode(int mode) {
    switch (mode) { case 1: return 3; case 2: return 5; case 3: return 12; case 4: return 13; case 6: return 17; default: return 16; }
}
static int num_from_mode2(int mode) { return mode == 1 ? 4 : mode == 3 ? 14 : 18; }
static int type_from_mode(int mode) { return (mode == 1 || mode == 2) ? 83 : (mode == 3 || mode == 4) ? 87 : 100; }

static int kirk_cmd(u8 *data, int length, int num, int encrypt) {
    put32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    put32(data + 4, 0);
    put32(data + 8, 0);
    put32(data + 12, (u32)num);
    put32(data + 16, (u32)length);
    return kirk_sceUtilsBufferCopyWithRange(&g_kirk, data, length + 20, data, length + 20,
                                            encrypt ? KIRK_CMD_ENCRYPT_IV_0 : KIRK_CMD_DECRYPT_IV_0) ? -257 : 0;
}
static int kirk_fuse_cmd(u8 *data, int length, int encrypt) {
    put32(data + 0, encrypt ? KIRK_MODE_ENCRYPT_CBC : KIRK_MODE_DECRYPT_CBC);
    put32(data + 4, 0);
    put32(data + 8, 0);
    put32(data + 12, 256);
    put32(data + 16, (u32)length);
    if (!g_fuse) return -258;
    return kirk_sceUtilsBufferCopyWithRange(&g_kirk, data, length + 20, data, length + 20,
                                            encrypt ? KIRK_CMD_ENCRYPT_IV_FUSE : KIRK_CMD_DECRYPT_IV_FUSE) ? -258 : 0;
}

static int sub_15B0(u8 *data, int aligned, u8 *buf, int val) {
    u8 sp0[16];
    memcpy(sp0, data + aligned + 4, 16);
    const int res = kirk_cmd(data, aligned, val, 0);
    if (res) return res;
    memxor(data, buf, 16);
    memcpy(buf, sp0, 16);
    return 0;
}

static int sub_0000(u8 *out, u8 *data, int aligned, const u8 *data2, int *data3, int mode) {
    memcpy(out + 20, data2, 16);
    const int type = type_from_mode(mode);
    if (type == 87) memxor(out + 20, key19AC, 16);
    else if (type == 100) memxor(out + 20, key19DC, 16);
    int res = (mode == 2 || mode == 4 || mode == 6) ? kirk_fuse_cmd(out, 16, 0) : kirk_cmd(out, 16, num_from_mode2(mode), 0);
    if (type == 87) memxor(out, key199C, 16);
    else if (type == 100) memxor(out, key19CC, 16);
    if (res) return res;

    u8 sp0[16], sp16[16];
    memcpy(sp16, out, 16);
    if (*data3 == 1) memset(sp0, 0, 16);
    else { memcpy(sp0, sp16, 12); put32(sp0 + 12, (u32)(*data3 - 1)); }
    for (int i = 20; i < aligned + 20; i += 16) {
        memcpy(out + i, sp16, 12);
        put32(out + 12 + i, (u32)*data3);
        (*data3)++;
    }
    res = sub_15B0(out, aligned, sp0, type);
    if (res) return res;
    if (aligned > 0) memxor(data, out, (size_t)aligned);
    return 0;
}

static int sub_1510(u8 *data, int size, u8 *result, int num) {
    memxor(data + 20, result, 16);
    const int res = kirk_cmd(data, size, num, 1);
    if (res) return res;
    memcpy(result, data + size + 4, 16);
    return 0;
}

static void mac_init(mac_ctx *c, int mode) { memset(c, 0, sizeof *c); c->mode = mode; }

static int mac_update(mac_ctx *c, const u8 *data, int length) {
    if (c->key_len < 0 || c->key_len >= 17) return -1026;
    if (c->key_len + length < 17) {
        memcpy(c->key + c->key_len, data, (size_t)length);
        c->key_len += length;
        return 0;
    }
    const int num = num_from_mode(c->mode);
    memset(g_buf2, 0, 2048);
    memcpy(g_buf2, c->key, (size_t)c->key_len);
    int len = (c->key_len + length) & 0xF;
    if (len == 0) len = 16;
    int size = c->key_len;
    c->key_len = len;
    const int diff = length - len;
    memcpy(c->key, data + diff, (size_t)len);
    for (int i = 0; i < diff; i++) {
        if (size == 2048) {
            const int res = sub_1510(g_buf, 2048, c->result, num);
            if (res) return res;
            size = 0;
        }
        g_buf2[size++] = data[i];
    }
    if (size) sub_1510(g_buf, size, c->result, num);
    return 0;
}

static void shift_left_1(u8 *d) {
    const int carry = (d[0] & 0x80) ? 135 : 0;
    for (int i = 0; i < 15; i++) d[i] = (u8)((d[i] << 1) | (d[i + 1] >> 7));
    d[15] = (u8)((d[15] << 1) ^ carry);
}

static int mac_final(mac_ctx *c, u8 *hash, const u8 *in_key) {
    if (c->key_len < 0 || c->key_len >= 17) return -1026;
    const int num = num_from_mode(c->mode);
    memset(g_buf2, 0, 16);
    int res = kirk_cmd(g_buf, 16, num, 1);
    if (res) return res;

    u8 d1[16], d2[16];
    memcpy(d1, g_buf2, 16);
    shift_left_1(d1);
    if (c->key_len < 16) {
        shift_left_1(d1);
        c->key[c->key_len] = 0x80;
        if (c->key_len + 1 < 16) memset(c->key + c->key_len + 1, 0, (size_t)(16 - c->key_len - 1));
    }
    memxor(c->key, d1, 16);
    memcpy(g_buf2, c->key, 16);
    memcpy(d2, c->result, 16);
    res = sub_1510(g_buf, 16, d2, num);
    if (res) return res;

    if (c->mode == 3 || c->mode == 4) memxor(d2, hash198C, 16);
    else if (c->mode == 5 || c->mode == 6) memxor(d2, hash19BC, 16);

    if (c->mode == 2 || c->mode == 4 || c->mode == 6) {
        memcpy(g_buf2, d2, 16);
        res = kirk_fuse_cmd(g_buf, 16, 1);
        if (res) return res;
        res = kirk_cmd(g_buf, 16, num, 1);
        if (res) return res;
        memcpy(d2, g_buf2, 16);
    }
    if (in_key) {
        for (int i = 0; i < 16; i++) d2[i] ^= in_key[i];
        memcpy(g_buf2, d2, 16);
        res = kirk_cmd(g_buf, 16, num, 1);
        if (res) return res;
        memcpy(d2, g_buf2, 16);
    }
    memcpy(hash, d2, 16);
    mac_init(c, 0);
    return 0;
}

/* how = 1: encrypt (makes a new IV in data[0..16)); 2: decrypt (reads it) */
static int cipher_init(cipher_ctx *c, int mode, int how, u8 *data, const u8 *key) {
    c->mode = mode;
    c->unkn = 1;
    if (how == 2) {
        memcpy(c->crypted, data, 16);
        if (key) memxor(c->crypted, key, 16);
        return 0;
    }
    u8 hdr[37];
    u8 *kd = hdr + 20;
    if (kirk_sceUtilsBufferCopyWithRange(&g_kirk, hdr, 20, 0, 0, 14) != 0) return -261;   /* KIRK 14: random */
    memcpy(hdr + 20, hdr, 16);
    memset(hdr + 32, 0, 4);
    const int type = type_from_mode(mode);
    if (type == 87) memxor(kd, key199C, 16);
    else if (type == 100) memxor(kd, key19CC, 16);
    const int res = (mode == 2 || mode == 4 || mode == 6) ? kirk_fuse_cmd(hdr, 16, 1) : kirk_cmd(hdr, 16, num_from_mode2(mode), 1);
    if (type == 87) memxor(kd, key19AC, 16);
    else if (type == 100) memxor(kd, key19DC, 16);
    if (res) return res;
    memcpy(c->crypted, kd, 16);
    memcpy(data, kd, 16);
    if (key) memxor(c->crypted, key, 16);
    return 0;
}

static int cipher_update(cipher_ctx *c, u8 *data, int aligned) {
    if (aligned == 0) return 0;
    if (aligned & 0xF) return -1025;
    u8 kd[20 + 2048];
    int i = 0;
    for (; aligned >= 2048; i += 2048, aligned -= 2048) {
        const int res = sub_0000(kd, data + i, 2048, c->crypted, &c->unkn, c->mode);
        if (res) return res;
    }
    return aligned ? sub_0000(kd, data + i, aligned, c->crypted, &c->unkn, c->mode) : 0;
}

static void ensure_kirk(void) {
    if (!g_kirk_ready) { kirk_init(&g_kirk); g_kirk_ready = 1; }
}

static uint32_t align16(uint32_t n) { return (n + 15u) & ~15u; }

/* DecryptData: the file is a 16-byte IV, then the data. */
static int sc_decrypt(int mode, const uint8_t *file, uint32_t file_len, const uint8_t *key,
                      const uint8_t *expected_hash, uint8_t **out, uint32_t *out_len) {
    ensure_kirk();
    if (file_len <= 0x10 || file_len > 0x7FFFFFF0u) return -1;
    const uint32_t aligned = align16(file_len);
    u8 *d = (u8 *)calloc(1, aligned);
    if (!d) return -1;
    memcpy(d, file, file_len);
    const int body = (int)(aligned - 0x10);
    mac_ctx m;
    cipher_ctx c;
    mac_init(&m, mode);
    int rc = cipher_init(&c, mode, 2, d, key);
    if (!rc) rc = mac_update(&m, d, 0x10);
    if (!rc) rc = mac_update(&m, d + 0x10, body);
    if (!rc) rc = cipher_update(&c, d + 0x10, body);
    if (!rc && expected_hash) {
        u8 h[16];
        rc = mac_final(&m, h, key);
        if (!rc && memcmp(h, expected_hash, 16) != 0) rc = -8;
    }
    if (rc) { free(d); return rc; }
    memmove(d, d + 0x10, file_len - 0x10);
    *out = d;
    *out_len = file_len - 0x10;
    return 0;
}

/* EncryptData: a fresh IV, then the data; the hash goes into PARAM.SFO's file list. */
static int sc_encrypt(int mode, const uint8_t *plain, uint32_t len, const uint8_t *key,
                      uint8_t **out, uint32_t *out_len, uint8_t hash[16]) {
    ensure_kirk();
    if (len > 0x7FFFFFE0u) return -1;
    const uint32_t aligned = align16(len);
    u8 *d = (u8 *)calloc(1, aligned + 0x10);
    if (!d) return -1;
    memcpy(d + 0x10, plain, len);
    memset(hash, 0, 16);
    mac_ctx m;
    cipher_ctx c;
    int rc = cipher_init(&c, mode, 1, d, key);
    if (!rc) { mac_init(&m, mode); rc = mac_update(&m, d, 0x10); }
    if (!rc) rc = cipher_update(&c, d + 0x10, (int)aligned);
    if (!rc) { memset(d + 0x10 + len, 0, aligned - len); rc = mac_update(&m, d + 0x10, (int)aligned); }
    if (!rc) rc = mac_final(&m, hash, key);
    if (rc) { free(d); return rc; }
    *out = d;
    *out_len = len + 0x10;
    return 0;
}

/* BuildHash: as PPSSPP, a hash whose KIRK command is missing reads as 0x01s. */
static void build_hash(u8 *outh, const u8 *data, uint32_t aligned, int mode) {
    mac_ctx m;
    mac_init(&m, mode & 0xFF);
    mac_update(&m, data, (int)aligned);
    if (mac_final(&m, outh, NULL) < 0) memset(outh, 0x01, 16);
}

/* UpdateHash: SAVEDATA_PARAMS (128 bytes at params_off). */
static int sc_sfo_hash(uint8_t *sfo, uint32_t sfo_size, uint32_t params_off, int mode) {
    ensure_kirk();
    if (params_off + 128 > sfo_size) return -1;
    const uint32_t aligned = align16(sfo_size);
    u8 h[16];
    memset(sfo + params_off, 0, 128);
    int first = (mode & 2) ? 4 : 2, second = (mode & 2) ? 3 : 0;
    if (mode & 4) { first = 6; second = 5; }
    build_hash(h, sfo, aligned, first);
    memcpy(sfo + params_off + 0x20, h, 16);
    sfo[params_off] |= 0x01;
    if (mode & 6) {
        sfo[params_off] |= (u8)((mode & 6) << 4);
        build_hash(h, sfo, aligned, second);
        memcpy(sfo + params_off + 0x70, h, 16);
    }
    build_hash(h, sfo, aligned, 1);
    memcpy(sfo + params_off + 0x10, h, 16);
    return 0;
}

static const psp_savedata_crypto g_provider = { sc_decrypt, sc_encrypt, sc_sfo_hash };

const psp_savedata_crypto *savecrypt_provider(void) { return &g_provider; }

void savecrypt_set_fuse(int on) { g_fuse = on; }

void savecrypt_install(void) { psp_savedata_set_crypto(&g_provider); }
