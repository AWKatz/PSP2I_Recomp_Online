/* psprecomp — psp_net_transport over ENet. See include/psprecomp/net_enet.h.
 *
 * Wire format (little-endian), first byte is the message type:
 *   HELLO    1  mac[6]                                      client -> host, on connect
 *   WELCOME  2  id u64, host_mac[6], host_id u64            host -> client
 *   DATA     3  src_mac[6], src_id u64, dst_mac[6], port u16, payload...
 * dst_mac FF:FF:FF:FF:FF:FF = every peer. */

#include "psprecomp/net_enet.h"

#include <enet/enet.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MSG_HELLO = 1, MSG_WELCOME = 2, MSG_DATA = 3 };
enum { HELLO_LEN = 7, WELCOME_LEN = 23, DATA_HDR = 23 };
#define HOST_ID      1u
#define MAX_PEERS_CAP 64

typedef struct {
    ENetPeer *peer;
    uint8_t   mac[6];
    uint64_t  id;
    int       welcomed;
} slot;

struct psp_net_enet {
    psp_net_transport  vt;
    psp_net_enet_config cfg;
    ENetHost *host;
    int       is_host, inited;
    uint8_t   self_mac[6];
    uint64_t  self_id, next_id;
    slot      slots[MAX_PEERS_CAP];   /* host: clients; client: [0] = the host */
    int       max_slots;
    uint8_t   buf[DATA_HDR + PSP_NET_MAX_PAYLOAD];
};

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static int g_enet_refs;

static void elog(psp_net_enet *e, const char *fmt, ...) {
    if (!e->cfg.log) return;
    char line[256];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    e->cfg.log(line);
}

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static slot *slot_by_peer(psp_net_enet *e, ENetPeer *p) {
    for (int i = 0; i < e->max_slots; i++) if (e->slots[i].peer == p) return &e->slots[i];
    return NULL;
}
static slot *slot_by_dest(psp_net_enet *e, const psp_net_peer *to) {
    for (int i = 0; i < e->max_slots; i++) {
        slot *s = &e->slots[i];
        if (!s->peer || !s->welcomed) continue;
        if (to->id ? s->id == to->id : memcmp(s->mac, to->mac, 6) == 0) return s;
    }
    return NULL;
}

static int raw_send(psp_net_enet *e, ENetPeer *p, int channel, const uint8_t *data, size_t len) {
    ENetPacket *pk = enet_packet_create(data, len, channel == PSP_NET_CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!pk) return PSP_NET_ERR;
    if (enet_peer_send(p, (enet_uint8)channel, pk) != 0) { enet_packet_destroy(pk); return PSP_NET_ERR; }
    return PSP_NET_OK;
}

/* ---- transport functions ------------------------------------------------------------- */

static int t_initialize(void *ctx, const uint8_t self_mac[6]) {
    psp_net_enet *e = ctx;
    if (e->inited) return PSP_NET_OK;
    if (g_enet_refs++ == 0 && enet_initialize() != 0) { g_enet_refs = 0; return PSP_NET_ERR; }
    memcpy(e->self_mac, self_mac, 6);
    e->inited = 1;
    return PSP_NET_OK;
}

static void t_shutdown(void *ctx) {
    psp_net_enet *e = ctx;
    if (e->host) {
        for (int i = 0; i < e->max_slots; i++) if (e->slots[i].peer) enet_peer_disconnect_now(e->slots[i].peer, 0);
        enet_host_flush(e->host);
        enet_host_destroy(e->host);
        e->host = NULL;
    }
    memset(e->slots, 0, sizeof e->slots);
    e->is_host = 0; e->self_id = 0;
    if (e->inited) { e->inited = 0; if (--g_enet_refs == 0) enet_deinitialize(); }
}

static int t_host_session(void *ctx, const char *group, int max_players) {
    psp_net_enet *e = ctx;
    if (!e->inited) return PSP_NET_ERR_NOT_INIT;
    if (e->host) return PSP_NET_ERR;
    if (max_players < 2) max_players = 2;
    if (max_players - 1 > MAX_PEERS_CAP) max_players = MAX_PEERS_CAP + 1;
    ENetAddress a;
    a.host = ENET_HOST_ANY;
    if (e->cfg.bind_host && enet_address_set_host_ip(&a, e->cfg.bind_host) != 0) return PSP_NET_ERR;
    a.port = e->cfg.port;
    e->host = enet_host_create(&a, (size_t)(max_players - 1), PSP_NET_CH_COUNT, 0, 0);
    if (!e->host) return PSP_NET_ERR;
    e->is_host = 1; e->self_id = HOST_ID; e->next_id = HOST_ID + 1;
    e->max_slots = max_players - 1;
    elog(e, "enet: hosting '%s' on port %u for %d players", group ? group : "", psp_net_enet_port(e), max_players);
    return PSP_NET_OK;
}

static int handle_event(psp_net_enet *e, ENetEvent *ev, psp_net_packet_fn fn, void *user);

static int t_join_session(void *ctx, const char *address, const char *group) {
    psp_net_enet *e = ctx;
    if (!e->inited) return PSP_NET_ERR_NOT_INIT;
    if (e->host || !address) return PSP_NET_ERR;
    char hostname[256];
    const char *colon = strrchr(address, ':');
    if (!colon || (size_t)(colon - address) >= sizeof hostname) return PSP_NET_ERR;
    memcpy(hostname, address, (size_t)(colon - address));
    hostname[colon - address] = 0;
    ENetAddress a;
    if (enet_address_set_host(&a, hostname) != 0) return PSP_NET_ERR;
    a.port = (enet_uint16)atoi(colon + 1);

    e->host = enet_host_create(NULL, 1, PSP_NET_CH_COUNT, 0, 0);
    if (!e->host) return PSP_NET_ERR;
    e->max_slots = 1;
    ENetPeer *p = enet_host_connect(e->host, &a, PSP_NET_CH_COUNT, 0);
    if (!p) { enet_host_destroy(e->host); e->host = NULL; return PSP_NET_ERR; }
    e->slots[0].peer = p;

    /* Wait for the connection and the host's WELCOME. */
    const uint32_t timeout = e->cfg.connect_timeout_ms ? e->cfg.connect_timeout_ms : 3000;
    const enet_uint32 start = enet_time_get();
    ENetEvent ev;
    while (!e->slots[0].welcomed && enet_time_get() - start < timeout) {
        if (e->cfg.idle) e->cfg.idle(e->cfg.idle_user);
        if (enet_host_service(e->host, &ev, e->cfg.idle ? 1 : 10) > 0) {
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            handle_event(e, &ev, NULL, NULL);
        }
    }
    if (!e->slots[0].welcomed) {
        elog(e, "enet: join %s failed", address);
        enet_peer_reset(p);
        enet_host_destroy(e->host); e->host = NULL;
        memset(e->slots, 0, sizeof e->slots);
        return PSP_NET_ERR_NO_PEER;
    }
    elog(e, "enet: joined '%s' at %s as id %llu", group ? group : "", address, (unsigned long long)e->self_id);
    return PSP_NET_OK;
}

static int t_send_packet(void *ctx, const psp_net_peer *to, int channel, uint16_t port, const void *data, uint32_t len) {
    psp_net_enet *e = ctx;
    if (!e->host) return PSP_NET_ERR_NOT_INIT;
    if (channel != PSP_NET_CH_RELIABLE && channel != PSP_NET_CH_UNRELIABLE) return PSP_NET_ERR;
    if (len > PSP_NET_MAX_PAYLOAD) return PSP_NET_ERR_TOO_BIG;

    uint8_t *b = e->buf;
    b[0] = MSG_DATA;
    memcpy(b + 1, e->self_mac, 6);
    put_u64(b + 7, e->self_id);
    /* a client addressing the host by id: the host's MAC (from WELCOME) */
    const uint8_t *dst = !to ? BROADCAST
                       : (!e->is_host && to->id && to->id == e->slots[0].id) ? e->slots[0].mac : to->mac;
    memcpy(b + 15, dst, 6);
    b[21] = (uint8_t)port; b[22] = (uint8_t)(port >> 8);
    if (len) memcpy(b + DATA_HDR, data, len);
    const size_t n = DATA_HDR + len;

    int rc = PSP_NET_OK;
    if (!e->is_host) {                                   /* everything goes through the host */
        if (!e->slots[0].welcomed) return PSP_NET_ERR_NO_PEER;
        rc = raw_send(e, e->slots[0].peer, channel, b, n);
    } else if (!to || memcmp(to->mac, BROADCAST, 6) == 0) {
        for (int i = 0; i < e->max_slots; i++)
            if (e->slots[i].peer && e->slots[i].welcomed && raw_send(e, e->slots[i].peer, channel, b, n) != 0) rc = PSP_NET_ERR;
    } else {
        slot *s = slot_by_dest(e, to);
        if (!s) return PSP_NET_ERR_NO_PEER;
        rc = raw_send(e, s->peer, channel, b, n);
    }
    enet_host_flush(e->host);
    return rc;
}

/* Returns 1 when a packet was delivered to fn. */
static int handle_event(psp_net_enet *e, ENetEvent *ev, psp_net_packet_fn fn, void *user) {
    int delivered = 0;
    switch (ev->type) {
    case ENET_EVENT_TYPE_CONNECT:
        if (e->is_host) {
            slot *s = slot_by_peer(e, NULL);              /* a free slot */
            if (!s) { enet_peer_disconnect_now(ev->peer, 0); break; }
            memset(s, 0, sizeof *s);
            s->peer = ev->peer;                           /* welcomed after its HELLO */
        } else {
            uint8_t hello[HELLO_LEN];
            hello[0] = MSG_HELLO; memcpy(hello + 1, e->self_mac, 6);
            raw_send(e, ev->peer, PSP_NET_CH_RELIABLE, hello, sizeof hello);
            enet_host_flush(e->host);
        }
        break;
    case ENET_EVENT_TYPE_DISCONNECT: {
        slot *s = slot_by_peer(e, ev->peer);
        if (s) {
            elog(e, "enet: peer %llu left", (unsigned long long)s->id);
            memset(s, 0, sizeof *s);
        }
        break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
        const uint8_t *d = ev->packet->data;
        const size_t n = ev->packet->dataLength;
        slot *s = slot_by_peer(e, ev->peer);
        if (!s || n < 1) break;
        if (e->is_host && d[0] == MSG_HELLO && n >= HELLO_LEN && !s->welcomed) {
            memcpy(s->mac, d + 1, 6);
            s->id = e->next_id++;
            s->welcomed = 1;
            uint8_t w[WELCOME_LEN];
            w[0] = MSG_WELCOME; put_u64(w + 1, s->id); memcpy(w + 9, e->self_mac, 6); put_u64(w + 15, e->self_id);
            raw_send(e, s->peer, PSP_NET_CH_RELIABLE, w, sizeof w);
            enet_host_flush(e->host);
            elog(e, "enet: peer %02X:%02X:%02X:%02X:%02X:%02X joined as id %llu",
                 s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5], (unsigned long long)s->id);
        } else if (!e->is_host && d[0] == MSG_WELCOME && n >= WELCOME_LEN) {
            e->self_id = get_u64(d + 1);
            memcpy(s->mac, d + 9, 6);
            s->id = get_u64(d + 15);
            s->welcomed = 1;
        } else if (d[0] == MSG_DATA && n >= DATA_HDR && s->welcomed) {
            psp_net_packet pk;
            if (e->is_host) {                              /* the sender is who the connection says */
                memcpy(pk.from.mac, s->mac, 6); pk.from.id = s->id;
            } else {
                memcpy(pk.from.mac, d + 1, 6); pk.from.id = get_u64(d + 7);
            }
            const uint8_t *dst = d + 15;
            pk.channel = ev->channelID;
            pk.port = (uint16_t)(d[21] | (d[22] << 8));
            pk.len = (uint32_t)(n - DATA_HDR);
            pk.data = d + DATA_HDR;
            const int bcast = memcmp(dst, BROADCAST, 6) == 0;
            if (e->is_host && !(memcmp(dst, e->self_mac, 6) == 0)) {
                /* relay: re-stamp the source, forward on the same channel */
                uint8_t *b = e->buf;
                memcpy(b, d, n);
                memcpy(b + 1, s->mac, 6); put_u64(b + 7, s->id);
                for (int i = 0; i < e->max_slots; i++) {
                    slot *o = &e->slots[i];
                    if (!o->peer || !o->welcomed || o == s) continue;
                    if (bcast || memcmp(o->mac, dst, 6) == 0) raw_send(e, o->peer, ev->channelID, b, n);
                }
                enet_host_flush(e->host);
            }
            if (bcast || memcmp(dst, e->self_mac, 6) == 0) {
                if (fn) fn(&pk, user);
                delivered = 1;
            }
        }
        enet_packet_destroy(ev->packet);
        break;
    }
    default: break;
    }
    return delivered;
}

static int t_poll_packets(void *ctx, psp_net_packet_fn fn, void *user) {
    psp_net_enet *e = ctx;
    if (!e->host) return PSP_NET_ERR_NOT_INIT;
    int count = 0;
    ENetEvent ev;
    while (enet_host_service(e->host, &ev, 0) > 0) count += handle_event(e, &ev, fn, user);
    return count;
}

/* ---- public ---------------------------------------------------------------------------- */

psp_net_enet *psp_net_enet_create(const psp_net_enet_config *cfg) {
    psp_net_enet *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (cfg) e->cfg = *cfg;
    e->vt.name = "enet";
    e->vt.ctx = e;
    e->vt.initialize = t_initialize;
    e->vt.host_session = t_host_session;
    e->vt.join_session = t_join_session;
    e->vt.send_packet = t_send_packet;
    e->vt.poll_packets = t_poll_packets;
    e->vt.shutdown = t_shutdown;
    return e;
}

void psp_net_enet_destroy(psp_net_enet *e) {
    if (!e) return;
    t_shutdown(e);
    free(e);
}

const psp_net_transport *psp_net_enet_transport(psp_net_enet *e) { return &e->vt; }

uint16_t psp_net_enet_port(const psp_net_enet *e) {
    if (!e->host) return 0;
    ENetAddress a;
    if (enet_socket_get_address(e->host->socket, &a) != 0) return 0;
    return a.port;
}

uint64_t psp_net_enet_self_id(const psp_net_enet *e) { return e->self_id; }

int psp_net_enet_peer_count(const psp_net_enet *e) {
    int n = 0;
    for (int i = 0; i < e->max_slots; i++) n += e->slots[i].peer && e->slots[i].welcomed;
    return n;
}
