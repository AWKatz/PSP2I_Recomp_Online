/* psprecomp — the PSP2i MMO control protocol (client <-> server), MMO build only.
 *
 * Control messages ride the transport's reliable channel on port
 * PSP_MMO_CTRL_PORT (the game's own ad hoc traffic never uses port 0). Every
 * message starts with psp_mmo_hdr; all integers are little-endian and the
 * structs are packed byte-for-byte as written (encode/decode helpers below, no
 * struct casts on the wire). */
#ifndef PSPRECOMP_NET_PROTO_H
#define PSPRECOMP_NET_PROTO_H

#include <stdint.h>
#include <string.h>

#define PSP_MMO_CTRL_PORT     0
#define PSP_MMO_MAGIC         0x4D493250u   /* "P2IM" little-endian */
#define PSP_MMO_PROTO_VERSION 1

enum {
    MSG_C2S_HANDSHAKE     = 0x0001,
    MSG_S2C_HANDSHAKE_ACK = 0x8001,
};

enum { PSP_MMO_HDR_LEN = 8 };            /* magic u32, type u16, version u16 */
enum { PSP_MMO_NAME_LEN = 32, PSP_MMO_VERSION_LEN = 32 };

/* MSG_C2S_HANDSHAKE: hdr, mac[6], name[32] (NUL-padded), client version[32] */
enum { PSP_MMO_HANDSHAKE_LEN = PSP_MMO_HDR_LEN + 6 + PSP_MMO_NAME_LEN + PSP_MMO_VERSION_LEN };
/* MSG_S2C_HANDSHAKE_ACK: hdr, status u32 (0 = accepted), session id u64 */
enum { PSP_MMO_HANDSHAKE_ACK_LEN = PSP_MMO_HDR_LEN + 4 + 8 };

static inline void psp_mmo_put_hdr(uint8_t *b, uint16_t type) {
    const uint32_t m = PSP_MMO_MAGIC;
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(m >> (8 * i));
    b[4] = (uint8_t)type; b[5] = (uint8_t)(type >> 8);
    b[6] = PSP_MMO_PROTO_VERSION; b[7] = 0;
}
/* The message type, or 0 when b is not a valid control message. */
static inline uint16_t psp_mmo_type(const uint8_t *b, uint32_t len) {
    if (len < PSP_MMO_HDR_LEN) return 0;
    const uint32_t m = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (m != PSP_MMO_MAGIC || b[6] != PSP_MMO_PROTO_VERSION) return 0;
    return (uint16_t)(b[4] | (b[5] << 8));
}
static inline void psp_mmo_put_str(uint8_t *dst, const char *s, size_t cap) {
    memset(dst, 0, cap);
    if (s) { size_t n = strlen(s); if (n > cap - 1) n = cap - 1; memcpy(dst, s, n); }
}

#endif
