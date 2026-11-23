/* psprecomp — shared between the MMO ad hoc files (adhoc_mmo.c, adhoc_matching.c). */
#ifndef PSPRECOMP_ADHOC_INTERNAL_H
#define PSPRECOMP_ADHOC_INTERNAL_H

#include "psprecomp/net_transport.h"

int  mmo_net_ready(void);                         /* joined the server: the transport can send */

/* The party directory (adhoc_mmo.c): our role in the game's party, sent to
 * the server when it changes; the join target (steering). */
void mmo_party_report(int role, int members, int max, int open, const uint8_t leader_mac[6]);
/* Matching stopped: keep the party role (closed) while ad hoc control is up. */
void mmo_party_keep(void);
int  mmo_join_target_allows(const uint8_t mac[6]);   /* a HELLO from this leader may reach the game */

/* sceNetAdhocMatching (adhoc_matching.c) */
void mmo_matching_register(void);
int  mmo_matching_port(void);                     /* its port, or -1 when there is no matching context */
void mmo_matching_on_packet(const psp_net_packet *p);
void mmo_matching_pump(void);                     /* once a frame: HELLOs, events */
void mmo_matching_status(int *mode, int *running, int *established, int *target_seen);

/* sceNetAdhoc PDP/PTP sockets (adhoc_sockets.c) */
void mmo_sockets_register(void);
void mmo_sockets_on_packet(const psp_net_packet *p);
void mmo_net_poll(void);                          /* read the network now (blocking calls wait with it) */

/* PSN sign-in without PSN (np_mmo.c) */
void mmo_np_register(void);
void mmo_np_set_name(const char *name);
void mmo_np_pump(void);                           /* once a frame: ticket callbacks */

#endif
