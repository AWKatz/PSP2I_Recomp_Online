/* psprecomp — sceNetAdhocMatching over the MMO transport (ENABLE_MMO_NET).
 *
 * The matching library is the PSP's "who is hosting, who joins" layer:
 *   parent (mode 1): Start -> broadcasts HELLO (with its hello option data)
 *                    every hello interval while it has room;
 *   child  (mode 2): sees HELLO -> event HELLO(parent, opt); the game picks
 *                    a parent -> SelectTarget -> REQUEST(opt) to it;
 *   parent:          event REQUEST(child, opt); the game accepts with
 *                    SelectTarget(child) -> ACCEPT; both sides get
 *                    ESTABLISHED. CancelTarget refuses / leaves (DENY /
 *                    CANCEL / LEAVE on the other side). SendData -> DATA.
 *                    Stop / Delete -> BYE to everyone.
 * Messages ride the transport on the game's matching port; HELLO is
 * broadcast (the server relays it to every player past the handshake).
 *
 * Events reach the game's callback(id, event, mac*, optlen, opt*) between
 * frames, like the firmware's matching event thread would; mac and opt point
 * into a buffer in game memory. */

#include "adhoc_internal.h"

#include "psprecomp/hle.h"
#include "psprecomp/net.h"
#include "psprecomp/net_transport.h"

#include <stdlib.h>
#include <string.h>

enum { EV_HELLO = 1, EV_REQUEST = 2, EV_LEAVE = 3, EV_DENY = 4, EV_CANCEL = 5, EV_ACCEPT = 6,
       EV_ESTABLISHED = 7, EV_TIMEOUT = 8, EV_ERROR = 9, EV_BYE = 10, EV_DATA = 11, EV_DATA_ACK = 12 };
enum { MODE_PARENT = 1, MODE_CHILD = 2, MODE_P2P = 3 };
enum { M_HELLO = 1, M_REQUEST, M_ACCEPT, M_CANCEL, M_BYE, M_DATA, M_DATA_ACK };
enum { P_NONE = 0, P_SEEN, P_REQUESTED, P_INCOMING, P_ESTABLISHED };   /* a peer's state, from our side */

#define MAX_PEERS 16
#define MAX_OPT   4096
#define MAX_EVENTS 64
#define ERR_INVALID_ID   0x80410807u
#define ERR_NOT_RUNNING  0x80410808u
#define ERR_INVALID_ARG  0x80410804u
#define ERR_NOT_INIT     0x80410813u

typedef struct { uint8_t mac[6]; int state; } peer;

static struct {
    int inited, used, running, id;
    int mode, maxnum;
    uint16_t port;
    uint32_t hello_us, callback;
    uint8_t *hello; uint32_t hello_len;
    peer peers[MAX_PEERS];
    uint64_t last_hello;
} M;

typedef struct { int ev; uint8_t mac[6]; uint32_t len; uint8_t *data; } event;
static event g_ev[MAX_EVENTS];
static int g_nev;
static uint32_t g_buf;                        /* game memory for an event's mac + data */

static void ev_queue(int ev, const uint8_t mac[6], const void *data, uint32_t len) {
    if (g_nev >= MAX_EVENTS) return;
    event *e = &g_ev[g_nev++];
    e->ev = ev;
    memcpy(e->mac, mac, 6);
    e->len = len > MAX_OPT ? MAX_OPT : len;
    e->data = e->len ? malloc(e->len) : NULL;
    if (e->data) memcpy(e->data, data, e->len); else e->len = 0;
}

static const char *ev_name(int ev) {
    static const char *n[] = { "?", "HELLO", "REQUEST", "LEAVE", "DENY", "CANCEL", "ACCEPT", "ESTABLISHED",
                               "TIMEOUT", "ERROR", "BYE", "DATA", "DATA_ACK" };
    return ev > 0 && ev <= EV_DATA_ACK ? n[ev] : "?";
}

static peer *find_peer(const uint8_t mac[6], int add) {
    peer *free_slot = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (M.peers[i].state && !memcmp(M.peers[i].mac, mac, 6)) return &M.peers[i];
        if (!M.peers[i].state && !free_slot) free_slot = &M.peers[i];
    }
    if (!add || !free_slot) return NULL;
    memcpy(free_slot->mac, mac, 6);
    free_slot->state = P_SEEN;
    return free_slot;
}

static int established_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++) n += M.peers[i].state == P_ESTABLISHED;
    return n;
}

/* ---- sending ---- */

static int send_msg(const uint8_t *to_mac, int type, const void *data, uint32_t len, int reliable) {
    const psp_net_transport *t = psp_net_get_transport();
    if (!mmo_net_ready()) return -1;
    uint8_t buf[1 + MAX_OPT];
    if (len > MAX_OPT) len = MAX_OPT;
    buf[0] = (uint8_t)type;
    if (len) memcpy(buf + 1, data, len);
    psp_net_peer to;
    if (to_mac) { memcpy(to.mac, to_mac, 6); to.id = 0; }
    return t->send_packet(t->ctx, to_mac ? &to : NULL, reliable ? PSP_NET_CH_RELIABLE : PSP_NET_CH_UNRELIABLE,
                          M.port, buf, len + 1);
}

/* ---- receiving (adhoc_mmo.c routes packets on our port here) ---- */

int mmo_matching_port(void) { return M.used ? M.port : -1; }

void mmo_matching_on_packet(const psp_net_packet *p) {
    if (!M.used || !M.running || p->len < 1) return;
    const int type = p->data[0];
    const uint8_t *opt = p->data + 1;
    const uint32_t olen = p->len - 1;
    const uint8_t *mac = p->from.mac;
    peer *pe = find_peer(mac, 0);
    switch (type) {
    case M_HELLO:
        if (M.mode != MODE_CHILD && M.mode != MODE_P2P) break;
        if (!pe) {
            pe = find_peer(mac, 1);
            if (pe) ev_queue(EV_HELLO, mac, opt, olen);
        }
        break;
    case M_REQUEST:
        if (M.mode == MODE_CHILD) break;
        if (!pe) pe = find_peer(mac, 1);
        if (!pe) break;
        if (established_count() >= M.maxnum - 1) { send_msg(mac, M_CANCEL, NULL, 0, 1); pe->state = P_NONE; break; }
        pe->state = P_INCOMING;
        ev_queue(EV_REQUEST, mac, opt, olen);
        break;
    case M_ACCEPT:
        if (!pe || pe->state != P_REQUESTED) break;
        pe->state = P_ESTABLISHED;
        ev_queue(EV_ACCEPT, mac, opt, olen);
        ev_queue(EV_ESTABLISHED, mac, NULL, 0);
        break;
    case M_CANCEL:
        if (!pe) break;
        ev_queue(pe->state == P_REQUESTED ? EV_DENY : pe->state == P_INCOMING ? EV_CANCEL : EV_LEAVE, mac, opt, olen);
        pe->state = P_NONE;
        break;
    case M_BYE:
        if (!pe) break;
        ev_queue(EV_BYE, mac, NULL, 0);
        pe->state = P_NONE;
        break;
    case M_DATA:
        if (!pe || pe->state != P_ESTABLISHED) break;
        ev_queue(EV_DATA, mac, opt, olen);
        send_msg(mac, M_DATA_ACK, NULL, 0, 1);
        break;
    case M_DATA_ACK:
        if (pe && pe->state == P_ESTABLISHED) ev_queue(EV_DATA_ACK, mac, NULL, 0);
        break;
    }
}

/* ---- per frame: HELLO broadcasts, event delivery ---- */

void mmo_matching_pump(void) {
    if (M.used && M.running && (M.mode == MODE_PARENT || M.mode == MODE_P2P) && mmo_net_ready()) {
        const uint64_t now = psp_wall_us();
        if (now - M.last_hello >= (M.hello_us ? M.hello_us : 1000000u) && established_count() < M.maxnum - 1) {
            M.last_hello = now;
            send_msg(NULL, M_HELLO, M.hello, M.hello_len, 0);
        }
    }
    if (!g_nev) return;
    if (!g_buf) g_buf = psp_sysmem_alloc(16 + MAX_OPT, 1);
    const int n = g_nev;
    g_nev = 0;
    for (int i = 0; i < n; i++) {
        event *e = &g_ev[i];
        if (g_buf && M.used && M.callback) {
            psp_mem_write_block(g_buf, e->mac, 6);
            if (e->len) psp_mem_write_block(g_buf + 16, e->data, e->len);
            psp_net_log("mmo: matching event %s from %02X:%02X:%02X:%02X:%02X:%02X (%u bytes)", ev_name(e->ev),
                        e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->len);
            psp_call_guest5(M.callback, (uint32_t)M.id, (uint32_t)e->ev, g_buf, e->len, e->len ? g_buf + 16 : 0);
        }
        free(e->data);
    }
}

/* ---- the API ---- */

static void hle_MatchingInit(void) {
    M.inited = 1;
    psp_net_log("mmo: sceNetAdhocMatchingInit(pool %u)", psp_arg(0));
    psp_ret(0);
}

static void hle_MatchingTerm(void) {
    M.inited = 0;
    psp_ret(0);
}

/* Create(mode, maxnum, port, rxbuflen, hello_int, keepalive_int, init_count, rexmt_int, callback) */
static void hle_MatchingCreate(void) {
    if (!M.inited) { psp_ret(ERR_NOT_INIT); return; }
    const int mode = (int)psp_arg(0), maxnum = (int)psp_arg(1);
    if (mode < MODE_PARENT || mode > MODE_P2P || maxnum < 2 || maxnum > 16) { psp_ret(ERR_INVALID_ARG); return; }
    free(M.hello);
    memset(&M, 0, sizeof M);
    M.inited = 1;
    M.used = 1;
    M.id = 1;
    M.mode = mode;
    M.maxnum = maxnum;
    M.port = (uint16_t)psp_arg(2);
    M.hello_us = psp_arg(4);
    M.callback = psp_read32(psp_cpu.r[PSP_REG_SP]);    /* the 9th argument: EABI passes it at 0($sp) */
    psp_net_log("mmo: sceNetAdhocMatchingCreate(mode %d, max %d, port %u, hello %u us, callback 0x%08X) -> %d",
                mode, maxnum, M.port, M.hello_us, M.callback, M.id);
    psp_ret((uint32_t)M.id);
}

/* Start(id, evthpri, evthstack, inthpri, inthstack, optlen, optdata) */
static void hle_MatchingStart(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    const uint32_t len = psp_arg(5), data = psp_arg(6);
    free(M.hello);
    M.hello = NULL;
    M.hello_len = 0;
    if (len && data && len <= MAX_OPT) {
        M.hello = malloc(len);
        if (M.hello) { psp_mem_read_block(M.hello, data, len); M.hello_len = len; }
    }
    M.running = 1;
    M.last_hello = 0;
    psp_net_log("mmo: sceNetAdhocMatchingStart(%d, hello opt %u bytes) as %s", M.id, M.hello_len,
                M.mode == MODE_PARENT ? "parent" : M.mode == MODE_CHILD ? "child" : "p2p");
    psp_ret(0);
}

static void bye_all(void) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (M.peers[i].state == P_ESTABLISHED || M.peers[i].state == P_REQUESTED || M.peers[i].state == P_INCOMING)
            send_msg(M.peers[i].mac, M_BYE, NULL, 0, 1);
    memset(M.peers, 0, sizeof M.peers);
}

static void hle_MatchingStop(void) {
    if (M.used && psp_arg(0) == (uint32_t)M.id) { bye_all(); M.running = 0; }
    psp_ret(0);
}

static void hle_MatchingDelete(void) {
    if (M.used && psp_arg(0) == (uint32_t)M.id) { bye_all(); free(M.hello); memset(&M, 0, sizeof M); M.inited = 1; }
    psp_ret(0);
}

/* SelectTarget(id, mac, optlen, optdata) */
static void hle_MatchingSelectTarget(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    if (!M.running) { psp_ret(ERR_NOT_RUNNING); return; }
    uint8_t mac[6], opt[MAX_OPT];
    psp_mem_read_block(mac, psp_arg(1), 6);
    uint32_t len = psp_arg(2);
    if (len > MAX_OPT) len = MAX_OPT;
    if (len && psp_arg(3)) psp_mem_read_block(opt, psp_arg(3), len); else len = 0;
    peer *pe = find_peer(mac, M.mode != MODE_PARENT);
    if (!pe) { psp_ret(ERR_INVALID_ARG); return; }
    if (M.mode == MODE_PARENT || (M.mode == MODE_P2P && pe->state == P_INCOMING)) {
        if (pe->state != P_INCOMING) { psp_ret(ERR_INVALID_ARG); return; }
        pe->state = P_ESTABLISHED;                            /* accept the request */
        send_msg(mac, M_ACCEPT, opt, len, 1);
        ev_queue(EV_ESTABLISHED, mac, NULL, 0);
    } else {
        pe->state = P_REQUESTED;                              /* ask to join */
        send_msg(mac, M_REQUEST, opt, len, 1);
    }
    psp_net_log("mmo: sceNetAdhocMatchingSelectTarget(%02X:%02X:%02X:%02X:%02X:%02X, %u bytes)", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], len);
    psp_ret(0);
}

static void cancel_target(uint32_t mac_addr, uint32_t optlen, uint32_t optdata) {
    uint8_t mac[6], opt[MAX_OPT];
    psp_mem_read_block(mac, mac_addr, 6);
    if (optlen > MAX_OPT) optlen = MAX_OPT;
    if (optlen && optdata) psp_mem_read_block(opt, optdata, optlen); else optlen = 0;
    peer *pe = find_peer(mac, 0);
    if (pe) { send_msg(mac, M_CANCEL, opt, optlen, 1); pe->state = P_NONE; }
}

static void hle_MatchingCancelTarget(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    cancel_target(psp_arg(1), 0, 0);
    psp_ret(0);
}

static void hle_MatchingCancelTargetWithOpt(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    cancel_target(psp_arg(1), psp_arg(2), psp_arg(3));
    psp_ret(0);
}

/* SetHelloOpt(id, optlen, optdata) / GetHelloOpt(id, *optlen, optdata) */
static void hle_MatchingSetHelloOpt(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    const uint32_t len = psp_arg(1), data = psp_arg(2);
    free(M.hello);
    M.hello = NULL;
    M.hello_len = 0;
    if (len && data && len <= MAX_OPT && (M.hello = malloc(len))) { psp_mem_read_block(M.hello, data, len); M.hello_len = len; }
    psp_ret(0);
}

static void hle_MatchingGetHelloOpt(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    const uint32_t plen = psp_arg(1), data = psp_arg(2);
    if (plen) {
        const uint32_t cap = psp_read32(plen);
        if (data && M.hello_len) psp_mem_write_block(data, M.hello, M.hello_len < cap ? M.hello_len : cap);
        psp_write32(plen, M.hello_len);
    }
    psp_ret(0);
}

/* SendData(id, mac, datalen, data) */
static void hle_MatchingSendData(void) {
    if (!M.used || psp_arg(0) != (uint32_t)M.id) { psp_ret(ERR_INVALID_ID); return; }
    uint8_t mac[6], data[MAX_OPT];
    psp_mem_read_block(mac, psp_arg(1), 6);
    uint32_t len = psp_arg(2);
    if (!len || len > MAX_OPT || !psp_arg(3)) { psp_ret(ERR_INVALID_ARG); return; }
    psp_mem_read_block(data, psp_arg(3), len);
    peer *pe = find_peer(mac, 0);
    if (!pe || pe->state != P_ESTABLISHED) { psp_ret(0x80410809u); return; }   /* not established */
    send_msg(mac, M_DATA, data, len, 1);
    psp_ret(0);
}

static void hle_MatchingAbortSendData(void) { psp_ret(0); }

void mmo_matching_register(void) {
    psp_hle_register(0x2A2A1E07, "sceNetAdhocMatching", "sceNetAdhocMatchingInit", hle_MatchingInit);
    psp_hle_register(0x7945ECDA, "sceNetAdhocMatching", "sceNetAdhocMatchingTerm", hle_MatchingTerm);
    psp_hle_register(0xCA5EDA6F, "sceNetAdhocMatching", "sceNetAdhocMatchingCreate", hle_MatchingCreate);
    psp_hle_register(0x93EF3843, "sceNetAdhocMatching", "sceNetAdhocMatchingStart", hle_MatchingStart);
    psp_hle_register(0x32B156B3, "sceNetAdhocMatching", "sceNetAdhocMatchingStop", hle_MatchingStop);
    psp_hle_register(0xF16EAF4F, "sceNetAdhocMatching", "sceNetAdhocMatchingDelete", hle_MatchingDelete);
    psp_hle_register(0x5E3D4B79, "sceNetAdhocMatching", "sceNetAdhocMatchingSelectTarget", hle_MatchingSelectTarget);
    psp_hle_register(0xEA3C6108, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTarget", hle_MatchingCancelTarget);
    psp_hle_register(0x8F58BEDF, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTargetWithOpt", hle_MatchingCancelTargetWithOpt);
    psp_hle_register(0xB58E61B7, "sceNetAdhocMatching", "sceNetAdhocMatchingSetHelloOpt", hle_MatchingSetHelloOpt);
    psp_hle_register(0xB5D96C2A, "sceNetAdhocMatching", "sceNetAdhocMatchingGetHelloOpt", hle_MatchingGetHelloOpt);
    psp_hle_register(0xF79472D7, "sceNetAdhocMatching", "sceNetAdhocMatchingSendData", hle_MatchingSendData);
    psp_hle_register(0xEC19337D, "sceNetAdhocMatching", "sceNetAdhocMatchingAbortSendData", hle_MatchingAbortSendData);
}
