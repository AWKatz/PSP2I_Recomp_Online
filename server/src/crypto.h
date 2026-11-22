/* SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, OS randomness, password hashes.
 * Self-contained (no OpenSSL): the server stays dependency-free. */
#ifndef PSP2I_SERVER_CRYPTO_H
#define PSP2I_SERVER_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

void sha256(const void *data, size_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
void pbkdf2_sha256(const void *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                   uint32_t iterations, uint8_t *out, size_t outlen);
/* Cryptographically secure random bytes from the OS. 0 on success. */
int  os_random(void *buf, size_t len);

#define PW_HASH_MAX 128
#define PW_ITERATIONS 100000u
/* "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>". 0 on success. */
int  password_hash(const char *password, char out[PW_HASH_MAX]);
/* 1 if password matches stored (constant-time compare), else 0. */
int  password_verify(const char *password, const char *stored);

#endif
