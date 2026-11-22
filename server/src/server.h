/* The PSP2i dedicated server core: sessions over the ENet transport, the
 * control protocol (net_proto.h), handshake checks, rate limiting and LAN
 * discovery. No rendering, audio, windowing or game runtime.
 *
 * Driven by srv_tick() at a fixed rate (main.c: 30 Hz). Single-threaded. */
#ifndef PSP2I_SERVER_H
#define PSP2I_SERVER_H

#include <stdint.h>

#include "loot.h"

typedef struct db db;

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
    /* loot (Task 2.3), all optional */
    db         *database;             /* drops are recorded here (else numbered in memory) */
    const char *drop_tables_path;     /* JSON; reloaded when its contents change */
    const char *rates_path;           /* TOML; reloaded when its contents change */
    uint32_t    kills_per_sec;        /* per player (burst 2x; default 10) */
    uint64_t    rng_seed;             /* 0 = from the OS */
} srv_config;

typedef struct {
    int      players;                 /* sessions past the handshake */
    int      pending;                 /* connected, no handshake yet */
    uint64_t handshakes_ok, handshakes_rejected;
    uint64_t kicked_timeout, kicked_rate, kicked_protocol;
    uint64_t packets_in, packets_dropped;
    uint64_t lan_replies;
    uint64_t kills, kills_dropped, drops, rare_drops, reloads, reload_errors;
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

/* Loot: check both config files now (otherwise once a second). */
void       srv_reload(srv *s);
void       srv_reload_drops(srv *s);   /* only the drop tables: live rates stay */
void       srv_reload_rates(srv *s);   /* back to rates.toml */
loot_rates srv_rates(const srv *s);
/* Live, in memory (the admin tools); -1 for an unknown name or bad value. */
int        srv_set_rate(srv *s, const char *name, double value);
int        srv_enemy_count(const srv *s);

/* ---- admin (Task 2.4) ---- */
typedef struct {
    uint64_t session;
    char     name[33];
    char     mac[18];
    char     ip[64];                  /* "ip:port" */
    int      ping_ms;                 /* -1 unknown */
    uint64_t joined_ms;               /* srv_tick time of the handshake */
    int64_t  account_id;              /* 0 until accounts log in through the game */
    char     location[32];            /* lobby / mission: "-" until the game reports it */
} srv_player;
int  srv_players(const srv *s, srv_player *out, int max);
/* Disconnect a player (PSP_MMO_DISC_*); 0, or -1 if no such session. */
int  srv_kick(srv *s, uint64_t session, uint32_t reason);
/* Ban the player's IP (minutes 0 = permanent) and disconnect them (BANNED).
 * Needs the database. The banned IP is written to ip_out (cap >= 64). */
int  srv_ban_session(srv *s, uint64_t session, const char *reason, int64_t minutes, const char *admin, char *ip_out);
/* Drop every connected peer from this IP (after a ban by IP). */
int  srv_kick_ip(srv *s, const char *ip, uint32_t reason);
/* MSG_S2C_SYSTEM_NOTICE to every player. */
int  srv_announce(srv *s, const char *text, uint32_t rgb);
/* Seasonal events / lobby themes: a fixed list; changes reach every player
 * (MSG_S2C_SERVER_EVENTS), and new players on join. */
#define SRV_EVENT_COUNT 6
const char *srv_event_name(int i);     /* NULL past the end */
int  srv_set_event(srv *s, const char *name, int on);   /* -1 unknown */
uint32_t srv_events(const srv *s);
int  srv_find_ip(const srv *s, uint64_t session, char *ip, size_t cap);   /* bare IP */
db  *srv_db(const srv *s);
const char *srv_drop_tables_path(const srv *s);
const char *srv_name(const srv *s);
int  srv_max_players(const srv *s);
int  srv_mode_online(const srv *s);

/* The handshake check, on its own (tests): PSP_MMO_OK or PSP_MMO_ERR_*.
 * On PSP_MMO_OK, name (33 bytes) and mac are filled in. */
uint32_t srv_check_handshake(const uint8_t *msg, uint32_t len, char *name, uint8_t mac[6]);

#endif
