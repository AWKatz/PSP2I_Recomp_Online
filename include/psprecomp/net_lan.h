/* psprecomp — LAN discovery for the PSP2i MMO (client and dedicated server).
 *
 * A client broadcasts MSG_LAN_PROBE (UDP, PSP_MMO_LAN_PORT); every server in
 * --lan mode answers with MSG_LAN_REPLY: its game port, player counts,
 * version hash and name. Plain UDP sockets, no ENet. */
#ifndef PSPRECOMP_NET_LAN_H
#define PSPRECOMP_NET_LAN_H

#include <stddef.h>
#include <stdint.h>

#include "psprecomp/net_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[PSP_MMO_NAME_LEN + 1];
    uint16_t game_port;
    uint16_t players, max_players;
    uint32_t version_hash;
} psp_mmo_lan_info;

/* Server side. */
typedef struct psp_mmo_lan_responder psp_mmo_lan_responder;
/* Bind 0.0.0.0:port (0 = any free port). NULL on failure. */
psp_mmo_lan_responder *psp_mmo_lan_responder_open(uint16_t port);
uint16_t psp_mmo_lan_responder_port(const psp_mmo_lan_responder *r);
/* Answer every probe waiting (non-blocking). Returns how many were answered. */
int      psp_mmo_lan_responder_poll(psp_mmo_lan_responder *r, const psp_mmo_lan_info *info);
void     psp_mmo_lan_responder_close(psp_mmo_lan_responder *r);

/* Client side: broadcast a probe to `port` (and to 127.0.0.1, for a server on
 * this machine) and wait up to timeout_ms for the first reply. On success
 * fills *out and the server's IP (ip_cap >= 16) and returns 0; -1 if none. */
int psp_mmo_lan_discover(uint16_t port, uint32_t timeout_ms, psp_mmo_lan_info *out, char *ip, size_t ip_cap);

#ifdef __cplusplus
}
#endif
#endif
