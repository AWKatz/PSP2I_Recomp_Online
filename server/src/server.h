/* The PSP2i dedicated server core: sessions over the ENet transport, the
 * control protocol (net_proto.h), handshake checks, rate limiting and LAN
 * discovery. No rendering, audio, windowing or game runtime.
 *
 * Driven by srv_tick() at a fixed rate (main.c: 30 Hz). Single-threaded. */
#ifndef PSP2I_SERVER_H
#define PSP2I_SERVER_H

#include <stdint.h>

typedef enum { SRV_MODE_LAN, SRV_MODE_ONLINE } srv_mode;

typedef struct {
    srv_mode    mode;
    const char *bind;                 /* NULL = all interfaces (0.0.0.0) */
    uint16_t    port;                 /* the game port (ENet); 0 = any free port */
    uint16_t    lan_port;             /* LAN mode: discovery (UDP); 0 = any free port */
    const char *name;                 /* shown to LAN clients */
    int         max_players;          /* 1..63 */
    uint32_t    rate_pps;             /* online: packets per second per player (burst 2x) */
    uint32_t    handshake_timeout_ms; /* a peer must send a valid handshake within this */
    void      (*log)(const char *line);
} srv_config;

typedef struct {
    int      players;                 /* sessions past the handshake */
    int      pending;                 /* connected, no handshake yet */
    uint64_t handshakes_ok, handshakes_rejected;
    uint64_t kicked_timeout, kicked_rate, kicked_protocol;
    uint64_t packets_in, packets_dropped;
    uint64_t lan_replies;
} srv_stats;

typedef struct srv srv;

/* Applies the defaults for zero fields. */
srv *srv_create(const srv_config *cfg);
/* Bind the game port (and the discovery port in LAN mode). 0 on success. */
int  srv_start(srv *s);
/* One tick: network in, session upkeep, LAN replies. `now_ms` is monotonic. */
void srv_tick(srv *s, uint64_t now_ms);
/* Disconnect everyone (reason SHUTDOWN) and close the sockets. */
void srv_stop(srv *s);
void srv_destroy(srv *s);

void     srv_get_stats(const srv *s, srv_stats *out);
uint16_t srv_port(const srv *s);
uint16_t srv_lan_port(const srv *s);    /* 0 outside LAN mode */
uint32_t srv_version_hash(void);

/* The handshake check, on its own (tests): PSP_MMO_OK or PSP_MMO_ERR_*.
 * On PSP_MMO_OK, name (33 bytes) and mac are filled in. */
uint32_t srv_check_handshake(const uint8_t *msg, uint32_t len, char *name, uint8_t mac[6]);

#endif
