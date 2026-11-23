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
    MSG_LAN_PROBE         = 0x0002,   /* UDP broadcast to PSP_MMO_LAN_PORT */
    MSG_LAN_REPLY         = 0x8002,
    MSG_C2S_ENEMY_KILLED  = 0x0010,   /* a player killed an enemy: the server rolls the loot */
    MSG_S2C_SPAWN_DROP    = 0x8010,   /* to every player: a drop appears */
    MSG_S2C_KILL_REWARD   = 0x8011,   /* to the killer: experience and type points */
    MSG_S2C_SYSTEM_NOTICE = 0x8020,   /* an announcement for every player's HUD */
    MSG_S2C_SERVER_EVENTS = 0x8021,   /* the active seasonal events / lobby themes */
    MSG_C2S_PARTY_STATE   = 0x0030,   /* my party role (Multi Mode), whenever it changes */
    MSG_C2S_PARTY_LIST_REQ = 0x0031,  /* send me the party directory */
    MSG_S2C_PARTY_LIST    = 0x8030,   /* the party directory: every party a leader is hosting */
    MSG_C2S_PARTY_WARP    = 0x0032,   /* a leader: take my party to this area */
    MSG_S2C_PARTY_WARP    = 0x8032,   /* to the leader and every member: move to this area */
};

/* The party directory (Multi Mode). A client reports its role in the game's
 * ad hoc party; the server lists the parties being hosted, so a player can
 * pick one before the game's own Join Party search.
 * MSG_C2S_PARTY_STATE: hdr, role u8 (PSP_MMO_PARTY_*), members u8 (with the
 * leader), max u8 (the game's party size), open u8 (1 = taking members),
 * leader mac[6] (a member: its leader; a leader: itself) */
enum { PSP_MMO_PARTY_NONE = 0, PSP_MMO_PARTY_LEADER = 1, PSP_MMO_PARTY_MEMBER = 2 };
enum { PSP_MMO_PARTY_STATE_LEN = 8 + 4 + 6 };
/* MSG_C2S_PARTY_LIST_REQ: hdr */
enum { PSP_MMO_PARTY_LIST_REQ_LEN = 8 };
/* MSG_S2C_PARTY_LIST: hdr, players online u16, count u16, then count entries:
 * leader mac[6], members u8, max u8, open u8, pad u8, leader name[32] */
enum { PSP_MMO_PARTY_ENTRY_LEN = 6 + 4 + 32, PSP_MMO_PARTY_LIST_MAX = 32,
       PSP_MMO_PARTY_LIST_HDR_LEN = 8 + 4 };

/* MSG_C2S_PARTY_WARP / MSG_S2C_PARTY_WARP: hdr, destination i32 (quest table
 * * 1000 + entry), area index i16, zone i16, entry i16, pad u16. The server
 * takes it from a party leader only and sends it to the leader and every
 * session whose reported leader is that leader. */
enum { PSP_MMO_PARTY_WARP_LEN = 8 + 12 };
enum { PSP_MMO_WARP_OPTIONS = 1, PSP_MMO_WARP_WEAPONS = 2 };   /* flags: options are set; weapons in the lobby */

/* MSG_S2C_SYSTEM_NOTICE: hdr, colour u32 (0xRRGGBB), text[128] (UTF-8, NUL-padded) */
enum { PSP_MMO_NOTICE_TEXT_LEN = 128, PSP_MMO_NOTICE_LEN = 8 + 4 + PSP_MMO_NOTICE_TEXT_LEN };
/* MSG_S2C_SERVER_EVENTS: hdr, active event bits u32 (bit i = the server's event i) */
enum { PSP_MMO_EVENTS_LEN = 8 + 4 };

/* MSG_C2S_ENEMY_KILLED: hdr, enemy id u32, mission id u32, x/y/z f32 */
enum { PSP_MMO_ENEMY_KILLED_LEN = 8 + 4 + 4 + 12 };
/* MSG_S2C_SPAWN_DROP: hdr, drop id u64, enemy id u32, item id u32 (0 = Meseta),
 * meseta u32, flags u32 (PSP_MMO_DROP_*), x/y/z f32, killer session u64 */
enum { PSP_MMO_SPAWN_DROP_LEN = 8 + 8 + 4 + 4 + 4 + 4 + 12 + 8 };
enum { PSP_MMO_DROP_RARE = 1 };
/* MSG_S2C_KILL_REWARD: hdr, enemy id u32, exp u32, type points u32 */
enum { PSP_MMO_KILL_REWARD_LEN = 8 + 4 + 4 + 4 };

#define PSP_MMO_LAN_PORT    29000     /* LAN discovery (UDP) */
/* What a server and client must agree on to play together; its FNV-1a hash
 * is the version hash in MSG_LAN_REPLY. Bump with PSP_MMO_PROTO_VERSION. */
#define PSP_MMO_VERSION_STRING "psp2i-mmo/1"
#define PSP_MMO_GAME_PORT   29001     /* the server (ENet) */

/* MSG_S2C_HANDSHAKE_ACK status */
enum {
    PSP_MMO_OK = 0,
    PSP_MMO_ERR_VERSION   = 1,        /* protocol version or message malformed */
    PSP_MMO_ERR_NAME      = 2,        /* empty or unprintable player name */
    PSP_MMO_ERR_FULL      = 3,
    PSP_MMO_ERR_DUPLICATE = 4,        /* that MAC is already in a session */
    PSP_MMO_ERR_MAC       = 5,        /* zero, broadcast or multicast MAC */
    PSP_MMO_ERR_BANNED    = 6,        /* this address is banned */
};

/* Disconnect reasons (the ENet disconnect data a client sees) */
enum {
    PSP_MMO_DISC_NONE      = 0,
    PSP_MMO_DISC_KICKED    = 1,
    PSP_MMO_DISC_HANDSHAKE = 2,       /* rejected handshake (status in the ACK) */
    PSP_MMO_DISC_TIMEOUT   = 3,       /* no handshake in time */
    PSP_MMO_DISC_RATE      = 4,       /* rate limit exceeded */
    PSP_MMO_DISC_PROTOCOL  = 5,       /* traffic before the handshake */
    PSP_MMO_DISC_SHUTDOWN  = 6,
    PSP_MMO_DISC_BANNED    = 7,
};

enum { PSP_MMO_HDR_LEN = 8 };            /* magic u32, type u16, version u16 */
enum { PSP_MMO_NAME_LEN = 32, PSP_MMO_VERSION_LEN = 32 };

/* MSG_C2S_HANDSHAKE: hdr, mac[6], name[32] (NUL-padded), client version[32] */
enum { PSP_MMO_HANDSHAKE_LEN = PSP_MMO_HDR_LEN + 6 + PSP_MMO_NAME_LEN + PSP_MMO_VERSION_LEN };
/* MSG_S2C_HANDSHAKE_ACK: hdr, status u32 (0 = accepted), session id u64 */
enum { PSP_MMO_HANDSHAKE_ACK_LEN = PSP_MMO_HDR_LEN + 4 + 8 };

/* MSG_LAN_PROBE: hdr, nonce u32 */
enum { PSP_MMO_LAN_PROBE_LEN = PSP_MMO_HDR_LEN + 4 };
/* MSG_LAN_REPLY: hdr, nonce u32, game port u16, players u16, max players u16,
 * version hash u32, server name[32] */
enum { PSP_MMO_LAN_REPLY_LEN = PSP_MMO_HDR_LEN + 4 + 2 + 2 + 2 + 4 + PSP_MMO_NAME_LEN };

/* FNV-1a 32: the version hash of a version string (server and client must
 * agree on it to play together). */
static inline uint32_t psp_mmo_fnv1a(const char *s) {
    uint32_t h = 0x811C9DC5u;
    while (s && *s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

static inline void psp_mmo_put_u16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static inline void psp_mmo_put_u32(uint8_t *b, uint32_t v) { for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static inline uint16_t psp_mmo_get_u16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static inline uint32_t psp_mmo_get_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

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
