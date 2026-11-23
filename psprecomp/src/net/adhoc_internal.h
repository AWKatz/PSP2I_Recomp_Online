/* psprecomp — shared between the MMO ad hoc files (adhoc_mmo.c, adhoc_matching.c). */
#ifndef PSPRECOMP_ADHOC_INTERNAL_H
#define PSPRECOMP_ADHOC_INTERNAL_H

#include "psprecomp/net_transport.h"

int  mmo_net_ready(void);                         /* joined the server: the transport can send */

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

#endif
