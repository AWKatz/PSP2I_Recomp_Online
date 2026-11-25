/* psprecomp — sceNpMatching2 (PSN room matching) for Internet Multi Mode, MMO build.
 *
 * The game finds servers, worlds, lobbies and rooms through sceNpMatching2.
 * Here they come from our dedicated server (one server, one world, lobby
 * rooms of 12, party rooms of 4). Work in progress: each call is
 * implemented as the game reaches it; docs/psn_lobby_plan.md has the plan.
 *
 * NIDs are named after the game's own log lines (PSP2I_GAMELOG=1) where it
 * names the call; the others are left unnamed until they are seen. Every
 * call is logged with its arguments (np2: lines). */

#include "adhoc_internal.h"

#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/net.h"
#include "psprecomp/net_proto.h"

#include <stdio.h>
#include <string.h>

#ifndef SCE_KERNEL_ERROR_ERROR
#define SCE_KERNEL_ERROR_ERROR 0x80020001u
#endif

static void log_call(const char *name, const char *result) {
    psp_net_log("np2: %s 0x%08X(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) -> %s", name ? name : "NID",
                psp_hle_current_nid(), psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), psp_arg(4), result);
}

/* ---- context ------------------------------------------------------------------------- */

#define CTX_ID 1
static int g_ctx_created;

/* sceNpMatching2CreateContext(const SceNpId *, const SceNpCommunicationId *, u16 *ctx_id) */
static void hle_CreateContext(void) {
    if (psp_arg(2)) psp_write16(psp_arg(2), CTX_ID);
    g_ctx_created = 1;
    psp_net_mmo_connect();                                 /* rooms live on our server */
    log_call("sceNpMatching2CreateContext", "0 (context 1)");
    psp_ret(0);
}

/* sceNpMatching2ContextStart(ctx_id, timeout?) */
static int g_ctx_started;
static void hle_ContextStart(void) {
    g_ctx_started = 1;
    log_call("sceNpMatching2ContextStart", "0");
    psp_ret(0);
}

/* sceNpMatching2RegisterSignalingCallback(ctx_id, callback, arg) */
static uint32_t g_sig_cb, g_sig_arg;
static void hle_RegisterSignalingCallback(void) {
    g_sig_cb = psp_arg(1); g_sig_arg = psp_arg(2);
    log_call("sceNpMatching2RegisterSignalingCallback", "0");
    psp_ret(0);
}

/* ---- asynchronous requests ----------------------------------------------------------- */
/* A request returns its id at once; its callback runs on the next frame (pump) with the
 * reply data in guest memory: cb(u16 ctx, u32 request id, u16 event, s32 error, void *data,
 * void *arg) -- the callback 0x08CB301C reads them so. The options block passed with a
 * request: +0 callback, +4 argument, +8 timeout, +12 application request id. */

#define EV_GET_SERVER_INFO     0x0001
#define EV_GET_WORLD_INFO_LIST 0x0002
#define EV_CREATE_JOIN_ROOM    0x0101
#define EV_LEAVE_ROOM          0x0103
#define EV_SEARCH_ROOM         0x0106
#define EV_SEND_ROOM_MESSAGE   0x0108
#define EV_SET_ROOM_DATA_INTERNAL 0x0109

#define MAX_REQ 8
#define REQ_DATA 16384
static struct { int used; uint32_t id, cb, arg; uint16_t event; int32_t error; uint32_t size; uint8_t data[REQ_DATA]; } g_req[MAX_REQ];
static uint32_t g_next_req = 1;
static uint32_t g_scratch;                                  /* guest memory for reply data */

/* Where reply data lands in guest memory, for replies that hold pointers into themselves. */
static uint32_t scratch(void) {
    if (!g_scratch) g_scratch = psp_sysmem_alloc(REQ_DATA, 1);
    return g_scratch;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Queue the callback for request id. */
static void enqueue(uint32_t id, uint32_t cb, uint32_t arg, uint16_t event, int32_t error, const void *data, uint32_t size) {
    for (int i = 0; i < MAX_REQ; i++)
        if (!g_req[i].used) {
            g_req[i].used = 1; g_req[i].id = id; g_req[i].cb = cb; g_req[i].arg = arg;
            g_req[i].event = event; g_req[i].error = error;
            g_req[i].size = size > REQ_DATA ? REQ_DATA : size;
            if (data && g_req[i].size) memcpy(g_req[i].data, data, g_req[i].size);
            return;
        }
    psp_net_log("np2: callback queue full; request %u dropped", id);
}

/* Queue a reply for the request whose options block is at opt; returns the request id (0: full). */
static uint32_t queue_reply(uint32_t opt, uint32_t req_id_out, uint16_t event, int32_t error, const void *data, uint32_t size) {
    for (int i = 0; i < MAX_REQ; i++)
        if (!g_req[i].used) {
            g_req[i].used = 1; g_req[i].id = g_next_req++;
            g_req[i].cb = opt ? psp_read32(opt) : 0; g_req[i].arg = opt ? psp_read32(opt + 4) : 0;
            g_req[i].event = event; g_req[i].error = error;
            g_req[i].size = size > REQ_DATA ? REQ_DATA : size;
            if (data && g_req[i].size) memcpy(g_req[i].data, data, g_req[i].size);
            if (req_id_out) psp_write32(req_id_out, g_req[i].id);
            return g_req[i].id;
        }
    return 0;
}


/* ---- servers and worlds ------------------------------------------------------------- */
/* One matching server (our dedicated server), id 1. */
#define SERVER_ID 1

/* sceNpMatching2GetServerIdListLocal(ctx_id, u16 *ids, max) -> number of ids */
static void hle_GetServerIdListLocal(void) {
    if (psp_arg(1) && psp_arg(2) >= 1) psp_write16(psp_arg(1), SERVER_ID);
    log_call("sceNpMatching2GetServerIdListLocal", "1 (server 1)");
    psp_ret(1);
}

static void dump_words(const char *what, uint32_t a, int n) {
    if (!a) return;
    char line[256]; size_t o = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(line + o, sizeof line - o, "%08X ", psp_read32(a + 4u * (uint32_t)i));
    psp_net_log("np2:   %s @0x%08X: %s", what, a, line);
}

/* sceNpMatching2GetServerInfo(ctx_id, const {u16 server id} *, options *, u32 *request id).
 * Reply: SceNpMatching2Server {u16 server id, u8 status (1 available)}. */
static void hle_GetServerInfo(void) {
    uint8_t server[4] = { SERVER_ID & 0xFF, SERVER_ID >> 8, 1, 0 };
    const uint32_t id = queue_reply(psp_arg(2), psp_arg(3), EV_GET_SERVER_INFO, 0, server, sizeof server);
    log_call("sceNpMatching2GetServerInfo", id ? "0 (queued)" : "busy");
    psp_ret(id ? 0 : SCE_KERNEL_ERROR_ERROR);
}

/* sceNpMatching2GetWorldInfoList(ctx_id, const {u16 server id} *, options *, u32 *request id).
 * Reply: {World *worlds, u32 count}; World (linked): +0 next, +4 u32 world id, then counts
 * (lobbies, room members max / now, rooms, lobby members) -- the callback 0x08CB310C needs
 * count != 0 and world id != 0. One world, id 1. */
#define WORLD_ID 1
static void hle_GetWorldInfoList(void) {
    const uint32_t base = scratch();
    uint8_t d[8 + 64] = { 0 };
    put32(d, base + 8); put32(d + 4, 1);
    uint8_t *w = d + 8;
    put32(w + 0, 0);                  /* next */
    put32(w + 4, WORLD_ID);
    put32(w + 8, 1);                  /* lobbies */
    put32(w + 12, 32);                /* room members, max: the server's player cap */
    const uint32_t id = base ? queue_reply(psp_arg(2), psp_arg(3), EV_GET_WORLD_INFO_LIST, 0, d, sizeof d) : 0;
    log_call("sceNpMatching2GetWorldInfoList", id ? "0 (queued: world 1)" : "busy");
    psp_ret(id ? 0 : SCE_KERNEL_ERROR_ERROR);
}

/* ---- rooms ---------------------------------------------------------------------------- */
/* Rooms live on our server (net_proto.h: MSG_*_NP2_*). Layouts follow the PS3 SDK's
 * sceNpMatching2 structures with 32-bit pointers, checked against the game:
 *   CreateJoinRoom request: +0 world, +8 lobby u64, +16 max slots, +20 flags, +24/+28 internal
 *     bin attrs, +32/+36 searchable ints {u16 id, u32 value}, +40/+44 searchable bins,
 *     +48/+52 external bins {u16 id, void *data, u32 size};
 *   RoomDataInternal: +16 room u64, +32 max, +36 members, +40 count, +44 me, +48 owner, +60 flags;
 *   member: +0 next, +4 NpId, +40 online name *, +56 member id;
 *   RoomDataExternal (88 bytes): +0 next, +4 server, +8 world, +24 room u64, +32 u32 max slots,
 *     +36 u32 members (not the PS3's u16 pairs: the lobby code 0x08CAA0B8 reads +36 as a word), +48 owner (SceNpUserInfo2: NpId, +36 online name *), +60 flags,
 *     +64/+68 searchable ints, +72/+76 searchable bins, +80/+84 external bins. */

#define EV_JOIN_ROOM 0x0102

typedef struct { uint16_t member_id; char name[PSP_MMO_NP2_NAME_LEN + 1]; uint32_t addr; } np2_member;
typedef struct {
    uint64_t room_id;
    uint32_t max_slots, flags;
    int      count, me, owner;
    np2_member members[PSP_MMO_NP2_ROOM_MAX];
} np2_room;

static np2_room g_room;                                    /* the room we are in (room_id 0: none) */

/* Signaling: the game learns that it can reach another member (Established) or no longer
 * can (Dead) through the callback it registered: cb(ctx, -, room u64 in $a2:$a3, member
 * $t0, event $t1, error $t2, arg $t3) -- 0x08CB35FC. Then it asks the connection status
 * for the member's address. On our server every member is reachable at once. */
#define SIG_DEAD        0x5101
#define SIG_ESTABLISHED 0x5102
static struct { uint16_t member, event; } g_sig[32];
/* Room events (member joined / left) and room messages, for the next frame. */
#define ROOM_EV_MEMBER_JOINED 0x1101
#define ROOM_EV_MEMBER_LEFT   0x1102
#define ROOM_MSG_MESSAGE      0x2102
typedef struct { int kind; uint16_t event, member; char name[PSP_MMO_NP2_NAME_LEN + 1]; uint16_t size; uint8_t data[PSP_MMO_NP2_MSG_MAX]; } room_post;
static room_post g_post[16];
static int g_npost;
static int g_nsig;
static void signal_member(uint16_t member, uint16_t event) {
    if (g_nsig < 32) { g_sig[g_nsig].member = member; g_sig[g_nsig].event = event; g_nsig++; }
}

/* Requests waiting for the server's answer. */
static struct { int used; uint32_t id, cb, arg; uint16_t event; } g_pend[MAX_REQ];

static uint32_t pend_add(uint32_t opt, uint32_t req_id_out, uint16_t event) {
    for (int i = 0; i < MAX_REQ; i++)
        if (!g_pend[i].used) {
            g_pend[i].used = 1; g_pend[i].id = g_next_req++; g_pend[i].event = event;
            g_pend[i].cb = opt ? psp_read32(opt) : 0; g_pend[i].arg = opt ? psp_read32(opt + 4) : 0;
            if (req_id_out) psp_write32(req_id_out, g_pend[i].id);
            return g_pend[i].id;
        }
    return 0;
}

/* A growing layout in guest memory: bytes at d, guest address base + offset. */
typedef struct { uint8_t *d; uint32_t base, n, cap; } lay;
static uint32_t lay_take(lay *l, uint32_t size) {              /* -> offset; 4-aligned */
    const uint32_t o = (l->n + 3u) & ~3u;
    if (o + size > l->cap) return UINT32_MAX;
    memset(l->d + o, 0, size);
    l->n = o + size;
    return o;
}

/* RoomDataInternal for g_room, at a new place in l; returns its guest address. */
static uint32_t lay_room_internal(lay *l) {
    const np2_room *r = &g_room;
    const uint32_t ro = lay_take(l, 72), mo = lay_take(l, (uint32_t)r->count * 76), no = lay_take(l, (uint32_t)r->count * 24);
    if (ro == UINT32_MAX || mo == UINT32_MAX || no == UINT32_MAX) return 0;
    uint8_t *d = l->d + ro;
    const uint32_t mem0 = l->base + mo;
    d[0] = SERVER_ID; put32(d + 4, WORLD_ID);
    put32(d + 16, (uint32_t)r->room_id); put32(d + 20, (uint32_t)(r->room_id >> 32));
    put32(d + 32, r->max_slots);
    put32(d + 36, mem0); put32(d + 40, (uint32_t)r->count);
    put32(d + 44, mem0 + (uint32_t)r->me * 76);
    put32(d + 48, mem0 + (uint32_t)r->owner * 76);
    put32(d + 60, r->flags);
    for (int i = 0; i < r->count; i++) {
        uint8_t *m = l->d + mo + (uint32_t)i * 76;
        const size_t nl = strlen(r->members[i].name);
        if (i + 1 < r->count) put32(m, mem0 + (uint32_t)(i + 1) * 76);
        memcpy(m + 4, r->members[i].name, nl > 16 ? 16 : nl);          /* NpId: the online id first */
        memcpy(l->d + no + (uint32_t)i * 24, r->members[i].name, nl);
        put32(m + 40, l->base + no + (uint32_t)i * 24);
        m[56] = (uint8_t)r->members[i].member_id; m[57] = (uint8_t)(r->members[i].member_id >> 8);
    }
    return l->base + ro;
}

/* ATTRS (net_proto.h) -> the attribute arrays of a RoomDataExternal at ext. */
static void lay_attrs(lay *l, uint32_t ext, const uint8_t *a, uint32_t len) {
    if (len < 1) return;
    uint32_t p = 0;
    const uint32_t ni = a[p++];
    if (p + ni * 6 > len) return;
    const uint32_t io = lay_take(l, ni * 8);
    if (io == UINT32_MAX) return;
    for (uint32_t i = 0; i < ni; i++, p += 6) {
        l->d[io + 8 * i] = a[p]; l->d[io + 8 * i + 1] = a[p + 1];
        memcpy(l->d + io + 8 * i + 4, a + p + 2, 4);
    }
    put32(l->d + ext + 64, ni ? l->base + io : 0); put32(l->d + ext + 68, ni);
    if (p >= len) return;
    const uint32_t nb = a[p++];
    uint32_t cnt[2] = { 0, 0 }, q = p;
    for (uint32_t i = 0; i < nb && q + 5 <= len; i++) {      /* count each kind */
        const uint32_t sz = (uint32_t)(a[q + 3] | a[q + 4] << 8);
        if (a[q + 2] == 1 || a[q + 2] == 2) cnt[a[q + 2] - 1]++;
        q += 5 + sz;
    }
    uint32_t list[2], k_used[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        list[k] = cnt[k] ? lay_take(l, cnt[k] * 12) : 0;
        if (list[k] == UINT32_MAX) return;
        put32(l->d + ext + 72 + 8 * (uint32_t)k, cnt[k] ? l->base + list[k] : 0);
        put32(l->d + ext + 76 + 8 * (uint32_t)k, cnt[k]);
    }
    for (uint32_t i = 0; i < nb && p + 5 <= len; i++) {
        const uint8_t kind = a[p + 2];
        const uint32_t sz = (uint32_t)(a[p + 3] | a[p + 4] << 8);
        if (p + 5 + sz > len) return;
        if (kind == 1 || kind == 2) {
            const int k = kind - 1;
            const uint32_t data = lay_take(l, sz);
            if (data == UINT32_MAX) return;
            memcpy(l->d + data, a + p + 5, sz);
            uint8_t *e = l->d + list[k] + 12 * k_used[k]++;
            e[0] = a[p]; e[1] = a[p + 1];
            put32(e + 4, l->base + data); put32(e + 8, sz);
        }
        p += 5 + sz;
    }
}

static uint8_t g_lay[REQ_DATA];

/* The server's answers. */
static void on_reply(const uint8_t *m, uint32_t len) {
    if (len < 16) return;
    const uint32_t req = psp_mmo_get_u32(m + 8), status = psp_mmo_get_u32(m + 12);
    int i = 0;
    while (i < MAX_REQ && !(g_pend[i].used && g_pend[i].id == req)) i++;
    if (i == MAX_REQ) { psp_net_log("np2: answer to unknown request %u", req); return; }
    g_pend[i].used = 0;
    const uint16_t ev = g_pend[i].event;
    const uint32_t base = scratch();
    lay l = { g_lay, base, 0, sizeof g_lay };
    if (status != PSP_MMO_NP2_OK) {
        psp_net_log("np2: request %u (event 0x%04X) refused by the server: %u", req, ev, status);
        enqueue(req, g_pend[i].cb, g_pend[i].arg, ev, (int32_t)0x80550C01 + (int32_t)status, NULL, 0);
        return;
    }
    if (ev == EV_SEARCH_ROOM && len >= 18) {
        const uint32_t count = psp_mmo_get_u16(m + 16);
        const uint32_t ro = lay_take(&l, 16);
        put32(g_lay + ro, 1); put32(g_lay + ro + 4, count); put32(g_lay + ro + 8, count);
        uint32_t p = 18, prev = 0;
        for (uint32_t k = 0; k < count && p + PSP_MMO_NP2_ROOM_HDR_LEN <= len; k++) {
            const uint8_t *r = m + p;
            const uint32_t attrs_len = psp_mmo_get_u16(r + 38);
            const uint32_t e = lay_take(&l, 88), u = lay_take(&l, 44), nm = lay_take(&l, 24);
            if (e == UINT32_MAX || u == UINT32_MAX || nm == UINT32_MAX) break;
            if (k == 0) put32(g_lay + ro + 12, base + e); else put32(g_lay + prev, base + e);
            prev = e;
            uint8_t *x = g_lay + e;
            x[4] = SERVER_ID; put32(x + 8, WORLD_ID);
            memcpy(x + 24, r, 8);                                                   /* room id */
            const uint16_t max = psp_mmo_get_u16(r + 8), cur = psp_mmo_get_u16(r + 10);
            x[12] = (uint8_t)max;                                                   /* public slots */
            put32(x + 32, max); put32(x + 36, cur);   /* PSP: u32 max slots, u32 members (the lobby joins a room
                                                         with members below the login reply's +69 cap) */
            memcpy(g_lay + u, r + 14, 16);                                          /* owner NpId: online id */
            memcpy(g_lay + nm, r + 14, PSP_MMO_NP2_NAME_LEN);
            put32(g_lay + u + 36, base + nm);
            put32(x + 48, base + u);
            memcpy(x + 60, r + 34, 4);                                              /* flags */
            if (p + PSP_MMO_NP2_ROOM_HDR_LEN + attrs_len <= len) lay_attrs(&l, e, r + PSP_MMO_NP2_ROOM_HDR_LEN, attrs_len);
            p += PSP_MMO_NP2_ROOM_HDR_LEN + attrs_len;
        }
        psp_net_log("np2: search: %u room(s)", count);
        enqueue(req, g_pend[i].cb, g_pend[i].arg, ev, 0, g_lay, l.n);
        return;
    }
    if ((ev == EV_CREATE_JOIN_ROOM || ev == EV_JOIN_ROOM) && len >= 16 + PSP_MMO_NP2_ROOM_HDR_LEN) {
        const uint8_t *r = m + 16;
        const uint32_t attrs_len = psp_mmo_get_u16(r + 38);
        uint32_t p = 16 + PSP_MMO_NP2_ROOM_HDR_LEN + attrs_len;
        if (p + 2 > len) return;
        memset(&g_room, 0, sizeof g_room);
        g_room.room_id = psp_mmo_get_u64(r);
        g_room.max_slots = psp_mmo_get_u16(r + 8);
        g_room.flags = psp_mmo_get_u32(r + 34);
        const uint16_t owner = psp_mmo_get_u16(r + 12), me = psp_mmo_get_u16(m + p);
        p += 2;
        while (p + PSP_MMO_NP2_MEMBER_LEN <= len && g_room.count < PSP_MMO_NP2_ROOM_MAX) {
            np2_member *mb = &g_room.members[g_room.count];
            mb->member_id = psp_mmo_get_u16(m + p);
            memcpy(mb->name, m + p + 2, PSP_MMO_NP2_NAME_LEN); mb->name[PSP_MMO_NP2_NAME_LEN] = 0;
            mb->addr = psp_mmo_get_u32(m + p + 2 + PSP_MMO_NP2_NAME_LEN);
            if (mb->member_id == me) g_room.me = g_room.count;
            if (mb->member_id == owner) g_room.owner = g_room.count;
            g_room.count++;
            p += PSP_MMO_NP2_MEMBER_LEN;
        }
        const uint32_t wrapper = lay_take(&l, 4);
        put32(g_lay + wrapper, lay_room_internal(&l));                              /* {RoomDataInternal *} */
        psp_net_log("np2: in room %llu: %d member(s), I am %u, owner %u", (unsigned long long)g_room.room_id, g_room.count, me, owner);
        for (int k = 0; k < g_room.count; k++)
            if (k != g_room.me) signal_member(g_room.members[k].member_id, SIG_ESTABLISHED);
        enqueue(req, g_pend[i].cb, g_pend[i].arg, ev, 0, g_lay, l.n);
        return;
    }
    enqueue(req, g_pend[i].cb, g_pend[i].arg, ev, 0, NULL, 0);
}

void mmo_matching2_on_packet(const uint8_t *m, uint32_t len) {
    const uint16_t type = psp_mmo_type(m, len);
    if (type == MSG_S2C_NP2_REPLY) on_reply(m, len);
    else if (type == MSG_S2C_NP2_MEMBER && len >= 19 + PSP_MMO_NP2_MEMBER_LEN) {
        if (psp_mmo_get_u64(m + 8) != g_room.room_id) return;
        np2_member mb;
        memset(&mb, 0, sizeof mb);
        mb.member_id = psp_mmo_get_u16(m + 19);
        memcpy(mb.name, m + 21, PSP_MMO_NP2_NAME_LEN);
        mb.addr = psp_mmo_get_u32(m + 21 + PSP_MMO_NP2_NAME_LEN);
        const uint16_t owner = psp_mmo_get_u16(m + 17);
        int at = -1;
        for (int k = 0; k < g_room.count; k++) if (g_room.members[k].member_id == mb.member_id) at = k;
        if (m[16] && at < 0 && g_room.count < PSP_MMO_NP2_ROOM_MAX) g_room.members[g_room.count++] = mb;
        else if (!m[16] && at >= 0) {
            const uint16_t me_id = g_room.members[g_room.me].member_id;
            g_room.members[at] = g_room.members[--g_room.count];
            for (int k = 0; k < g_room.count; k++) if (g_room.members[k].member_id == me_id) g_room.me = k;
        }
        for (int k = 0; k < g_room.count; k++) if (g_room.members[k].member_id == owner) g_room.owner = k;
        psp_net_log("np2: room %llu: member %u '%s' %s (owner %u)", (unsigned long long)g_room.room_id, mb.member_id, mb.name,
                    m[16] ? "joined" : "left", owner);
        signal_member(mb.member_id, m[16] ? SIG_ESTABLISHED : SIG_DEAD);
        if (g_npost < 16) {
            room_post *q = &g_post[g_npost++];
            q->kind = 0; q->event = m[16] ? ROOM_EV_MEMBER_JOINED : ROOM_EV_MEMBER_LEFT; q->member = mb.member_id;
            memcpy(q->name, mb.name, sizeof q->name);
        }
    } else if (type == MSG_S2C_NP2_ROOM_MSG && len >= 20) {
        const uint16_t from = psp_mmo_get_u16(m + 16), size = psp_mmo_get_u16(m + 18);
        if (psp_mmo_get_u64(m + 8) != g_room.room_id || size > PSP_MMO_NP2_MSG_MAX || 20u + size > len || g_npost >= 16) return;
        room_post *q = &g_post[g_npost++];
        q->kind = 1; q->event = ROOM_MSG_MESSAGE; q->member = from; q->size = size;
        memcpy(q->data, m + 20, size);
        q->name[0] = 0;
        for (int k = 0; k < g_room.count; k++) if (g_room.members[k].member_id == from) memcpy(q->name, g_room.members[k].name, sizeof q->name);
        psp_net_log("np2: room message from member %u: %u bytes (category %u)", from, size, size >= 4 ? psp_mmo_get_u32(m + 20) : 0);
    }
}

/* Send a request to the server; the reply arrives through mmo_matching2_on_packet. */
static void server_request(const char *name, const uint8_t *msg, uint32_t len, uint32_t id) {
    if (!id || !mmo_net_ready()) {
        log_call(name, id ? "not connected to the server" : "busy");
        psp_ret(SCE_KERNEL_ERROR_ERROR);
        return;
    }
    mmo_ctrl_send(msg, len);
    log_call(name, "0 (sent to the server)");
    psp_ret(0);
}

/* sceNpMatching2SearchRoom(ctx_id, request *, options *, u32 *request id) */
static void hle_SearchRoom(void) {
    const uint32_t id = pend_add(psp_arg(2), psp_arg(3), EV_SEARCH_ROOM);
    uint8_t m[12];
    psp_mmo_put_hdr(m, MSG_C2S_NP2_SEARCH);
    psp_mmo_put_u32(m + 8, id);
    server_request("sceNpMatching2SearchRoom", m, sizeof m, id);
}

/* The request's attribute lists as ATTRS (net_proto.h). */
static uint32_t pack_attrs(uint32_t rq, uint8_t *a, uint32_t cap) {
    uint32_t p = 0;
    const uint32_t ia = psp_read32(rq + 32), ni = psp_read32(rq + 36);
    a[p++] = (uint8_t)(ia && ni <= 16 ? ni : 0);
    for (uint32_t i = 0; i < a[0]; i++) {
        psp_mmo_put_u16(a + p, psp_read16(ia + 8 * i)); psp_mmo_put_u32(a + p + 2, psp_read32(ia + 8 * i + 4)); p += 6;
    }
    const uint32_t nb_at = p++;
    a[nb_at] = 0;
    for (int k = 0; k < 2; k++) {                         /* searchable bins (kind 1), external bins (kind 2) */
        const uint32_t arr = psp_read32(rq + 40 + 8 * (uint32_t)k), num = psp_read32(rq + 44 + 8 * (uint32_t)k);
        for (uint32_t i = 0; arr && i < num && i < 8; i++) {
            const uint32_t e = arr + 12 * i, ptr = psp_read32(e + 4), sz = psp_read32(e + 8);
            if (p + 5 + sz > cap || sz > 512) continue;
            psp_mmo_put_u16(a + p, psp_read16(e)); a[p + 2] = (uint8_t)(k + 1); psp_mmo_put_u16(a + p + 3, (uint16_t)sz);
            if (ptr && sz) psp_mem_read_block(a + p + 5, ptr, sz);
            p += 5 + sz;
            a[nb_at]++;
        }
    }
    return p;
}

/* sceNpMatching2CreateJoinRoom(ctx_id, request *, options *, u32 *request id, ?) */
/* CreateJoinRoom and JoinRoom also take the room's callbacks: $a3 {room event callback, arg},
 * $t0 {room message callback, arg}; the request id goes to $t1. */
static uint32_t g_ev_cb, g_ev_arg, g_msg_cb, g_msg_arg;
static void take_room_callbacks(void) {
    const uint32_t ev = psp_arg(3), msg = psp_cpu.r[8];
    if (ev) { g_ev_cb = psp_read32(ev); g_ev_arg = psp_read32(ev + 4); }
    if (msg) { g_msg_cb = psp_read32(msg); g_msg_arg = psp_read32(msg + 4); }
}

static void hle_CreateJoinRoom(void) {
    const uint32_t rq = psp_arg(1);
    take_room_callbacks();
    const uint32_t id = rq ? pend_add(psp_arg(2), psp_cpu.r[9], EV_CREATE_JOIN_ROOM) : 0;
    static uint8_t m[22 + PSP_MMO_NP2_ATTRS_MAX];
    psp_mmo_put_hdr(m, MSG_C2S_NP2_CREATE);
    psp_mmo_put_u32(m + 8, id);
    psp_mmo_put_u16(m + 12, rq ? (uint16_t)psp_read32(rq + 16) : 4);
    psp_mmo_put_u16(m + 14, 0);
    psp_mmo_put_u32(m + 16, rq ? psp_read32(rq + 20) : 0);
    const uint32_t alen = rq ? pack_attrs(rq, m + 22, PSP_MMO_NP2_ATTRS_MAX) : 0;
    psp_mmo_put_u16(m + 20, (uint16_t)alen);
    server_request("sceNpMatching2CreateJoinRoom", m, 22 + alen, id);
}

/* sceNpMatching2JoinRoom(ctx_id, {u64 room id, ...} *, options *, u32 *request id) */
static void hle_JoinRoom(void) {
    const uint32_t rq = psp_arg(1);
    take_room_callbacks();
    const uint32_t id = rq ? pend_add(psp_arg(2), psp_cpu.r[9], EV_JOIN_ROOM) : 0;
    uint8_t m[20];
    psp_mmo_put_hdr(m, MSG_C2S_NP2_JOIN);
    psp_mmo_put_u32(m + 8, id);
    psp_mmo_put_u32(m + 12, rq ? psp_read32(rq) : 0); psp_mmo_put_u32(m + 16, rq ? psp_read32(rq + 4) : 0);
    server_request("sceNpMatching2JoinRoom", m, sizeof m, id);
}

/* sceNpMatching2SignalingGetConnectionStatus(ctx, -, room u64 in $a2:$a3, member $t0,
 * int *status $t1, u32 *address $t2, u16 *port $t3): every member we know is reachable
 * (status 2, active) at the address the server saw, on the signaling port 3658 (both in
 * network order). */
#define SIG_STATUS_ACTIVE 2
#define SIG_PORT 3658
static void hle_SignalingGetConnectionStatus(void) {
    const uint16_t member = (uint16_t)psp_cpu.r[8];
    const uint32_t st = psp_cpu.r[9], ad = psp_cpu.r[10], po = psp_cpu.r[11];
    int k = 0;
    while (k < g_room.count && g_room.members[k].member_id != member) k++;
    if (k == g_room.count) {
        psp_net_log("np2: connection status of member %u: not in the room", member);
        if (st) psp_write32(st, 0);
        psp_ret(0x80550C08u);                              /* no such member */
        return;
    }
    const uint32_t a = g_room.members[k].addr;              /* network order, as stored */
    if (st) psp_write32(st, SIG_STATUS_ACTIVE);
    if (ad) psp_write32(ad, a);
    if (po) psp_write16(po, (uint16_t)((SIG_PORT >> 8) | (SIG_PORT << 8)));
    psp_net_log("np2: connection status of member %u '%s': active, %u.%u.%u.%u:%u", member, g_room.members[k].name,
                a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24, SIG_PORT);
    psp_ret(0);
}

/* Requests that succeed with no reply data. */
static void ok_request(const char *name, uint16_t event) {
    const uint32_t id = queue_reply(psp_arg(2), psp_arg(3), event, 0, NULL, 0);
    log_call(name, id ? "0 (queued)" : "busy");
    psp_ret(id ? 0 : SCE_KERNEL_ERROR_ERROR);
}
/* sceNpMatching2SetRoomDataInternal(ctx, {u64 room id, u32 flag filter, u32 flags, bin attrs...} *, options *, u32 *) */
static void hle_SetRoomDataInternal(void) { ok_request("sceNpMatching2SetRoomDataInternal", EV_SET_ROOM_DATA_INTERNAL); }
/* sceNpMatching2SetRoomDataExternal(ctx, {u64 room id, searchable ints, bins...} *, options *, u32 *):
 * the owner updates what searches show (e.g. the player count). Accepted; the server's copy
 * keeps the attributes from creation for now. */
static void hle_SetRoomDataExternal(void) { ok_request("sceNpMatching2SetRoomDataExternal", 0x0004); }

/* sceNpMatching2LeaveRoom(ctx, {u64 room id, opt data} *, options *, u32 *) */
static void hle_LeaveRoom(void) {
    uint8_t m[20];
    psp_mmo_put_hdr(m, MSG_C2S_NP2_LEAVE);
    psp_mmo_put_u32(m + 8, 0);
    psp_mmo_put_u64(m + 12, g_room.room_id);
    mmo_ctrl_send(m, sizeof m);                            /* the server's answer is not waited for */
    memset(&g_room, 0, sizeof g_room);
    ok_request("sceNpMatching2LeaveRoom", EV_LEAVE_ROOM);
}

/* sceNpMatching2SendRoomMessage(ctx, {u64 room id, u8 cast type, ..., +20 msg *, +24 size} *, options *, u32 *).
 * Relayed by the server to the other members. */
static void hle_SendRoomMessage(void) {
    const uint32_t rq = psp_arg(1);
    if (rq && g_room.room_id) {
        const uint32_t ptr = psp_read32(rq + 20), sz = psp_read32(rq + 24);
        if (sz <= PSP_MMO_NP2_MSG_MAX) {
            static uint8_t m[20 + PSP_MMO_NP2_MSG_MAX];
            psp_mmo_put_hdr(m, MSG_C2S_NP2_ROOM_MSG);
            psp_mmo_put_u64(m + 8, g_room.room_id);
            psp_mmo_put_u16(m + 16, 0);                    /* everyone else (cast type: see the log) */
            psp_mmo_put_u16(m + 18, (uint16_t)sz);
            if (ptr && sz) psp_mem_read_block(m + 20, ptr, sz);
            mmo_ctrl_send(m, 20 + sz);
            psp_net_log("np2: room message: %u bytes, cast type %u", sz, psp_read8(rq + 8));
        }
    }
    ok_request("sceNpMatching2SendRoomMessage", EV_SEND_ROOM_MESSAGE);
}

/* Not yet implemented: logged, and fail as before. */
static void hle_unimplemented(void) {
    dump_words("arg1", psp_arg(1), 8); dump_words("arg2", psp_arg(2), 8);
    log_call(psp_hle_name_of(psp_hle_current_nid()), "0x80020001 (not implemented)");
    psp_ret(SCE_KERNEL_ERROR_ERROR);
}

static const uint32_t NIDS[] = {
    0x5030CC53, 0x3DE70241, 0x190FF903, 0x2B3892FC, 0xF47342FC, 0x4EE3A8EC, 0xA53E7C69, 0xA3C298D1,
    0x6D6D0C75, 0xFADBA9DB, 0xAAD0946A, 0x7BBFC427, 0xC870535A, 0x81C13E6D, 0x12C5A111, 0xD7D4AEB2,
    0xE6C93DBD, 0xF940D9AD, 0x97529ECC, 0xC8FC5D41,
};

void mmo_matching2_register(void) {
    for (size_t i = 0; i < sizeof NIDS / sizeof NIDS[0]; i++)
        psp_hle_register(NIDS[i], "sceNpMatching2", NULL, hle_unimplemented);
    psp_hle_register(0x5030CC53, "sceNpMatching2", "sceNpMatching2CreateContext", hle_CreateContext);
    psp_hle_register(0xA3C298D1, "sceNpMatching2", "sceNpMatching2RegisterSignalingCallback", hle_RegisterSignalingCallback);
    psp_hle_register(0xF47342FC, "sceNpMatching2", "sceNpMatching2GetServerIdListLocal", hle_GetServerIdListLocal);
    psp_hle_register(0x4EE3A8EC, "sceNpMatching2", "sceNpMatching2GetServerInfo", hle_GetServerInfo);
    psp_hle_register(0xA53E7C69, "sceNpMatching2", "sceNpMatching2GetWorldInfoList", hle_GetWorldInfoList);
    psp_hle_register(0x81C13E6D, "sceNpMatching2", "sceNpMatching2SearchRoom", hle_SearchRoom);
    psp_hle_register(0xAAD0946A, "sceNpMatching2", "sceNpMatching2CreateJoinRoom", hle_CreateJoinRoom);
    psp_hle_register(0xE6C93DBD, "sceNpMatching2", "sceNpMatching2SetRoomDataInternal", hle_SetRoomDataInternal);
    psp_hle_register(0xC870535A, "sceNpMatching2", "sceNpMatching2LeaveRoom", hle_LeaveRoom);
    psp_hle_register(0xF940D9AD, "sceNpMatching2", "sceNpMatching2SendRoomMessage", hle_SendRoomMessage);
    psp_hle_register(0x7BBFC427, "sceNpMatching2", NULL, hle_JoinRoom);   /* sceNpMatching2JoinRoom: NID to be confirmed */
    psp_hle_register(0x6D6D0C75, "sceNpMatching2", "sceNpMatching2SignalingGetConnectionStatus", hle_SignalingGetConnectionStatus);
    psp_hle_register(0xD7D4AEB2, "sceNpMatching2", "sceNpMatching2SetRoomDataExternal", hle_SetRoomDataExternal);
    psp_hle_register(0x190FF903, "sceNpMatching2", "sceNpMatching2ContextStart", hle_ContextStart);
}

static void deliver_signaling(void) {
    for (int i = 0; i < g_nsig; i++) {
        if (!g_sig_cb) break;
        psp_net_log("np2: signaling: member %u %s -> callback 0x%08X", g_sig[i].member,
                    g_sig[i].event == SIG_ESTABLISHED ? "established" : "dead", g_sig_cb);
        psp_call_guest8(g_sig_cb, CTX_ID, 0, (uint32_t)g_room.room_id, (uint32_t)(g_room.room_id >> 32),
                        g_sig[i].member, g_sig[i].event, 0, g_sig_arg);
    }
    g_nsig = 0;
}

/* Room events: data = a member record (RoomMemberDataInternal, 76 bytes) wrapped as
 * {member *, u8 cause}. Room messages: RoomMessageInfo {u8 filtered, u8 cast type, -, dst *,
 * +8 sender SceNpUserInfo2 *, +12 message *, +16 size}. */
static void deliver_room(void) {
    const uint32_t base = scratch();
    for (int i = 0; i < g_npost && base; i++) {
        room_post *q = &g_post[i];
        uint8_t d[16 + 76 + 24 + PSP_MMO_NP2_MSG_MAX];
        memset(d, 0, sizeof d);
        const uint32_t lo = (uint32_t)g_room.room_id, hi = (uint32_t)(g_room.room_id >> 32);
        if (q->kind == 0) {
            if (!g_ev_cb) continue;
            put32(d, base + 16);                                   /* {member *, cause} */
            uint8_t *mr = d + 16;
            memcpy(mr + 4, q->name, strnlen(q->name, 16));
            put32(mr + 40, base + 16 + 76);
            memcpy(d + 16 + 76, q->name, strlen(q->name));
            mr[56] = (uint8_t)q->member; mr[57] = (uint8_t)(q->member >> 8);
            psp_mem_write_block(base, d, 16 + 76 + 24);
            psp_net_log("np2: room event 0x%04X (member %u '%s') -> callback 0x%08X", q->event, q->member, q->name, g_ev_cb);
            psp_call_guest8(g_ev_cb, CTX_ID, 0, lo, hi, q->event, base, g_ev_arg, 0);
        } else {
            if (!g_msg_cb) continue;
            d[1] = 1;                                               /* cast type: everyone */
            put32(d + 8, base + 24);                                /* sender: SceNpUserInfo2 (NpId, +36 name *) */
            memcpy(d + 24, q->name, strnlen(q->name, 16));
            put32(d + 24 + 36, base + 24 + 44);
            memcpy(d + 24 + 44, q->name, strlen(q->name));
            put32(d + 12, base + 96); put32(d + 16, q->size);
            memcpy(d + 96, q->data, q->size);
            psp_mem_write_block(base, d, 96u + q->size);
            psp_call_guest8(g_msg_cb, CTX_ID, 0, lo, hi, q->member, q->event, base, g_msg_arg);
        }
    }
    g_npost = 0;
}

/* Once a frame. Request replies first; signaling, room events and room messages wait for a
 * frame with no reply, so the game has taken in a join before it hears about the room. */
void mmo_matching2_pump(void) {
    int replied = 0;
    for (int i = 0; i < MAX_REQ; i++) {
        if (!g_req[i].used) continue;
        g_req[i].used = 0;
        if (!g_req[i].cb) continue;
        scratch();
        uint32_t data = 0;
        if (g_req[i].size && g_scratch) { psp_mem_write_block(g_scratch, g_req[i].data, g_req[i].size); data = g_scratch; }
        psp_net_log("np2: request %u: event 0x%04X, error 0x%08X, %u bytes -> callback 0x%08X", g_req[i].id, g_req[i].event,
                    (uint32_t)g_req[i].error, g_req[i].size, g_req[i].cb);
        psp_call_guest6(g_req[i].cb, CTX_ID, g_req[i].id, g_req[i].event, (uint32_t)g_req[i].error, data, g_req[i].arg);
        replied = 1;
    }
    if (replied) return;
    if (g_nsig) deliver_signaling();
    if (g_npost) deliver_room();
}

/* The server connection was lost: the room, the requests waiting on the server and the
 * events not yet delivered belong to the old session. A new session starts empty. */
void mmo_matching2_reset(void) {
    memset(&g_room, 0, sizeof g_room);
    memset(g_pend, 0, sizeof g_pend);
    g_npost = 0;
    g_nsig = 0;
}
