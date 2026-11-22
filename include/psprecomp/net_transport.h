/* psprecomp — the multiplayer transport seam (MMO build only, ENABLE_MMO_NET).
 *
 * The PSP's ad hoc libraries (sceNetAdhoc, Adhocctl, AdhocMatching) talk to
 * peers by 6-byte MAC address. In the MMO build those calls are routed through
 * one psp_net_transport, supplied by the host program the same way
 * psp_http_transport is: the HLE layer never touches a socket for ad hoc
 * traffic itself. Without a transport installed (and always in the
 * single-player build) the null transport answers, and every session request
 * fails exactly as offline.c's stubs do.
 *
 * Peers are named by psp_net_peer: the game's MAC plus a 64-bit id a server
 * assigns (session/account). Channels follow the roadmap:
 *   PSP_NET_CH_RELIABLE   login, chat, trades, mission triggers, drop claims
 *   PSP_NET_CH_UNRELIABLE position / rotation / animation state, sequenced */
#ifndef PSPRECOMP_NET_TRANSPORT_H
#define PSPRECOMP_NET_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PSP_NET_CH_RELIABLE = 0, PSP_NET_CH_UNRELIABLE = 1, PSP_NET_CH_COUNT = 2 };

/* Results. 0 is success; failures are negative so a transport can also hand
 * back a count (poll). */
enum {
    PSP_NET_OK           = 0,
    PSP_NET_ERR          = -1,   /* generic failure */
    PSP_NET_ERR_NOT_INIT = -2,
    PSP_NET_ERR_NO_PEER  = -3,
    PSP_NET_ERR_TOO_BIG  = -4,
    PSP_NET_ERR_UNSUPPORTED = -5 /* the null transport */
};

#define PSP_NET_MAX_PAYLOAD 2048u   /* PDP datagrams top out well below this */

typedef struct {
    uint8_t  mac[6];       /* SceNetEtherAddr as the game sees it */
    uint64_t id;           /* server-assigned session/account id; 0 = unknown */
} psp_net_peer;

typedef struct {
    psp_net_peer from;
    uint8_t      channel;  /* PSP_NET_CH_* */
    uint16_t     port;     /* the PDP/PTP port the game addressed */
    uint32_t     len;
    const uint8_t *data;   /* valid only during the callback */
} psp_net_packet;

typedef void (*psp_net_packet_fn)(const psp_net_packet *pkt, void *user);

typedef struct {
    const char *name;
    /* Bring the transport up; self = this machine's game MAC. */
    int  (*initialize)(const uint8_t self_mac[6]);
    /* Host a session (group name as the game gives it, max players). */
    int  (*host_session)(const char *group, int max_players);
    /* Join a session at address ("host:port"), or by group name on LAN. */
    int  (*join_session)(const char *address, const char *group);
    /* Send to one peer (or every peer: to == NULL). */
    int  (*send_packet)(const psp_net_peer *to, int channel, uint16_t port,
                        const void *data, uint32_t len);
    /* Deliver pending packets to fn; returns how many, or a PSP_NET_ERR_*. */
    int  (*poll_packets)(psp_net_packet_fn fn, void *user);
    void (*shutdown)(void);
} psp_net_transport;

/* Install a transport (NULL = the null transport). The pointer must outlive use. */
void psp_net_set_transport(const psp_net_transport *t);
/* The current transport; never NULL. */
const psp_net_transport *psp_net_get_transport(void);
/* The always-failing default. */
const psp_net_transport *psp_net_null_transport(void);

#ifdef __cplusplus
}
#endif
#endif
