/* psprecomp — a psp_net_transport over ENet (UDP). MMO build only (ENABLE_MMO_NET).
 *
 * Star topology: one instance hosts (host_session), the others join it
 * (join_session "ip:port"). The host assigns each peer a 64-bit id after a
 * HELLO/WELCOME exchange and relays packets between clients, so peers only
 * ever address each other by the game's MAC. The host stamps the sender of
 * every relayed packet itself: a client cannot claim another MAC or id.
 *
 * Channels: PSP_NET_CH_RELIABLE -> ENet reliable sequenced,
 *           PSP_NET_CH_UNRELIABLE -> ENet unreliable sequenced.
 *
 * Instances are independent (several may live in one process); each is used
 * from one thread at a time. */
#ifndef PSPRECOMP_NET_ENET_H
#define PSPRECOMP_NET_ENET_H

#include "psprecomp/net_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct psp_net_enet psp_net_enet;

typedef struct {
    const char *bind_host;           /* host_session: address to bind (NULL = all interfaces) */
    uint16_t    port;                /* host_session: port to bind (0 = any free port) */
    uint32_t    connect_timeout_ms;  /* join_session: how long to wait for WELCOME (0 = 3000) */
    void      (*log)(const char *line);  /* optional */
    /* optional: called while join_session waits, e.g. to service a host that
     * lives in the same thread (a LAN game hosting and playing in one process) */
    void      (*idle)(void *user);
    void       *idle_user;

    /* Host only, all optional (a dedicated server's policy hooks): */
    /* a peer was welcomed (joined = 1, after its HELLO) or left (joined = 0) */
    void      (*on_peer)(void *user, uint64_t id, const uint8_t mac[6], int joined);
    /* every data packet a peer sends, before delivery or relay; return 0 to
     * drop it (rate limiting) */
    int       (*admit)(void *user, uint64_t id, uint32_t len);
    void       *host_user;
    /* relay only between peers marked with psp_net_enet_authorize (packets
     * from the others still reach the host itself, e.g. their handshake) */
    int         relay_authorized_only;
} psp_net_enet_config;

psp_net_enet *psp_net_enet_create(const psp_net_enet_config *cfg);   /* NULL cfg = defaults */
void          psp_net_enet_destroy(psp_net_enet *e);
/* The transport view of e; its ctx is e. Valid until destroy. */
const psp_net_transport *psp_net_enet_transport(psp_net_enet *e);

/* After host_session: the bound port. */
uint16_t psp_net_enet_port(const psp_net_enet *e);
/* This instance's id (host = 1; a client's after join_session; 0 = none yet). */
uint64_t psp_net_enet_self_id(const psp_net_enet *e);
/* Connected, welcomed peers (host: clients; client: 1 once joined). */
int      psp_net_enet_peer_count(const psp_net_enet *e);

/* Wait up to timeout_ms for network traffic and handle it (relays included),
 * delivering packets to fn: a server sleeps here between ticks instead of
 * in a plain sleep, so it relays the moment a packet arrives. */
int      psp_net_enet_service(psp_net_enet *e, uint32_t timeout_ms, psp_net_packet_fn fn, void *user);
/* Host: let a peer's packets be relayed (relay_authorized_only). */
int      psp_net_enet_authorize(psp_net_enet *e, uint64_t id, int on);
/* Host: disconnect a peer; `reason` reaches the client (PSP_MMO_DISC_*). */
int      psp_net_enet_disconnect(psp_net_enet *e, uint64_t id, uint32_t reason);
/* Host: disconnect every peer at once (shutdown): anything still queued for
 * them is dropped, so the notice goes out now. */
void     psp_net_enet_disconnect_all(psp_net_enet *e, uint32_t reason);
/* Host: a peer's address as "ip:port". 0 on success. */
int      psp_net_enet_peer_address(const psp_net_enet *e, uint64_t id, char *out, size_t cap);
/* Host: a peer's round-trip time in ms (ENet's smoothed estimate); -1 if unknown. */
int      psp_net_enet_peer_rtt(const psp_net_enet *e, uint64_t id);
/* Client: the reason the host gave when it disconnected us (0 = none / still joined). */
uint32_t psp_net_enet_disconnect_reason(const psp_net_enet *e);

#ifdef __cplusplus
}
#endif
#endif
