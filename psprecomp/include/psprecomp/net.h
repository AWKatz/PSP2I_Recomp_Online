/* psprecomp — networking HLE: the PSP's WLAN, network and HTTP libraries
 * answered from the host (src/hle/net.c, net_sock.c, http.c). PSN and ad hoc
 * are not provided (src/hle/offline.c).
 *
 * The HLE side is platform neutral. An HTTP transport (psp_http_set_transport),
 * supplied by the host program, says how a game's HTTP requests are answered;
 * without one they fail with the firmware's error code. */
#ifndef PSPRECOMP_NET_H
#define PSPRECOMP_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the host connection ---------------------------------------------------- */

typedef struct {
    int      connected;    /* an interface up, not loopback, with IPv4 and a gateway */
    int      wireless;
    uint32_t ipv4, netmask, gateway, dns1, dns2;   /* host byte order */
} psp_net_host;

/* Fills *out (may be NULL); returns connected. PSPRECOMP_NET=off forces 0. */
int  psp_net_host_info(psp_net_host *out);
/* Nonzero: the host looks disconnected (the WLAN switch reads off), as with
 * PSPRECOMP_NET=off. */
void psp_net_set_forced_off(int off);
void psp_net_ether_addr(uint8_t mac[6]);
/* Every network event line (resolver lookups, the network dialog), for a log file. */
void psp_net_set_log(void (*fn)(const char *line));
/* Write a line to that log (and stderr). */
void psp_net_log(const char *fmt, ...);
/* Name redirection for the resolver: fn returns the host name to look up
 * instead of `host`, or NULL to look up `host` itself (a game's own servers
 * pointed at a replacement). */
void psp_net_set_resolve_redirect(const char *(*fn)(const char *host));

/* ---- HTTP ---------------------------------------------------------------------- */

typedef struct {
    const char *method, *scheme, *host, *path, *user_agent;
    uint32_t    port;
    const uint8_t *body;
    uint32_t    body_len;
} psp_http_request;

typedef struct {
    int      status;       /* HTTP status code */
    uint8_t *body;         /* malloc'd by the transport, freed by http.c */
    uint32_t len;
} psp_http_response;

typedef struct {
    /* Perform the request. 0 = a response is in *resp; otherwise an
     * SCE_HTTP_ERROR_* code for the game (e.g. 0x80431063 network). */
    int  (*send)(const psp_http_request *req, psp_http_response *resp);
    void (*log)(const char *line);
} psp_http_transport;

void psp_http_set_transport(const psp_http_transport *t);

void psp_net_register(void);
void psp_http_register(void);
/* PSN, NP matching and ad hoc are not provided: offline.c answers their
 * imports as unavailable. */
void psp_offline_register(void);
/* offline.c's stub table, for layers that replace it (MMO build). Returns 0 past the end. */
int  psp_offline_stub(int i, uint32_t *nid, const char **lib, const char **name, int *succeeds);

/* ---- MMO build (ENABLE_MMO_NET) ---------------------------------------------------- */
/* Nonzero: the WLAN switch always reads on, whatever the host's connection. */
void psp_net_set_wlan_forced_on(int on);
/* The network dialog's ad hoc actions (2 connect, 4 create, 5 join): fn
 * returns the dialog's result (0 = connected). Without one they fail. */
void psp_net_set_adhoc_dialog(int (*fn)(int action, uint32_t param));

#ifdef PSPRECOMP_MMO_NET
typedef struct {
    const char *server;       /* "ip:port" the ad hoc layer joins */
    const char *player_name;  /* sent in the handshake */
    const char *version;      /* client build/version string, sent in the handshake */
} psp_net_mmo_config;
/* Copied. Call before the game starts. */
void psp_net_mmo_configure(const psp_net_mmo_config *cfg);
void psp_net_mmo_register(void);
/* Service the transport once (call every frame from the host). */
void psp_net_mmo_pump(void);
/* Join the server now (otherwise the game's sceNetAdhocctlInit does). 0 = joined. */
int  psp_net_mmo_connect(void);
int  psp_net_mmo_connected(void);

/* The party directory (Multi Mode): the parties leaders are hosting on the server. */
typedef struct {
    uint8_t leader_mac[6];
    char    leader_name[33];
    int     members, max, open;   /* members with the leader; open = taking members */
} psp_net_mmo_party;
/* Ask the server for the list; the answer arrives a few frames later. */
void psp_net_mmo_request_parties(void);
/* The last list received: up to `max` entries, the players online, and a
 * counter that changes with every list received (0 = none yet). */
int  psp_net_mmo_parties(psp_net_mmo_party *out, int max, int *players, unsigned *generation);
/* Steering: with a target, the game's Join Party search only sees that
 * leader's party (other leaders' HELLOs are dropped). NULL = see every party. */
void psp_net_mmo_set_join_target(const uint8_t mac[6]);
int  psp_net_mmo_join_target(uint8_t mac[6]);      /* 1 when one is set */
/* This client's role in the game's party (PSP_MMO_PARTY_*: 0 none, 1 leader, 2 member). */
int  psp_net_mmo_party_role(void);

/* Where the game is in Multi Mode, for the host's party automation. */
typedef struct {
    int adhocctl;          /* the game entered Multi Mode (sceNetAdhocctlInit) */
    int matching_mode;     /* 0 none, 1 leader (Form Party), 2 member (Join Party) */
    int matching_running;
    int established;       /* matching peers established with us */
    int target_seen;       /* a member: the join target's HELLO reached the game */
} psp_net_mmo_status;
void psp_net_mmo_get_status(psp_net_mmo_status *out);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_NET_H */
