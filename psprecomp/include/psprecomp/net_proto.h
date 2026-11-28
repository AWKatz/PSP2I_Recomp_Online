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
    MSG_S2C_SERVER_TEXT   = 0x8022,   /* the server's Terms of Use / Notices text, in parts */
    MSG_C2S_PARTY_STATE   = 0x0030,   /* my party role (Multi Mode), whenever it changes */
    MSG_C2S_PARTY_LIST_REQ = 0x0031,  /* send me the party directory */
    MSG_S2C_PARTY_LIST    = 0x8030,   /* the party directory: every party a leader is hosting */
    MSG_C2S_PARTY_WARP    = 0x0032,   /* a leader: take my party to this area */
    MSG_S2C_PARTY_WARP    = 0x8032,   /* to the leader and every member: move to this area */
    /* Internet Multi Mode room matching (the game's sceNpMatching2, served by us) */
    MSG_C2S_NP2_SEARCH    = 0x0040,   /* list the rooms */
    MSG_C2S_NP2_CREATE    = 0x0041,   /* create a room and join it */
    MSG_C2S_NP2_JOIN      = 0x0042,   /* join a room */
    MSG_C2S_NP2_LEAVE     = 0x0043,   /* leave my room */
    MSG_C2S_NP2_ROOM_MSG  = 0x0044,   /* a room message to the other members (or one) */
    MSG_C2S_NP2_SET_EXT   = 0x0045,   /* the owner updates the room's external attributes */
    MSG_S2C_NP2_REPLY     = 0x8040,   /* the answer to a request above */
    MSG_S2C_NP2_MEMBER    = 0x8041,   /* someone joined or left my room */
    MSG_S2C_NP2_ROOM_MSG  = 0x8042,   /* a room message from a member */
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

/* ---- room matching (Internet Multi Mode) -----------------------------------------------
 * Rooms hold up to 12 players, as on the original servers. A room's attributes (the
 * game's searchable ints, searchable and external binary attributes) are an opaque
 * blob to the server: ATTRS = int count u8, {id u16, value u32} x count, bin count u8,
 * {id u16, kind u8 (1 searchable, 2 external), size u16, data} x count.
 *
 * MSG_C2S_NP2_SEARCH: hdr, request u32
 * MSG_C2S_NP2_CREATE: hdr, request u32, max members u16, reserved u16, flags u32, attrs size u16, ATTRS
 * MSG_C2S_NP2_JOIN:   hdr, request u32, room u64
 * MSG_C2S_NP2_LEAVE:  hdr, request u32, room u64
 * MSG_C2S_NP2_ROOM_MSG: hdr, room u64, to member u16 (0 = everyone else), size u16, data
 * MSG_C2S_NP2_SET_EXT:  hdr, room u64, attrs size u16, ATTRS (merged into the room's by kind and id;
 *                       searches and GetRoomDataExternalList then show them)
 * MSG_S2C_NP2_REPLY:  hdr, request u32, status u32 (0 = ok, else PSP_MMO_NP2_ERR_*), then
 *     search:       count u16, {ROOM} x count
 *     create, join: ROOM, my member id u16, then {MEMBER} x the room's member count
 *     leave:        nothing
 * ROOM:   room u64, max members u16, members u16, owner member id u16, owner name[20], flags u32,
 *         attrs size u16, ATTRS
 * MEMBER: member id u16, name[20] (NUL-padded), address u32 (IPv4, network order; 0 = unknown)
 * MSG_S2C_NP2_MEMBER:   hdr, room u64, joined u8 (0 = left), owner member id u16, MEMBER
 * MSG_S2C_NP2_ROOM_MSG: hdr, room u64, from member u16, size u16, data */
enum {
    PSP_MMO_NP2_ROOM_MAX = 12, PSP_MMO_NP2_NAME_LEN = 20, PSP_MMO_NP2_ATTRS_MAX = 1024,
    PSP_MMO_NP2_MEMBER_LEN = 2 + PSP_MMO_NP2_NAME_LEN + 4,
    PSP_MMO_NP2_ROOM_HDR_LEN = 8 + 2 + 2 + 2 + PSP_MMO_NP2_NAME_LEN + 4 + 2,   /* ROOM before its ATTRS */
    PSP_MMO_NP2_MSG_MAX = 1024,
};
enum { PSP_MMO_NP2_OK = 0, PSP_MMO_NP2_ERR_NO_ROOM = 1, PSP_MMO_NP2_ERR_FULL = 2, PSP_MMO_NP2_ERR_BAD = 3,
       PSP_MMO_NP2_ERR_SERVER_FULL = 4 };
static inline void psp_mmo_put_u64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }
static inline uint64_t psp_mmo_get_u64(const uint8_t *b) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | b[i]; return v; }

/* MSG_S2C_SYSTEM_NOTICE: hdr, colour u32 (0xRRGGBB), text[128] (UTF-8, NUL-padded) */
enum { PSP_MMO_NOTICE_TEXT_LEN = 128, PSP_MMO_NOTICE_LEN = 8 + 4 + PSP_MMO_NOTICE_TEXT_LEN };
/* MSG_S2C_SERVER_EVENTS: hdr, active event bits u32 (bit i = the server's event i) */
/* MSG_S2C_SERVER_TEXT: hdr, kind u8 (PSP_MMO_TEXT_*), part u8 (from 0), parts u8, reserved u8,
 * size u16, UTF-8 text (at most PSP_MMO_TEXT_CHUNK bytes). Sent after a successful handshake:
 * the parts of one text, in order, joined, are the whole text (at most PSP_MMO_TEXT_MAX bytes;
 * 0 parts = the server has none, and the client shows its own). */
enum { PSP_MMO_TEXT_TERMS = 1, PSP_MMO_TEXT_NOTICES = 2,
       PSP_MMO_TEXT_HDR_LEN = 8 + 6, PSP_MMO_TEXT_CHUNK = 1800, PSP_MMO_TEXT_MAX = 16384 };
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

/* MSG_C2S_HANDSHAKE: hdr, mac[6], name[32] (NUL-padded), client version[32], then (newer
 * clients) the client's own IPv4 address on its network, 4 bytes in network order (0 = unknown).
 * The server gives that address to players who reach it from the same outside address, so
 * players behind the same router (the server elsewhere) can reach each other directly.
 * A server accepts either length. */
enum { PSP_MMO_HANDSHAKE_LEN = PSP_MMO_HDR_LEN + 6 + PSP_MMO_NAME_LEN + PSP_MMO_VERSION_LEN };
enum { PSP_MMO_HANDSHAKE_LAN_LEN = PSP_MMO_HANDSHAKE_LEN + 4 };
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
