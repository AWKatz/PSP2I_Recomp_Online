/* SHA-256 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), password hashes. */
#if !defined(_WIN32) && !defined(__APPLE__)
#  define _DEFAULT_SOURCE
#endif
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#elif defined(__APPLE__)
#  include <stdlib.h>          /* arc4random_buf */
#else
#  include <sys/random.h>      /* getrandom */
#  include <errno.h>
#endif

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(sha_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_init(sha_ctx *c) {
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, H0, sizeof H0);
    c->len = 0;
    c->n = 0;
}
static void sha_update(sha_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        const size_t take = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}
static void sha_final(sha_ctx *c, uint8_t out[32]) {
    const uint64_t bits = c->len * 8;
    const uint8_t one = 0x80, zero = 0;
    sha_update(c, &one, 1);
    while (c->n != 56) sha_update(c, &zero, 1);
    uint8_t L[8];
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(c, L, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
    sha_ctx c;
    sha_init(&c);
    sha_update(&c, data, len);
    sha_final(&c, out);
}

/* HMAC with the key's pads precomputed (PBKDF2 reuses them every iteration) */
typedef struct { sha_ctx in, out; } hmac_ctx;
static void hmac_init(hmac_ctx *h, const uint8_t *key, size_t klen) {
    uint8_t k[64] = { 0 }, ip[64], op[64];
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    sha_init(&h->in); sha_update(&h->in, ip, 64);
    sha_init(&h->out); sha_update(&h->out, op, 64);
}
static void hmac_run(const hmac_ctx *h, const void *msg, size_t mlen, uint8_t out[32]) {
    sha_ctx c = h->in;
    uint8_t inner[32];
    sha_update(&c, msg, mlen);
    sha_final(&c, inner);
    c = h->out;
    sha_update(&c, inner, 32);
    sha_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]) {
    hmac_ctx h;
    hmac_init(&h, key, klen);
    hmac_run(&h, msg, mlen, out);
}

void pbkdf2_sha256(const void *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                   uint32_t iterations, uint8_t *out, size_t outlen) {
    hmac_ctx h;
    hmac_init(&h, pw, pwlen);
    uint8_t *msg = malloc(slen + 4);
    if (!msg) { memset(out, 0, outlen); return; }
    memcpy(msg, salt, slen);
    for (uint32_t blk = 1; outlen; blk++) {
        msg[slen] = (uint8_t)(blk >> 24); msg[slen + 1] = (uint8_t)(blk >> 16); msg[slen + 2] = (uint8_t)(blk >> 8); msg[slen + 3] = (uint8_t)blk;
        uint8_t u[32], t[32];
        hmac_run(&h, msg, slen + 4, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&h, u, 32, u);
            for (int k = 0; k < 32; k++) t[k] ^= u[k];
        }
        const size_t take = outlen < 32 ? outlen : 32;
        memcpy(out, t, take);
        out += take; outlen -= take;
    }
    free(msg);
}

int os_random(void *buf, size_t len) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#elif defined(__APPLE__)
    arc4random_buf(buf, len);
    return 0;
#else
    uint8_t *p = buf;
    while (len) {
        const ssize_t n = getrandom(p, len, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
#endif
}

static void hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}
static int unhex(const char *s, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = s[2 * i + k];
            const int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (x < 0) return -1;
            v = v * 16 + x;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

int password_hash(const char *password, char out[PW_HASH_MAX]) {
    uint8_t salt[16], dk[32];
    if (os_random(salt, sizeof salt) != 0) return -1;
    pbkdf2_sha256(password, strlen(password), salt, sizeof salt, PW_ITERATIONS, dk, sizeof dk);
    char sh[33], dh[65];
    hex(salt, sizeof salt, sh);
    hex(dk, sizeof dk, dh);
    snprintf(out, PW_HASH_MAX, "pbkdf2-sha256$%u$%s$%s", PW_ITERATIONS, sh, dh);
    return 0;
}

int password_verify(const char *password, const char *stored) {
    unsigned iter = 0;
    char sh[33], dh[65];
    if (!stored || sscanf(stored, "pbkdf2-sha256$%u$%32[0-9a-f]$%64[0-9a-f]", &iter, sh, dh) != 3) return 0;
    if (iter < 1 || iter > 10000000 || strlen(sh) != 32 || strlen(dh) != 64) return 0;
    uint8_t salt[16], want[32], got[32];
    if (unhex(sh, 16, salt) || unhex(dh, 32, want)) return 0;
    pbkdf2_sha256(password, strlen(password), salt, sizeof salt, iter, got, sizeof got);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(got[i] ^ want[i]);
    return diff == 0;
}
