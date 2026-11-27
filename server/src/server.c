/* The PSP2i dedicated server core (see server.h). */
#include "server.h"

#include "clock.h"
#include "crypto.h"
#include "db.h"

#include <psprecomp/net_enet.h>
#include <psprecomp/net_lan.h>
#include <psprecomp/net_proto.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define MAX_SESSIONS 64

typedef enum { SS_FREE, SS_PENDING, SS_ACTIVE } ss_state;

typedef struct {
    ss_state state;
    uint64_t id;                      /* the transport's peer id */
    uint8_t  mac[6];
    char     name[PSP_MMO_NAME_LEN + 1];
    uint64_t joined_ms;
    /* rate limit: token bucket, refilled in srv_tick */
    double   tokens;
    uint32_t dropped_this_second;
    int      bad_seconds;             /* consecutive seconds with drops */
    uint32_t kick;                    /* PSP_MMO_DISC_* to apply after this poll (0 = none) */
    double   kill_tokens;             /* enemy kill reports: token bucket */
    /* party directory (MSG_C2S_PARTY_STATE) */
    uint8_t  party_role, party_members, party_max, party_open;
    uint8_t  party_leader[6];
    /* Internet Multi Mode room (-1 = none) and our member id in it */
    int      room;
    uint16_t member_id;
    /* The room messages it broadcast in this room, the latest of each category (the first u32
     * of the message): replayed to a player who joins later (room_replay). */
    struct { uint32_t category; uint16_t size; uint8_t data[PSP_MMO_NP2_MSG_MAX]; } said[8];
    int      nsaid;
    int      replay_due;   /* joined: the others' messages go to it once it speaks itself */
} session;

/* An Internet Multi Mode room: a 12-player lobby room or a party room. */
#define MAX_ROOMS 32
typedef struct {
    int      used;
    uint64_t id;
    uint16_t max, owner, next_member;
    uint32_t flags;
    uint16_t attrs_len;
    uint8_t  attrs[PSP_MMO_NP2_ATTRS_MAX];
} room;

struct srv {
    srv_config cfg;
    char       name[PSP_MMO_NAME_LEN + 1];
    psp_net_enet *enet;
    psp_mmo_lan_responder *lan;
    session    sess[MAX_SESSIONS];
    srv_stats  st;
    uint64_t   now_ms, last_refill_ms, second_start_ms;
    int        started;
    /* loot */
    loot_table *loot;
    loot_rates  rates;
    loot_rng    rng;
    uint64_t    drops_sig, rates_sig;  /* content signatures of the config files (0 = missing) */
    uint64_t    next_drop_id;
    room        rooms[MAX_ROOMS];
    uint64_t    next_room_id;
    uint32_t    events;
};

static void slog(srv *s, const char *fmt, ...) {
    if (!s->cfg.log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->cfg.log(line);
}

uint32_t srv_version_hash(void) { return psp_mmo_fnv1a(PSP_MMO_VERSION_STRING); }

static session *find(srv *s, uint64_t id) {
    for (int i = 0; i < MAX_SESSIONS; i++) if (s->sess[i].state != SS_FREE && s->sess[i].id == id) return &s->sess[i];
    return NULL;
}

static int count(const srv *s, ss_state st) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) n += s->sess[i].state == st;
    return n;
}

/* ---- handshake ---------------------------------------------------------------- */

/* Printable UTF-8, no control characters. */
static int name_ok(const uint8_t *p, size_t n) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n;) {
        const uint8_t c = p[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (c < 0x80) { i++; continue; }
        const int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + (size_t)len > n || (len == 2 && c < 0xC2)) return 0;
        for (int k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)len;
    }
    return 1;
}

uint32_t srv_check_handshake(const uint8_t *msg, uint32_t len, char *name, uint8_t mac[6]) {
    if (len != PSP_MMO_HANDSHAKE_LEN || psp_mmo_type(msg, len) != MSG_C2S_HANDSHAKE) return PSP_MMO_ERR_VERSION;
    const uint8_t *m = msg + PSP_MMO_HDR_LEN;
    const uint8_t *nm = m + 6, *ver = nm + PSP_MMO_NAME_LEN;
    const uint8_t *nul = memchr(nm, 0, PSP_MMO_NAME_LEN);
    if (!nul || !name_ok(nm, (size_t)(nul - nm))) return PSP_MMO_ERR_NAME;
    if (!memchr(ver, 0, PSP_MMO_VERSION_LEN)) return PSP_MMO_ERR_VERSION;
    static const uint8_t zero[6] = { 0 };
    if (!memcmp(m, zero, 6) || (m[0] & 1)) return PSP_MMO_ERR_MAC;     /* zero, or broadcast/multicast */
    memcpy(mac, m, 6);
    memcpy(name, nm, (size_t)(nul - nm));
    name[nul - nm] = 0;
    return PSP_MMO_OK;
}

static void send_ack(srv *s, uint64_t id, uint32_t status) {
    uint8_t ack[PSP_MMO_HANDSHAKE_ACK_LEN];
    psp_mmo_put_hdr(ack, MSG_S2C_HANDSHAKE_ACK);
    psp_mmo_put_u32(ack + PSP_MMO_HDR_LEN, status);
    for (int i = 0; i < 8; i++) ack[PSP_MMO_HDR_LEN + 4 + i] = (uint8_t)((status ? 0 : id) >> (8 * i));
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, ack, sizeof ack);
}

static void send_events(srv *s, uint64_t id) {
    uint8_t m[PSP_MMO_EVENTS_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_SERVER_EVENTS);
    psp_mmo_put_u32(m + 8, s->events);
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m);
}

static void kick(srv *s, session *ss, uint32_t reason);

static void on_handshake(srv *s, session *ss, const psp_net_packet *p) {
    char name[PSP_MMO_NAME_LEN + 1];
    uint8_t mac[6];
    uint32_t st = srv_check_handshake(p->data, p->len, name, mac);
    char addr[64] = "?", ip[64] = "", why[128] = "";
    psp_net_enet_peer_address(s->enet, ss->id, addr, sizeof addr);
    srv_find_ip(s, ss->id, ip, sizeof ip);
    if (st == PSP_MMO_OK) {
        /* The same MAC already has a session. From the same address it is this
         * player coming back after a drop the server has not noticed yet (ENet
         * takes up to ~30 s to time a dead peer out): the old session goes and
         * the new one takes its place. From another address it is refused. */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session *old = &s->sess[i];
            if (old == ss || old->state != SS_ACTIVE || memcmp(old->mac, mac, 6)) continue;
            char oip[64] = "";
            if (srv_find_ip(s, old->id, oip, sizeof oip) == 0 && !strcmp(oip, ip)) {
                slog(s, "player '%s' reconnected from %s: replacing stale session %llu", name, addr,
                     (unsigned long long)old->id);
                kick(s, old, PSP_MMO_DISC_KICKED);
            } else {
                st = PSP_MMO_ERR_DUPLICATE;
            }
            break;
        }
    }
    if (st == PSP_MMO_OK && count(s, SS_ACTIVE) >= s->cfg.max_players) st = PSP_MMO_ERR_FULL;
    if (st == PSP_MMO_OK && s->cfg.database && db_ip_banned(s->cfg.database, ip, why, sizeof why)) {
        send_ack(s, ss->id, PSP_MMO_ERR_BANNED);
        ss->kick = PSP_MMO_DISC_BANNED;
        s->st.handshakes_rejected++;
        slog(s, "handshake from peer %llu (%s) refused: banned (%s)", (unsigned long long)ss->id, addr, why);
        return;
    }
    if (st != PSP_MMO_OK) {
        send_ack(s, ss->id, st);
        ss->kick = PSP_MMO_DISC_HANDSHAKE;
        s->st.handshakes_rejected++;
        slog(s, "handshake from peer %llu (%s) rejected: status %u", (unsigned long long)ss->id, addr, (unsigned)st);
        return;
    }
    memcpy(ss->mac, mac, 6);
    memcpy(ss->name, name, sizeof name);
    ss->state = SS_ACTIVE;
    ss->joined_ms = s->now_ms;
    psp_net_enet_authorize(s->enet, ss->id, 1);
    send_ack(s, ss->id, PSP_MMO_OK);
    send_events(s, ss->id);
    s->st.handshakes_ok++;
    slog(s, "player '%s' joined: session %llu, mac %02X:%02X:%02X:%02X:%02X:%02X, %s (%d/%d)", name,
         (unsigned long long)ss->id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], addr, count(s, SS_ACTIVE), s->cfg.max_players);
}

/* ---- transport hooks -------------------------------------------------------------- */

/* ---- Internet Multi Mode rooms (net_proto.h: MSG_*_NP2_*) ----------------------------------- */

static void send_to(srv *s, uint64_t id, const uint8_t *m, uint32_t n) {
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, n);
}

static int room_members(const srv *s, int r) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) n += s->sess[i].state == SS_ACTIVE && s->sess[i].room == r;
    return n;
}

/* The member's IPv4 address in network order, as the transport sees it (0 = unknown). */
/* A session's IPv4 address as the server sees it (network order), 0 if unknown. */
static uint32_t peer_addr(srv *s, const session *ss) {
    char a[64];
    if (!ss || psp_net_enet_peer_address(s->enet, ss->id, a, sizeof a) != 0) return 0;
    unsigned b[4];
    if (sscanf(a, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0;
    return (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24);
}

static int is_loopback(uint32_t a) { return (a & 0xFF) == 127; }

/* This machine's address on the route toward `to` (network order): a UDP
 * socket "connected" there (no packet is sent) is given the outgoing
 * interface's address. 0 if there is none. */
static uint32_t local_addr_toward(uint32_t to) {
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in d;
    memset(&d, 0, sizeof d);
    d.sin_family = AF_INET;
    d.sin_port = htons(3658);
    d.sin_addr.s_addr = to;
    uint32_t r = 0;
    struct sockaddr_in l;
    socklen_t ll = sizeof l;
    if (connect(fd, (struct sockaddr *)&d, sizeof d) == 0 && getsockname(fd, (struct sockaddr *)&l, &ll) == 0)
        r = l.sin_addr.s_addr;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return r;
}

/* The address member `ss` is reached at, as told to `to`: the games talk to
 * each other directly (UDP 3658). A player on the server's own machine joins
 * through 127.0.0.1, which a player elsewhere must not be given -- it would
 * send to itself -- so it gets this machine's address on its route instead. */
static uint32_t member_addr(srv *s, const session *ss, const session *to) {
    const uint32_t a = peer_addr(s, ss);
    if (!is_loopback(a)) return a;
    const uint32_t t = peer_addr(s, to);
    if (!t || is_loopback(t)) return a;
    const uint32_t l = local_addr_toward(t);
    return l ? l : a;
}

static uint32_t put_member(srv *s, uint8_t *m, const session *ss, const session *to) {
    psp_mmo_put_u16(m, ss->member_id);
    memset(m + 2, 0, PSP_MMO_NP2_NAME_LEN);
    memcpy(m + 2, ss->name, strnlen(ss->name, PSP_MMO_NP2_NAME_LEN - 1));
    psp_mmo_put_u32(m + 2 + PSP_MMO_NP2_NAME_LEN, member_addr(s, ss, to));
    return PSP_MMO_NP2_MEMBER_LEN;
}

static uint32_t put_room(const srv *s, uint8_t *m, int r) {
    const room *rm = &s->rooms[r];
    psp_mmo_put_u64(m, rm->id);
    psp_mmo_put_u16(m + 8, rm->max);
    psp_mmo_put_u16(m + 10, (uint16_t)room_members(s, r));
    psp_mmo_put_u16(m + 12, rm->owner);
    memset(m + 14, 0, PSP_MMO_NP2_NAME_LEN);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (s->sess[i].state == SS_ACTIVE && s->sess[i].room == r && s->sess[i].member_id == rm->owner)
            memcpy(m + 14, s->sess[i].name, strnlen(s->sess[i].name, PSP_MMO_NP2_NAME_LEN - 1));
    psp_mmo_put_u32(m + 14 + PSP_MMO_NP2_NAME_LEN, rm->flags);
    psp_mmo_put_u16(m + 18 + PSP_MMO_NP2_NAME_LEN, rm->attrs_len);
    memcpy(m + PSP_MMO_NP2_ROOM_HDR_LEN, rm->attrs, rm->attrs_len);
    return PSP_MMO_NP2_ROOM_HDR_LEN + rm->attrs_len;
}

static void np2_status(srv *s, session *ss, uint32_t req, uint32_t status) {
    uint8_t m[16];
    psp_mmo_put_hdr(m, MSG_S2C_NP2_REPLY);
    psp_mmo_put_u32(m + 8, req); psp_mmo_put_u32(m + 12, status);
    send_to(s, ss->id, m, sizeof m);
}

/* To every member of room r but `except`: who joined or left. */
static void room_tell(srv *s, int r, const session *who, int joined, const session *except) {
    uint8_t m[8 + 8 + 1 + 2 + PSP_MMO_NP2_MEMBER_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_NP2_MEMBER);
    psp_mmo_put_u64(m + 8, s->rooms[r].id);
    m[16] = (uint8_t)joined;
    psp_mmo_put_u16(m + 17, s->rooms[r].owner);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session *o = &s->sess[i];
        if (o->state != SS_ACTIVE || o->room != r || o == except) continue;
        put_member(s, m + 19, who, o);                 /* the address depends on who is told */
        send_to(s, o->id, m, sizeof m);
    }
}

/* The create / join reply: the room, my member id, every member. */
static void room_joined_reply(srv *s, session *ss, uint32_t req) {
    static uint8_t m[8 + 8 + PSP_MMO_NP2_ROOM_HDR_LEN + PSP_MMO_NP2_ATTRS_MAX + 2 + PSP_MMO_NP2_ROOM_MAX * PSP_MMO_NP2_MEMBER_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_NP2_REPLY);
    psp_mmo_put_u32(m + 8, req); psp_mmo_put_u32(m + 12, PSP_MMO_NP2_OK);
    uint32_t n = 16 + put_room(s, m + 16, ss->room);
    psp_mmo_put_u16(m + n, ss->member_id); n += 2;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session *o = &s->sess[i];
        if (o->state == SS_ACTIVE && o->room == ss->room) n += put_member(s, m + n, o, ss);
    }
    send_to(s, ss->id, m, n);
}

static void room_leave(srv *s, session *ss) {
    const int r = ss->room;
    if (r < 0 || r >= MAX_ROOMS || !s->rooms[r].used) { ss->room = -1; return; }
    ss->room = -1;
    room *rm = &s->rooms[r];
    if (room_members(s, r) == 0) {
        slog(s, "rooms: room %llu closed (last member '%s' left)", (unsigned long long)rm->id, ss->name);
        rm->used = 0;
        return;
    }
    if (rm->owner == ss->member_id) {                 /* the next member takes over */
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (s->sess[i].state == SS_ACTIVE && s->sess[i].room == r) { rm->owner = s->sess[i].member_id; break; }
    }
    room_tell(s, r, ss, 0, NULL);
    slog(s, "rooms: '%s' left room %llu (%d left)", ss->name, (unsigned long long)rm->id, room_members(s, r));
}

static void room_enter(srv *s, session *ss, int r) {
    room_leave(s, ss);
    ss->nsaid = 0;
    ss->replay_due = 0;
    ss->room = r;
    ss->member_id = ++s->rooms[r].next_member;
}

static void on_np2_search(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 12) return;
    static uint8_t m[8 + 8 + 2 + MAX_ROOMS * (PSP_MMO_NP2_ROOM_HDR_LEN + PSP_MMO_NP2_ATTRS_MAX)];
    psp_mmo_put_hdr(m, MSG_S2C_NP2_REPLY);
    psp_mmo_put_u32(m + 8, psp_mmo_get_u32(p->data + 8)); psp_mmo_put_u32(m + 12, PSP_MMO_NP2_OK);
    uint32_t n = 18;
    uint16_t c = 0;
    for (int r = 0; r < MAX_ROOMS; r++) {
        if (!s->rooms[r].used || n + PSP_MMO_NP2_ROOM_HDR_LEN + s->rooms[r].attrs_len > 1100) continue;   /* one datagram */
        n += put_room(s, m + n, r);
        c++;
    }
    psp_mmo_put_u16(m + 16, c);
    send_to(s, ss->id, m, n);
}

static void on_np2_create(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 22) return;
    const uint32_t req = psp_mmo_get_u32(p->data + 8);
    const uint16_t attrs_len = psp_mmo_get_u16(p->data + 20);
    uint16_t max = psp_mmo_get_u16(p->data + 12);
    if (attrs_len > PSP_MMO_NP2_ATTRS_MAX || 22u + attrs_len > p->len || max < 1) { np2_status(s, ss, req, PSP_MMO_NP2_ERR_BAD); return; }
    if (max > PSP_MMO_NP2_ROOM_MAX) max = PSP_MMO_NP2_ROOM_MAX;   /* the original servers' limit */
    int r = -1;
    for (int i = 0; i < MAX_ROOMS && r < 0; i++) if (!s->rooms[i].used) r = i;
    if (r < 0) { np2_status(s, ss, req, PSP_MMO_NP2_ERR_SERVER_FULL); return; }
    room *rm = &s->rooms[r];
    memset(rm, 0, sizeof *rm);
    rm->used = 1;
    rm->id = ++s->next_room_id;
    rm->max = max;
    rm->flags = psp_mmo_get_u32(p->data + 16);
    rm->attrs_len = attrs_len;
    memcpy(rm->attrs, p->data + 22, attrs_len);
    room_enter(s, ss, r);
    rm->owner = ss->member_id;
    slog(s, "rooms: '%s' created room %llu (%u players max)", ss->name, (unsigned long long)rm->id, (unsigned)max);
    room_joined_reply(s, ss, req);
}

/* A player who joins hears what the others already said to the room. The games broadcast their
 * own character data (a 228-byte room message) once, when they enter -- the room's owner when it
 * creates it, to nobody -- and on a PSP's servers a late joiner still got it: without it the
 * joiner had the owner's movement packets but no character to move, and never saw the owner. */
static void room_replay(srv *s, session *ss) {
    static uint8_t m[20 + PSP_MMO_NP2_MSG_MAX];
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session *o = &s->sess[i];
        if (o->state != SS_ACTIVE || o->room != ss->room || o == ss) continue;
        for (int k = 0; k < o->nsaid; k++) {
            psp_mmo_put_hdr(m, MSG_S2C_NP2_ROOM_MSG);
            psp_mmo_put_u64(m + 8, s->rooms[ss->room].id);
            psp_mmo_put_u16(m + 16, o->member_id);
            psp_mmo_put_u16(m + 18, o->said[k].size);
            memcpy(m + 20, o->said[k].data, o->said[k].size);
            send_to(s, ss->id, m, 20u + o->said[k].size);
            n++;
        }
    }
    if (n) slog(s, "rooms: replayed %d earlier room message(s) to '%s'", n, ss->name);
}

static void on_np2_join(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 20) return;
    const uint32_t req = psp_mmo_get_u32(p->data + 8);
    const uint64_t id = psp_mmo_get_u64(p->data + 12);
    int r = -1;
    for (int i = 0; i < MAX_ROOMS && r < 0; i++) if (s->rooms[i].used && s->rooms[i].id == id) r = i;
    if (r < 0) { np2_status(s, ss, req, PSP_MMO_NP2_ERR_NO_ROOM); return; }
    if (ss->room != r && room_members(s, r) >= s->rooms[r].max) { np2_status(s, ss, req, PSP_MMO_NP2_ERR_FULL); return; }
    if (ss->room != r) room_enter(s, ss, r);
    slog(s, "rooms: '%s' joined room %llu (%d/%u)", ss->name, (unsigned long long)id, room_members(s, r), (unsigned)s->rooms[r].max);
    room_joined_reply(s, ss, req);
    room_tell(s, r, ss, 1, ss);
    ss->replay_due = 1;                               /* room_replay: after the owner's welcome */
}

static void on_np2_leave(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 20) return;
    room_leave(s, ss);
    np2_status(s, ss, psp_mmo_get_u32(p->data + 8), PSP_MMO_NP2_OK);
}

/* ATTRS (net_proto.h) parsed: ints {id, value}, bins {id, kind, size, data}. */
typedef struct { uint16_t id; uint32_t v; } attr_int;
typedef struct { uint16_t id; uint8_t kind; uint16_t size; const uint8_t *data; } attr_bin;
static int attrs_parse(const uint8_t *a, uint32_t len, attr_int *ints, int *ni, attr_bin *bins, int *nb, int cap) {
    uint32_t p = 0;
    *ni = *nb = 0;
    if (len == 0) return 0;
    const int ci = a[p++];
    for (int i = 0; i < ci; i++) {
        if (p + 6 > len || *ni >= cap) return -1;
        ints[*ni].id = psp_mmo_get_u16(a + p); ints[*ni].v = psp_mmo_get_u32(a + p + 2); (*ni)++; p += 6;
    }
    if (p >= len) return 0;
    const int cb = a[p++];
    for (int i = 0; i < cb; i++) {
        if (p + 5 > len || *nb >= cap) return -1;
        attr_bin *b = &bins[(*nb)++];
        b->id = psp_mmo_get_u16(a + p); b->kind = a[p + 2]; b->size = psp_mmo_get_u16(a + p + 3);
        if (p + 5u + b->size > len) return -1;
        b->data = a + p + 5;
        p += 5u + b->size;
    }
    return 0;
}

/* The owner updates the room's external attributes (sceNpMatching2SetRoomDataExternal): merged
 * into the room's by kind and id, so a joiner's GetRoomDataExternalList (answered from the
 * search list) sees what the owner published -- its game reads the owner from there. */
static void on_np2_set_ext(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 18 || ss->room < 0) return;
    room *rm = &s->rooms[ss->room];
    if (psp_mmo_get_u64(p->data + 8) != rm->id || rm->owner != ss->member_id) return;
    const uint16_t alen = psp_mmo_get_u16(p->data + 16);
    if (alen > PSP_MMO_NP2_ATTRS_MAX || 18u + alen > p->len) return;
    enum { CAP = 32 };
    attr_int oi[CAP], ni_[CAP];
    attr_bin ob[CAP], nb_[CAP];
    int noi, nob, nni, nnb;
    uint8_t old[PSP_MMO_NP2_ATTRS_MAX];
    memcpy(old, rm->attrs, rm->attrs_len);
    if (attrs_parse(old, rm->attrs_len, oi, &noi, ob, &nob, CAP) || attrs_parse(p->data + 18, alen, ni_, &nni, nb_, &nnb, CAP)) return;
    for (int i = 0; i < nni; i++) {                    /* ints: replace by id, else add */
        int k = 0;
        while (k < noi && oi[k].id != ni_[i].id) k++;
        if (k == noi) { if (noi == CAP) continue; noi++; }
        oi[k] = ni_[i];
    }
    for (int i = 0; i < nnb; i++) {                    /* bins: replace by kind and id, else add */
        int k = 0;
        while (k < nob && !(ob[k].id == nb_[i].id && ob[k].kind == nb_[i].kind)) k++;
        if (k == nob) { if (nob == CAP) continue; nob++; }
        ob[k] = nb_[i];
    }
    uint8_t out[PSP_MMO_NP2_ATTRS_MAX];
    uint32_t n = 0;
    out[n++] = (uint8_t)noi;
    for (int i = 0; i < noi; i++) { psp_mmo_put_u16(out + n, oi[i].id); psp_mmo_put_u32(out + n + 2, oi[i].v); n += 6; }
    out[n++] = (uint8_t)nob;
    for (int i = 0; i < nob; i++) {
        if (n + 5u + ob[i].size > sizeof out) { out[1 + 6 * noi] = (uint8_t)i; break; }
        psp_mmo_put_u16(out + n, ob[i].id); out[n + 2] = ob[i].kind; psp_mmo_put_u16(out + n + 3, ob[i].size);
        memcpy(out + n + 5, ob[i].data, ob[i].size);
        n += 5u + ob[i].size;
    }
    memcpy(rm->attrs, out, n);
    rm->attrs_len = (uint16_t)n;
}

/* A room message: relayed to every other member, or to one. */
static void on_np2_room_msg(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len < 20 || ss->room < 0) return;
    const uint16_t to = psp_mmo_get_u16(p->data + 16), size = psp_mmo_get_u16(p->data + 18);
    if (size > PSP_MMO_NP2_MSG_MAX || 20u + size > p->len) return;
    static uint8_t m[20 + PSP_MMO_NP2_MSG_MAX];
    psp_mmo_put_hdr(m, MSG_S2C_NP2_ROOM_MSG);
    psp_mmo_put_u64(m + 8, s->rooms[ss->room].id);
    psp_mmo_put_u16(m + 16, ss->member_id);
    psp_mmo_put_u16(m + 18, size);
    memcpy(m + 20, p->data + 20, size);
    /* A joiner's first broadcast comes once its game has taken the owner's welcome (room messages
     * of categories 1, 3, 6): now it can take the others' character data. Sent before that,
     * straight after the join, the game dropped it. */
    if (ss->replay_due) { ss->replay_due = 0; room_replay(s, ss); }
    if (!to && size >= 4) {                           /* a broadcast: kept for players who join later */
        const uint32_t cat = psp_mmo_get_u32(p->data + 20);
        int k = 0;
        while (k < ss->nsaid && ss->said[k].category != cat) k++;
        if (k == ss->nsaid && k < 8) ss->nsaid++;
        if (k < 8) { ss->said[k].category = cat; ss->said[k].size = size; memcpy(ss->said[k].data, p->data + 20, size); }
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session *o = &s->sess[i];
        if (o->state != SS_ACTIVE || o->room != ss->room || o == ss || (to && o->member_id != to)) continue;
        send_to(s, o->id, m, 20u + size);
    }
}

static void on_peer(void *u, uint64_t id, const uint8_t mac[6], int joined) {
    srv *s = u;
    (void)mac;
    if (joined) {
        session *ss = NULL;
        for (int i = 0; i < MAX_SESSIONS && !ss; i++) if (s->sess[i].state == SS_FREE) ss = &s->sess[i];
        if (!ss) return;                              /* cannot happen: the transport caps peers */
        memset(ss, 0, sizeof *ss);
        ss->state = SS_PENDING;
        ss->id = id;
        ss->joined_ms = s->now_ms;
        ss->tokens = 2.0 * s->cfg.rate_pps;
        ss->kill_tokens = 2.0 * s->cfg.kills_per_sec;
        ss->room = -1;
        return;
    }
    session *ss = find(s, id);
    if (!ss) return;
    if (ss->state == SS_ACTIVE) slog(s, "player '%s' left (session %llu)", ss->name, (unsigned long long)id);
    room_leave(s, ss);
    memset(ss, 0, sizeof *ss);
}

static int admit(void *u, uint64_t id, uint32_t len) {
    srv *s = u;
    (void)len;
    s->st.packets_in++;
    session *ss = find(s, id);
    if (!ss || ss->kick) { s->st.packets_dropped++; return 0; }
    if (s->cfg.mode != SRV_MODE_ONLINE) return 1;
    if (ss->tokens >= 1.0) { ss->tokens -= 1.0; return 1; }
    ss->dropped_this_second++;
    s->st.packets_dropped++;
    return 0;
}

/* ---- loot ---------------------------------------------------------------------------- */

static uint64_t file_sig(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 0xCBF29CE484222325ull, n = 0;   /* FNV-1a 64 of the contents, and the length */
    int c;
    while ((c = fgetc(f)) != EOF) { h = (h ^ (uint8_t)c) * 0x100000001B3ull; n++; }
    fclose(f);
    return (h ^ n) | 1;
}

enum { RELOAD_DROPS = 1, RELOAD_RATES = 2 };
static void check_configs(srv *s, int force) {
    char err[320];
    const uint64_t ds = file_sig(s->cfg.drop_tables_path);
    if (s->cfg.drop_tables_path && ((force & RELOAD_DROPS) || ds != s->drops_sig)) {
        s->drops_sig = ds;
        loot_table *t = ds ? loot_load(s->cfg.drop_tables_path, err, sizeof err) : NULL;
        if (t) {
            loot_free(s->loot);                       /* single-threaded: swap between ticks */
            s->loot = t;
            s->st.reloads++;
            slog(s, "drop tables: %s: %d enemies, %d missions (version %d)", s->cfg.drop_tables_path,
                 t->n_enemies, t->n_missions, t->version);
        } else {
            s->st.reload_errors++;
            slog(s, "drop tables: %s; %s", ds ? err : "file missing",
                 s->loot ? "keeping the tables loaded before" : "no drops until it loads");
        }
    }
    const uint64_t rs = file_sig(s->cfg.rates_path);
    if (s->cfg.rates_path && ((force & RELOAD_RATES) || rs != s->rates_sig)) {
        s->rates_sig = rs;
        loot_rates r;
        const int rc = rs ? loot_rates_load(s->cfg.rates_path, &r, err, sizeof err) : -2;
        if (rc == 0) {
            s->rates = r;
            s->st.reloads++;
            slog(s, "rates: exp %.2f, drop %.2f, rare %.2f, meseta %.2f, type points %.2f", r.exp, r.drop, r.rare, r.meseta, r.type_points);
        } else if (rc == -2) {
            slog(s, "rates: %s missing; keeping %s", s->cfg.rates_path, s->st.reloads ? "the current rates" : "1.0 for all");
        } else {
            s->st.reload_errors++;
            slog(s, "rates: %s; keeping the current rates", err);
        }
    }
}

static void put_f32(uint8_t *b, const uint8_t *src) { memcpy(b, src, 4); }   /* floats pass through as sent */

static void on_kill(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len != PSP_MMO_ENEMY_KILLED_LEN) return;
    if (ss->kill_tokens < 1.0) { s->st.kills_dropped++; return; }
    ss->kill_tokens -= 1.0;
    const uint8_t *m = p->data + PSP_MMO_HDR_LEN;
    const uint32_t enemy = psp_mmo_get_u32(m);
    loot_result r;
    if (!s->loot || loot_roll_enemy(s->loot, enemy, &s->rates, &s->rng, &r) != 0) {
        s->st.kills_dropped++;
        return;
    }
    s->st.kills++;
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    uint8_t rw[PSP_MMO_KILL_REWARD_LEN];
    psp_mmo_put_hdr(rw, MSG_S2C_KILL_REWARD);
    psp_mmo_put_u32(rw + 8, enemy);
    psp_mmo_put_u32(rw + 12, r.exp);
    psp_mmo_put_u32(rw + 16, r.type_points);
    const psp_net_peer killer = { { 0 }, ss->id };
    t->send_packet(t->ctx, &killer, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, rw, sizeof rw);
    if (!r.has_drop) return;

    /* the drop: recorded (claims come later), then shown to every player */
    int64_t drop_id = 0;
    if (s->cfg.database) {
        char item[160];
        if (r.item_id) snprintf(item, sizeof item, "{\"id\":%u,\"rare\":%s}", (unsigned)r.item_id, r.rare ? "true" : "false");
        else snprintf(item, sizeof item, "{\"meseta\":%u}", (unsigned)r.meseta);
        char origin[64];
        snprintf(origin, sizeof origin, "enemy:%u", (unsigned)enemy);
        drop_id = db_drop_create(s->cfg.database, item, origin);
    }
    if (drop_id <= 0) drop_id = (int64_t)++s->next_drop_id;
    uint8_t sd[PSP_MMO_SPAWN_DROP_LEN];
    psp_mmo_put_hdr(sd, MSG_S2C_SPAWN_DROP);
    for (int i = 0; i < 8; i++) sd[8 + i] = (uint8_t)((uint64_t)drop_id >> (8 * i));
    psp_mmo_put_u32(sd + 16, enemy);
    psp_mmo_put_u32(sd + 20, r.item_id);
    psp_mmo_put_u32(sd + 24, r.meseta);
    psp_mmo_put_u32(sd + 28, r.rare ? PSP_MMO_DROP_RARE : 0);
    put_f32(sd + 32, m + 8); put_f32(sd + 36, m + 12); put_f32(sd + 40, m + 16);
    for (int i = 0; i < 8; i++) sd[44 + i] = (uint8_t)(ss->id >> (8 * i));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sess[i].state != SS_ACTIVE) continue;
        const psp_net_peer to = { { 0 }, s->sess[i].id };
        t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, sd, sizeof sd);
    }
    s->st.drops++;
    if (r.rare) {
        s->st.rare_drops++;
        slog(s, "rare drop: '%s' (item %u) from enemy %u for '%s'", r.name, (unsigned)r.item_id, (unsigned)enemy, ss->name);
    }
}

void srv_reload(srv *s) { check_configs(s, RELOAD_DROPS | RELOAD_RATES); }
void srv_reload_drops(srv *s) { check_configs(s, RELOAD_DROPS); }
void srv_reload_rates(srv *s) { check_configs(s, RELOAD_RATES); }
loot_rates srv_rates(const srv *s) { return s->rates; }
int srv_set_rate(srv *s, const char *name, double v) {
    if (loot_rates_set(&s->rates, name, v) != 0) return -1;
    slog(s, "rates: %s set to %.2f", name, v);
    return 0;
}
int srv_enemy_count(const srv *s) { return s->loot ? s->loot->n_enemies : 0; }

/* ---- party directory ------------------------------------------------------------------ */

static void on_party_state(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len != PSP_MMO_PARTY_STATE_LEN) return;
    const uint8_t *m = p->data + PSP_MMO_HDR_LEN;
    const uint8_t role = m[0] <= PSP_MMO_PARTY_MEMBER ? m[0] : PSP_MMO_PARTY_NONE;
    if (role != ss->party_role)
        slog(s, "party: '%s' %s", ss->name, role == PSP_MMO_PARTY_LEADER ? "is leading a party"
                                          : role == PSP_MMO_PARTY_MEMBER ? "joined a party" : "left the party");
    ss->party_role = role;
    ss->party_members = m[1];
    ss->party_max = m[2];
    ss->party_open = m[3] ? 1 : 0;
    if (role == PSP_MMO_PARTY_LEADER) memcpy(ss->party_leader, ss->mac, 6);   /* a leader leads itself */
    else memcpy(ss->party_leader, m + 4, 6);
}

int srv_parties(const srv *s, srv_party *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS && n < max; i++) {
        const session *ss = &s->sess[i];
        if (ss->state != SS_ACTIVE || ss->party_role != PSP_MMO_PARTY_LEADER) continue;
        srv_party *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->leader_session = ss->id;
        memcpy(e->leader_mac, ss->mac, 6);
        snprintf(e->leader_name, sizeof e->leader_name, "%s", ss->name);
        e->members = ss->party_members ? ss->party_members : 1;
        e->max = ss->party_max;
        e->open = ss->party_open && (!e->max || e->members < e->max);
    }
    return n;
}

static void on_party_list_req(srv *s, session *ss) {
    srv_party ps[PSP_MMO_PARTY_LIST_MAX];
    const int n = srv_parties(s, ps, PSP_MMO_PARTY_LIST_MAX);
    uint8_t m[PSP_MMO_PARTY_LIST_HDR_LEN + PSP_MMO_PARTY_LIST_MAX * PSP_MMO_PARTY_ENTRY_LEN];
    memset(m, 0, sizeof m);
    psp_mmo_put_hdr(m, MSG_S2C_PARTY_LIST);
    psp_mmo_put_u16(m + 8, (uint16_t)count(s, SS_ACTIVE));
    psp_mmo_put_u16(m + 10, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *e = m + PSP_MMO_PARTY_LIST_HDR_LEN + i * PSP_MMO_PARTY_ENTRY_LEN;
        memcpy(e, ps[i].leader_mac, 6);
        e[6] = (uint8_t)ps[i].members;
        e[7] = (uint8_t)ps[i].max;
        e[8] = (uint8_t)ps[i].open;
        psp_mmo_put_str(e + 10, ps[i].leader_name, PSP_MMO_NAME_LEN);
    }
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, ss->id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m,
                   (uint32_t)(PSP_MMO_PARTY_LIST_HDR_LEN + n * PSP_MMO_PARTY_ENTRY_LEN));
}

/* A party leader's warp: to the leader and its members (as they report it). */
static void on_party_warp(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len != PSP_MMO_PARTY_WARP_LEN || ss->party_role != PSP_MMO_PARTY_LEADER) return;
    uint8_t m[PSP_MMO_PARTY_WARP_LEN];
    memcpy(m, p->data, sizeof m);
    psp_mmo_put_hdr(m, MSG_S2C_PARTY_WARP);
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session *o = &s->sess[i];
        if (o->state != SS_ACTIVE || o->party_role == PSP_MMO_PARTY_NONE || memcmp(o->party_leader, ss->mac, 6)) continue;
        const psp_net_peer to = { { 0 }, o->id };
        t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m);
        n++;
    }
    s->st.party_warps++;
    slog(s, "party: '%s' warps the party (%d players) to destination %d", ss->name, n, (int)psp_mmo_get_u32(m + 8));
}

static void on_packet(const psp_net_packet *p, void *u) {
    srv *s = u;
    session *ss = find(s, p->from.id);
    if (!ss || ss->kick) return;
    const uint16_t type = p->port == PSP_MMO_CTRL_PORT ? psp_mmo_type(p->data, p->len) : 0;
    if (ss->state == SS_PENDING) {
        if (type == MSG_C2S_HANDSHAKE) { on_handshake(s, ss, p); return; }
        if (s->cfg.mode == SRV_MODE_ONLINE) {         /* nothing else before the handshake */
            ss->kick = PSP_MMO_DISC_PROTOCOL;
            s->st.kicked_protocol++;
        }
        return;
    }
    /* Active players: game traffic is relayed by the transport; control
     * messages are handled here. */
    if (type == MSG_C2S_ENEMY_KILLED) on_kill(s, ss, p);
    else if (type == MSG_C2S_PARTY_STATE) on_party_state(s, ss, p);
    else if (type == MSG_C2S_PARTY_LIST_REQ) on_party_list_req(s, ss);
    else if (type == MSG_C2S_PARTY_WARP) on_party_warp(s, ss, p);
    else if (type == MSG_C2S_NP2_SEARCH) on_np2_search(s, ss, p);
    else if (type == MSG_C2S_NP2_CREATE) on_np2_create(s, ss, p);
    else if (type == MSG_C2S_NP2_JOIN) on_np2_join(s, ss, p);
    else if (type == MSG_C2S_NP2_LEAVE) on_np2_leave(s, ss, p);
    else if (type == MSG_C2S_NP2_ROOM_MSG) on_np2_room_msg(s, ss, p);
    else if (type == MSG_C2S_NP2_SET_EXT) on_np2_set_ext(s, ss, p);
}

/* ---- lifecycle ---------------------------------------------------------------------- */

srv *srv_create(const srv_config *cfg) {
    srv *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg;
    if (s->cfg.max_players < 1) s->cfg.max_players = SRV_MAX_PLAYERS;
    if (s->cfg.max_players > SRV_MAX_PLAYERS) s->cfg.max_players = SRV_MAX_PLAYERS;   /* the hub cap */
    if (!s->cfg.rate_pps) s->cfg.rate_pps = 120;
    if (!s->cfg.handshake_timeout_ms) s->cfg.handshake_timeout_ms = 5000;
    snprintf(s->name, sizeof s->name, "%s", s->cfg.name && *s->cfg.name ? s->cfg.name : "PSP2i Server");
    s->cfg.name = s->name;
    if (!s->cfg.kills_per_sec) s->cfg.kills_per_sec = 10;
    const loot_rates def = LOOT_RATES_DEFAULT;
    s->rates = def;
    uint64_t seed = s->cfg.rng_seed;
    if (!seed && os_random(&seed, sizeof seed) != 0) seed = 0x5053503249ull ^ (uint64_t)(uintptr_t)s;
    loot_rng_seed(&s->rng, seed);
    return s;
}

int srv_start(srv *s) {
    psp_net_enet_config ec = {
        .bind_host = s->cfg.bind, .port = s->cfg.port, .log = s->cfg.log,
        .on_peer = on_peer, .admit = admit, .host_user = s, .relay_authorized_only = 1,
    };
    s->enet = psp_net_enet_create(&ec);
    if (!s->enet) return -1;
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    static const uint8_t server_mac[6] = { 0x02, 0x50, 0x32, 0x49, 0x00, 0x01 };   /* locally administered */
    /* ENet admits pending peers too: room for a few beyond max_players, capped by the transport */
    int slots = s->cfg.max_players + 4;
    if (slots > MAX_SESSIONS) slots = MAX_SESSIONS;
    if (t->initialize(t->ctx, server_mac) != PSP_NET_OK || t->host_session(t->ctx, "PSP2I", slots + 1) != PSP_NET_OK) {
        slog(s, "cannot bind the game port %u", (unsigned)s->cfg.port);
        psp_net_enet_destroy(s->enet);
        s->enet = NULL;
        return -1;
    }
    if (s->cfg.mode == SRV_MODE_LAN) {
        s->lan = psp_mmo_lan_responder_open(s->cfg.lan_port);
        if (!s->lan) {
            slog(s, "cannot bind the LAN discovery port %u", (unsigned)s->cfg.lan_port);
            srv_stop(s);
            return -1;
        }
    }
    s->started = 1;
    check_configs(s, RELOAD_DROPS | RELOAD_RATES);
    if (s->lan)
        slog(s, "'%s': LAN mode, game port %u (udp), discovery port %u (udp), up to %d players, version %s (hash %08X)",
             s->name, (unsigned)srv_port(s), (unsigned)srv_lan_port(s), s->cfg.max_players,
             PSP_MMO_VERSION_STRING, (unsigned)srv_version_hash());
    else
        slog(s, "'%s': online mode, game port %u (udp), up to %d players, version %s (hash %08X)",
             s->name, (unsigned)srv_port(s), s->cfg.max_players, PSP_MMO_VERSION_STRING, (unsigned)srv_version_hash());
    if (s->cfg.mode == SRV_MODE_ONLINE)
        slog(s, "online: %u packets/s per player (burst %u), handshake within %u ms",
             (unsigned)s->cfg.rate_pps, (unsigned)(2 * s->cfg.rate_pps), (unsigned)s->cfg.handshake_timeout_ms);
    return 0;
}

static void kick(srv *s, session *ss, uint32_t reason) {
    const uint64_t id = ss->id;
    psp_net_enet_disconnect(s->enet, id, reason);     /* calls on_peer(left): frees the session */
    session *still = find(s, id);
    if (still) memset(still, 0, sizeof *still);
}

void srv_tick(srv *s, uint64_t now_ms) {
    if (!s->started) return;
    s->now_ms = now_ms;
    if (!s->last_refill_ms) s->last_refill_ms = s->second_start_ms = now_ms;

    /* refill the token buckets */
    const double dt = (double)(now_ms - s->last_refill_ms) / 1000.0;
    s->last_refill_ms = now_ms;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        if (ss->state == SS_FREE) continue;
        ss->tokens += dt * s->cfg.rate_pps;
        if (ss->tokens > 2.0 * s->cfg.rate_pps) ss->tokens = 2.0 * s->cfg.rate_pps;
        ss->kill_tokens += dt * s->cfg.kills_per_sec;
        if (ss->kill_tokens > 2.0 * s->cfg.kills_per_sec) ss->kill_tokens = 2.0 * s->cfg.kills_per_sec;
    }

    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    t->poll_packets(t->ctx, on_packet, s);

    /* once a second: rate-limit verdicts */
    const int new_second = now_ms - s->second_start_ms >= 1000;
    if (new_second) { s->second_start_ms = now_ms; check_configs(s, 0); }   /* hot reload */

    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        if (ss->state == SS_FREE) continue;
        if (!ss->kick && ss->state == SS_PENDING && now_ms - ss->joined_ms > s->cfg.handshake_timeout_ms) {
            ss->kick = PSP_MMO_DISC_TIMEOUT;
            s->st.kicked_timeout++;
            slog(s, "peer %llu: no handshake within %u ms", (unsigned long long)ss->id, (unsigned)s->cfg.handshake_timeout_ms);
        }
        if (new_second && s->cfg.mode == SRV_MODE_ONLINE) {
            ss->bad_seconds = ss->dropped_this_second ? ss->bad_seconds + 1 : 0;
            ss->dropped_this_second = 0;
            if (!ss->kick && ss->bad_seconds >= 3) {
                ss->kick = PSP_MMO_DISC_RATE;
                s->st.kicked_rate++;
                slog(s, "peer %llu ('%s'): over %u packets/s for 3 s", (unsigned long long)ss->id, ss->name, (unsigned)s->cfg.rate_pps);
            }
        }
        if (ss->kick) kick(s, ss, ss->kick);
    }

    if (s->lan) {
        const psp_mmo_lan_info info = {
            .game_port = srv_port(s), .players = (uint16_t)count(s, SS_ACTIVE),
            .max_players = (uint16_t)s->cfg.max_players, .version_hash = srv_version_hash(),
        };
        psp_mmo_lan_info i2 = info;
        memcpy(i2.name, s->name, sizeof i2.name);
        s->st.lan_replies += (uint64_t)psp_mmo_lan_responder_poll(s->lan, &i2);
    }
}

void srv_wait(srv *s, uint32_t timeout_ms) {
    if (!s->started || !s->enet) { clk_sleep_until_ns(clk_now_ns() + (uint64_t)timeout_ms * 1000000u); return; }
    psp_net_enet_service(s->enet, timeout_ms, on_packet, s);
}

void srv_stop(srv *s) {
    if (s->enet) {
        psp_net_enet_disconnect_all(s->enet, PSP_MMO_DISC_SHUTDOWN);
        const psp_net_transport *t = psp_net_enet_transport(s->enet);
        for (int i = 0; i < 10; i++) {                /* ~100 ms for the disconnects to go out */
            t->poll_packets(t->ctx, NULL, NULL);
            clk_sleep_until_ns(clk_now_ns() + 10000000u);
        }
        psp_net_enet_destroy(s->enet);
        s->enet = NULL;
    }
    psp_mmo_lan_responder_close(s->lan);
    s->lan = NULL;
    memset(s->sess, 0, sizeof s->sess);
    s->started = 0;
}

void srv_destroy(srv *s) {
    if (!s) return;
    srv_stop(s);
    loot_free(s->loot);
    free(s);
}

void srv_get_stats(const srv *s, srv_stats *out) {
    *out = s->st;
    out->players = count(s, SS_ACTIVE);
    out->pending = count(s, SS_PENDING);
}

uint16_t srv_port(const srv *s) { return s->enet ? psp_net_enet_port(s->enet) : 0; }
uint16_t srv_lan_port(const srv *s) { return s->lan ? psp_mmo_lan_responder_port(s->lan) : 0; }

/* ---- admin ---------------------------------------------------------------------------- */

static const char *const EVENTS[SRV_EVENT_COUNT] = { "spring", "summer", "halloween", "christmas", "new_year", "anniversary" };
const char *srv_event_name(int i) { return i >= 0 && i < SRV_EVENT_COUNT ? EVENTS[i] : NULL; }
uint32_t srv_events(const srv *s) { return s->events; }
db *srv_db(const srv *s) { return s->cfg.database; }
const char *srv_drop_tables_path(const srv *s) { return s->cfg.drop_tables_path; }
const char *srv_name(const srv *s) { return s->name; }
int srv_max_players(const srv *s) { return s->cfg.max_players; }
int srv_mode_online(const srv *s) { return s->cfg.mode == SRV_MODE_ONLINE; }

int srv_find_ip(const srv *s, uint64_t id, char *ip, size_t cap) {
    char addr[64];
    if (!s->enet || psp_net_enet_peer_address(s->enet, id, addr, sizeof addr) != 0) return -1;
    char *colon = strrchr(addr, ':');
    if (colon) *colon = 0;
    snprintf(ip, cap, "%s", addr);
    return 0;
}

int srv_players(const srv *s, srv_player *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS && n < max; i++) {
        const session *ss = &s->sess[i];
        if (ss->state != SS_ACTIVE) continue;
        srv_player *p = &out[n++];
        memset(p, 0, sizeof *p);
        p->session = ss->id;
        memcpy(p->name, ss->name, sizeof p->name);
        snprintf(p->mac, sizeof p->mac, "%02X:%02X:%02X:%02X:%02X:%02X", ss->mac[0], ss->mac[1], ss->mac[2], ss->mac[3], ss->mac[4], ss->mac[5]);
        psp_net_enet_peer_address(s->enet, ss->id, p->ip, sizeof p->ip);
        p->ping_ms = psp_net_enet_peer_rtt(s->enet, ss->id);
        p->joined_ms = ss->joined_ms;
        snprintf(p->location, sizeof p->location, "-");
    }
    return n;
}

int srv_kick(srv *s, uint64_t id, uint32_t reason) {
    session *ss = find(s, id);
    if (!ss) return -1;
    slog(s, "kicking session %llu ('%s'), reason %u", (unsigned long long)id, ss->name, (unsigned)reason);
    kick(s, ss, reason);
    return 0;
}

int srv_kick_ip(srv *s, const char *ip, uint32_t reason) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        char pip[64];
        if (ss->state == SS_FREE || srv_find_ip(s, ss->id, pip, sizeof pip) != 0 || strcmp(pip, ip)) continue;
        kick(s, ss, reason);
        n++;
    }
    return n;
}

int srv_ban_session(srv *s, uint64_t id, const char *reason, int64_t minutes, const char *admin, char *ip_out) {
    char ip[64];
    if (!s->cfg.database || !find(s, id) || srv_find_ip(s, id, ip, sizeof ip) != 0) return -1;
    const int64_t expires = minutes > 0 ? (int64_t)time(NULL) + minutes * 60 : 0;
    if (db_ip_ban(s->cfg.database, ip, reason, expires, admin) != DB_OK) return -1;
    if (ip_out) snprintf(ip_out, 64, "%s", ip);
    srv_kick_ip(s, ip, PSP_MMO_DISC_BANNED);
    return 0;
}

int srv_announce(srv *s, const char *text, uint32_t rgb) {
    if (!s->enet || !text || !*text) return -1;
    uint8_t m[PSP_MMO_NOTICE_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_SYSTEM_NOTICE);
    psp_mmo_put_u32(m + 8, rgb & 0xFFFFFFu);
    psp_mmo_put_str(m + 12, text, PSP_MMO_NOTICE_TEXT_LEN);
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sess[i].state != SS_ACTIVE) continue;
        const psp_net_peer to = { { 0 }, s->sess[i].id };
        n += t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m) == PSP_NET_OK;
    }
    slog(s, "announcement (#%06X) to %d players: %s", (unsigned)(rgb & 0xFFFFFFu), n, text);
    return n;
}

int srv_set_event(srv *s, const char *name, int on) {
    for (int i = 0; i < SRV_EVENT_COUNT; i++) {
        if (strcmp(name, EVENTS[i])) continue;
        const uint32_t was = s->events;
        if (on) s->events |= 1u << i; else s->events &= ~(1u << i);
        if (s->events != was) {
            slog(s, "event '%s' %s", name, on ? "on" : "off");
            for (int k = 0; k < MAX_SESSIONS; k++) if (s->sess[k].state == SS_ACTIVE) send_events(s, s->sess[k].id);
        }
        return 0;
    }
    return -1;
}
