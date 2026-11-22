/* The PSP2i dedicated server core (see server.h). */
#include "server.h"

#include "clock.h"
#include "crypto.h"
#include "db.h"

#include <psprecomp/net_enet.h>
#include <psprecomp/net_lan.h>
#include <psprecomp/net_proto.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SESSIONS 64

typedef enum { SS_FREE, SS_PENDING, SS_ACTIVE } ss_state;

typedef struct {
    ss_state state;
    uint64_t id;                      /* the transport's peer id */
    uint8_t  mac[6];
    char     name[PSP_MMO_NAME_LEN + 1];
    uint64_t joined_ms;
    /* rate limit: token bucket, refilled in srv_tick */
    double   tokens;
    uint32_t dropped_this_second;
    int      bad_seconds;             /* consecutive seconds with drops */
    uint32_t kick;                    /* PSP_MMO_DISC_* to apply after this poll (0 = none) */
    double   kill_tokens;             /* enemy kill reports: token bucket */
} session;

struct srv {
    srv_config cfg;
    char       name[PSP_MMO_NAME_LEN + 1];
    psp_net_enet *enet;
    psp_mmo_lan_responder *lan;
    session    sess[MAX_SESSIONS];
    srv_stats  st;
    uint64_t   now_ms, last_refill_ms, second_start_ms;
    int        started;
    /* loot */
    loot_table *loot;
    loot_rates  rates;
    loot_rng    rng;
    uint64_t    drops_sig, rates_sig;  /* content signatures of the config files (0 = missing) */
    uint64_t    next_drop_id;
    uint32_t    events;
};

static void slog(srv *s, const char *fmt, ...) {
    if (!s->cfg.log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->cfg.log(line);
}

uint32_t srv_version_hash(void) { return psp_mmo_fnv1a(PSP_MMO_VERSION_STRING); }

static session *find(srv *s, uint64_t id) {
    for (int i = 0; i < MAX_SESSIONS; i++) if (s->sess[i].state != SS_FREE && s->sess[i].id == id) return &s->sess[i];
    return NULL;
}

static int count(const srv *s, ss_state st) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) n += s->sess[i].state == st;
    return n;
}

/* ---- handshake ---------------------------------------------------------------- */

/* Printable UTF-8, no control characters. */
static int name_ok(const uint8_t *p, size_t n) {
    if (n == 0) return 0;
    for (size_t i = 0; i < n;) {
        const uint8_t c = p[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (c < 0x80) { i++; continue; }
        const int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + (size_t)len > n || (len == 2 && c < 0xC2)) return 0;
        for (int k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)len;
    }
    return 1;
}

uint32_t srv_check_handshake(const uint8_t *msg, uint32_t len, char *name, uint8_t mac[6]) {
    if (len != PSP_MMO_HANDSHAKE_LEN || psp_mmo_type(msg, len) != MSG_C2S_HANDSHAKE) return PSP_MMO_ERR_VERSION;
    const uint8_t *m = msg + PSP_MMO_HDR_LEN;
    const uint8_t *nm = m + 6, *ver = nm + PSP_MMO_NAME_LEN;
    const uint8_t *nul = memchr(nm, 0, PSP_MMO_NAME_LEN);
    if (!nul || !name_ok(nm, (size_t)(nul - nm))) return PSP_MMO_ERR_NAME;
    if (!memchr(ver, 0, PSP_MMO_VERSION_LEN)) return PSP_MMO_ERR_VERSION;
    static const uint8_t zero[6] = { 0 };
    if (!memcmp(m, zero, 6) || (m[0] & 1)) return PSP_MMO_ERR_MAC;     /* zero, or broadcast/multicast */
    memcpy(mac, m, 6);
    memcpy(name, nm, (size_t)(nul - nm));
    name[nul - nm] = 0;
    return PSP_MMO_OK;
}

static void send_ack(srv *s, uint64_t id, uint32_t status) {
    uint8_t ack[PSP_MMO_HANDSHAKE_ACK_LEN];
    psp_mmo_put_hdr(ack, MSG_S2C_HANDSHAKE_ACK);
    psp_mmo_put_u32(ack + PSP_MMO_HDR_LEN, status);
    for (int i = 0; i < 8; i++) ack[PSP_MMO_HDR_LEN + 4 + i] = (uint8_t)((status ? 0 : id) >> (8 * i));
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, ack, sizeof ack);
}

static void send_events(srv *s, uint64_t id) {
    uint8_t m[PSP_MMO_EVENTS_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_SERVER_EVENTS);
    psp_mmo_put_u32(m + 8, s->events);
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    const psp_net_peer to = { { 0 }, id };
    t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m);
}

static void on_handshake(srv *s, session *ss, const psp_net_packet *p) {
    char name[PSP_MMO_NAME_LEN + 1];
    uint8_t mac[6];
    uint32_t st = srv_check_handshake(p->data, p->len, name, mac);
    if (st == PSP_MMO_OK) {
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (s->sess[i].state == SS_ACTIVE && !memcmp(s->sess[i].mac, mac, 6)) { st = PSP_MMO_ERR_DUPLICATE; break; }
    }
    if (st == PSP_MMO_OK && count(s, SS_ACTIVE) >= s->cfg.max_players) st = PSP_MMO_ERR_FULL;
    char addr[64] = "?", ip[64] = "", why[128] = "";
    psp_net_enet_peer_address(s->enet, ss->id, addr, sizeof addr);
    srv_find_ip(s, ss->id, ip, sizeof ip);
    if (st == PSP_MMO_OK && s->cfg.database && db_ip_banned(s->cfg.database, ip, why, sizeof why)) {
        send_ack(s, ss->id, PSP_MMO_ERR_BANNED);
        ss->kick = PSP_MMO_DISC_BANNED;
        s->st.handshakes_rejected++;
        slog(s, "handshake from peer %llu (%s) refused: banned (%s)", (unsigned long long)ss->id, addr, why);
        return;
    }
    if (st != PSP_MMO_OK) {
        send_ack(s, ss->id, st);
        ss->kick = PSP_MMO_DISC_HANDSHAKE;
        s->st.handshakes_rejected++;
        slog(s, "handshake from peer %llu (%s) rejected: status %u", (unsigned long long)ss->id, addr, (unsigned)st);
        return;
    }
    memcpy(ss->mac, mac, 6);
    memcpy(ss->name, name, sizeof name);
    ss->state = SS_ACTIVE;
    ss->joined_ms = s->now_ms;
    psp_net_enet_authorize(s->enet, ss->id, 1);
    send_ack(s, ss->id, PSP_MMO_OK);
    send_events(s, ss->id);
    s->st.handshakes_ok++;
    slog(s, "player '%s' joined: session %llu, mac %02X:%02X:%02X:%02X:%02X:%02X, %s (%d/%d)", name,
         (unsigned long long)ss->id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], addr, count(s, SS_ACTIVE), s->cfg.max_players);
}

/* ---- transport hooks -------------------------------------------------------------- */

static void on_peer(void *u, uint64_t id, const uint8_t mac[6], int joined) {
    srv *s = u;
    (void)mac;
    if (joined) {
        session *ss = NULL;
        for (int i = 0; i < MAX_SESSIONS && !ss; i++) if (s->sess[i].state == SS_FREE) ss = &s->sess[i];
        if (!ss) return;                              /* cannot happen: the transport caps peers */
        memset(ss, 0, sizeof *ss);
        ss->state = SS_PENDING;
        ss->id = id;
        ss->joined_ms = s->now_ms;
        ss->tokens = 2.0 * s->cfg.rate_pps;
        ss->kill_tokens = 2.0 * s->cfg.kills_per_sec;
        return;
    }
    session *ss = find(s, id);
    if (!ss) return;
    if (ss->state == SS_ACTIVE) slog(s, "player '%s' left (session %llu)", ss->name, (unsigned long long)id);
    memset(ss, 0, sizeof *ss);
}

static int admit(void *u, uint64_t id, uint32_t len) {
    srv *s = u;
    (void)len;
    s->st.packets_in++;
    session *ss = find(s, id);
    if (!ss || ss->kick) { s->st.packets_dropped++; return 0; }
    if (s->cfg.mode != SRV_MODE_ONLINE) return 1;
    if (ss->tokens >= 1.0) { ss->tokens -= 1.0; return 1; }
    ss->dropped_this_second++;
    s->st.packets_dropped++;
    return 0;
}

/* ---- loot ---------------------------------------------------------------------------- */

static uint64_t file_sig(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 0xCBF29CE484222325ull, n = 0;   /* FNV-1a 64 of the contents, and the length */
    int c;
    while ((c = fgetc(f)) != EOF) { h = (h ^ (uint8_t)c) * 0x100000001B3ull; n++; }
    fclose(f);
    return (h ^ n) | 1;
}

enum { RELOAD_DROPS = 1, RELOAD_RATES = 2 };
static void check_configs(srv *s, int force) {
    char err[320];
    const uint64_t ds = file_sig(s->cfg.drop_tables_path);
    if (s->cfg.drop_tables_path && ((force & RELOAD_DROPS) || ds != s->drops_sig)) {
        s->drops_sig = ds;
        loot_table *t = ds ? loot_load(s->cfg.drop_tables_path, err, sizeof err) : NULL;
        if (t) {
            loot_free(s->loot);                       /* single-threaded: swap between ticks */
            s->loot = t;
            s->st.reloads++;
            slog(s, "drop tables: %s: %d enemies, %d missions (version %d)", s->cfg.drop_tables_path,
                 t->n_enemies, t->n_missions, t->version);
        } else {
            s->st.reload_errors++;
            slog(s, "drop tables: %s; %s", ds ? err : "file missing",
                 s->loot ? "keeping the tables loaded before" : "no drops until it loads");
        }
    }
    const uint64_t rs = file_sig(s->cfg.rates_path);
    if (s->cfg.rates_path && ((force & RELOAD_RATES) || rs != s->rates_sig)) {
        s->rates_sig = rs;
        loot_rates r;
        const int rc = rs ? loot_rates_load(s->cfg.rates_path, &r, err, sizeof err) : -2;
        if (rc == 0) {
            s->rates = r;
            s->st.reloads++;
            slog(s, "rates: exp %.2f, drop %.2f, rare %.2f, meseta %.2f, type points %.2f", r.exp, r.drop, r.rare, r.meseta, r.type_points);
        } else if (rc == -2) {
            slog(s, "rates: %s missing; keeping %s", s->cfg.rates_path, s->st.reloads ? "the current rates" : "1.0 for all");
        } else {
            s->st.reload_errors++;
            slog(s, "rates: %s; keeping the current rates", err);
        }
    }
}

static void put_f32(uint8_t *b, const uint8_t *src) { memcpy(b, src, 4); }   /* floats pass through as sent */

static void on_kill(srv *s, session *ss, const psp_net_packet *p) {
    if (p->len != PSP_MMO_ENEMY_KILLED_LEN) return;
    if (ss->kill_tokens < 1.0) { s->st.kills_dropped++; return; }
    ss->kill_tokens -= 1.0;
    const uint8_t *m = p->data + PSP_MMO_HDR_LEN;
    const uint32_t enemy = psp_mmo_get_u32(m);
    loot_result r;
    if (!s->loot || loot_roll_enemy(s->loot, enemy, &s->rates, &s->rng, &r) != 0) {
        s->st.kills_dropped++;
        return;
    }
    s->st.kills++;
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    uint8_t rw[PSP_MMO_KILL_REWARD_LEN];
    psp_mmo_put_hdr(rw, MSG_S2C_KILL_REWARD);
    psp_mmo_put_u32(rw + 8, enemy);
    psp_mmo_put_u32(rw + 12, r.exp);
    psp_mmo_put_u32(rw + 16, r.type_points);
    const psp_net_peer killer = { { 0 }, ss->id };
    t->send_packet(t->ctx, &killer, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, rw, sizeof rw);
    if (!r.has_drop) return;

    /* the drop: recorded (claims come later), then shown to every player */
    int64_t drop_id = 0;
    if (s->cfg.database) {
        char item[160];
        if (r.item_id) snprintf(item, sizeof item, "{\"id\":%u,\"rare\":%s}", (unsigned)r.item_id, r.rare ? "true" : "false");
        else snprintf(item, sizeof item, "{\"meseta\":%u}", (unsigned)r.meseta);
        char origin[64];
        snprintf(origin, sizeof origin, "enemy:%u", (unsigned)enemy);
        drop_id = db_drop_create(s->cfg.database, item, origin);
    }
    if (drop_id <= 0) drop_id = (int64_t)++s->next_drop_id;
    uint8_t sd[PSP_MMO_SPAWN_DROP_LEN];
    psp_mmo_put_hdr(sd, MSG_S2C_SPAWN_DROP);
    for (int i = 0; i < 8; i++) sd[8 + i] = (uint8_t)((uint64_t)drop_id >> (8 * i));
    psp_mmo_put_u32(sd + 16, enemy);
    psp_mmo_put_u32(sd + 20, r.item_id);
    psp_mmo_put_u32(sd + 24, r.meseta);
    psp_mmo_put_u32(sd + 28, r.rare ? PSP_MMO_DROP_RARE : 0);
    put_f32(sd + 32, m + 8); put_f32(sd + 36, m + 12); put_f32(sd + 40, m + 16);
    for (int i = 0; i < 8; i++) sd[44 + i] = (uint8_t)(ss->id >> (8 * i));
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sess[i].state != SS_ACTIVE) continue;
        const psp_net_peer to = { { 0 }, s->sess[i].id };
        t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, sd, sizeof sd);
    }
    s->st.drops++;
    if (r.rare) {
        s->st.rare_drops++;
        slog(s, "rare drop: '%s' (item %u) from enemy %u for '%s'", r.name, (unsigned)r.item_id, (unsigned)enemy, ss->name);
    }
}

void srv_reload(srv *s) { check_configs(s, RELOAD_DROPS | RELOAD_RATES); }
void srv_reload_drops(srv *s) { check_configs(s, RELOAD_DROPS); }
void srv_reload_rates(srv *s) { check_configs(s, RELOAD_RATES); }
loot_rates srv_rates(const srv *s) { return s->rates; }
int srv_set_rate(srv *s, const char *name, double v) {
    if (loot_rates_set(&s->rates, name, v) != 0) return -1;
    slog(s, "rates: %s set to %.2f", name, v);
    return 0;
}
int srv_enemy_count(const srv *s) { return s->loot ? s->loot->n_enemies : 0; }

static void on_packet(const psp_net_packet *p, void *u) {
    srv *s = u;
    session *ss = find(s, p->from.id);
    if (!ss || ss->kick) return;
    const uint16_t type = p->port == PSP_MMO_CTRL_PORT ? psp_mmo_type(p->data, p->len) : 0;
    if (ss->state == SS_PENDING) {
        if (type == MSG_C2S_HANDSHAKE) { on_handshake(s, ss, p); return; }
        if (s->cfg.mode == SRV_MODE_ONLINE) {         /* nothing else before the handshake */
            ss->kick = PSP_MMO_DISC_PROTOCOL;
            s->st.kicked_protocol++;
        }
        return;
    }
    /* Active players: game traffic is relayed by the transport; control
     * messages are handled here. */
    if (type == MSG_C2S_ENEMY_KILLED) on_kill(s, ss, p);
}

/* ---- lifecycle ---------------------------------------------------------------------- */

srv *srv_create(const srv_config *cfg) {
    srv *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->cfg = *cfg;
    if (s->cfg.max_players < 1) s->cfg.max_players = 32;
    if (s->cfg.max_players > SRV_MAX_PLAYERS) s->cfg.max_players = SRV_MAX_PLAYERS;   /* the hub cap */
    if (!s->cfg.rate_pps) s->cfg.rate_pps = 120;
    if (!s->cfg.handshake_timeout_ms) s->cfg.handshake_timeout_ms = 5000;
    snprintf(s->name, sizeof s->name, "%s", s->cfg.name && *s->cfg.name ? s->cfg.name : "PSP2i Server");
    s->cfg.name = s->name;
    if (!s->cfg.kills_per_sec) s->cfg.kills_per_sec = 10;
    const loot_rates def = LOOT_RATES_DEFAULT;
    s->rates = def;
    uint64_t seed = s->cfg.rng_seed;
    if (!seed && os_random(&seed, sizeof seed) != 0) seed = 0x5053503249ull ^ (uint64_t)(uintptr_t)s;
    loot_rng_seed(&s->rng, seed);
    return s;
}

int srv_start(srv *s) {
    psp_net_enet_config ec = {
        .bind_host = s->cfg.bind, .port = s->cfg.port, .log = s->cfg.log,
        .on_peer = on_peer, .admit = admit, .host_user = s, .relay_authorized_only = 1,
    };
    s->enet = psp_net_enet_create(&ec);
    if (!s->enet) return -1;
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    static const uint8_t server_mac[6] = { 0x02, 0x50, 0x32, 0x49, 0x00, 0x01 };   /* locally administered */
    /* ENet admits pending peers too: room for a few beyond max_players, capped by the transport */
    int slots = s->cfg.max_players + 4;
    if (slots > MAX_SESSIONS) slots = MAX_SESSIONS;
    if (t->initialize(t->ctx, server_mac) != PSP_NET_OK || t->host_session(t->ctx, "PSP2I", slots + 1) != PSP_NET_OK) {
        slog(s, "cannot bind the game port %u", (unsigned)s->cfg.port);
        psp_net_enet_destroy(s->enet);
        s->enet = NULL;
        return -1;
    }
    if (s->cfg.mode == SRV_MODE_LAN) {
        s->lan = psp_mmo_lan_responder_open(s->cfg.lan_port);
        if (!s->lan) {
            slog(s, "cannot bind the LAN discovery port %u", (unsigned)s->cfg.lan_port);
            srv_stop(s);
            return -1;
        }
    }
    s->started = 1;
    check_configs(s, RELOAD_DROPS | RELOAD_RATES);
    if (s->lan)
        slog(s, "'%s': LAN mode, game port %u (udp), discovery port %u (udp), up to %d players, version %s (hash %08X)",
             s->name, (unsigned)srv_port(s), (unsigned)srv_lan_port(s), s->cfg.max_players,
             PSP_MMO_VERSION_STRING, (unsigned)srv_version_hash());
    else
        slog(s, "'%s': online mode, game port %u (udp), up to %d players, version %s (hash %08X)",
             s->name, (unsigned)srv_port(s), s->cfg.max_players, PSP_MMO_VERSION_STRING, (unsigned)srv_version_hash());
    if (s->cfg.mode == SRV_MODE_ONLINE)
        slog(s, "online: %u packets/s per player (burst %u), handshake within %u ms",
             (unsigned)s->cfg.rate_pps, (unsigned)(2 * s->cfg.rate_pps), (unsigned)s->cfg.handshake_timeout_ms);
    return 0;
}

static void kick(srv *s, session *ss, uint32_t reason) {
    const uint64_t id = ss->id;
    psp_net_enet_disconnect(s->enet, id, reason);     /* calls on_peer(left): frees the session */
    session *still = find(s, id);
    if (still) memset(still, 0, sizeof *still);
}

void srv_tick(srv *s, uint64_t now_ms) {
    if (!s->started) return;
    s->now_ms = now_ms;
    if (!s->last_refill_ms) s->last_refill_ms = s->second_start_ms = now_ms;

    /* refill the token buckets */
    const double dt = (double)(now_ms - s->last_refill_ms) / 1000.0;
    s->last_refill_ms = now_ms;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        if (ss->state == SS_FREE) continue;
        ss->tokens += dt * s->cfg.rate_pps;
        if (ss->tokens > 2.0 * s->cfg.rate_pps) ss->tokens = 2.0 * s->cfg.rate_pps;
        ss->kill_tokens += dt * s->cfg.kills_per_sec;
        if (ss->kill_tokens > 2.0 * s->cfg.kills_per_sec) ss->kill_tokens = 2.0 * s->cfg.kills_per_sec;
    }

    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    t->poll_packets(t->ctx, on_packet, s);

    /* once a second: rate-limit verdicts */
    const int new_second = now_ms - s->second_start_ms >= 1000;
    if (new_second) { s->second_start_ms = now_ms; check_configs(s, 0); }   /* hot reload */

    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        if (ss->state == SS_FREE) continue;
        if (!ss->kick && ss->state == SS_PENDING && now_ms - ss->joined_ms > s->cfg.handshake_timeout_ms) {
            ss->kick = PSP_MMO_DISC_TIMEOUT;
            s->st.kicked_timeout++;
            slog(s, "peer %llu: no handshake within %u ms", (unsigned long long)ss->id, (unsigned)s->cfg.handshake_timeout_ms);
        }
        if (new_second && s->cfg.mode == SRV_MODE_ONLINE) {
            ss->bad_seconds = ss->dropped_this_second ? ss->bad_seconds + 1 : 0;
            ss->dropped_this_second = 0;
            if (!ss->kick && ss->bad_seconds >= 3) {
                ss->kick = PSP_MMO_DISC_RATE;
                s->st.kicked_rate++;
                slog(s, "peer %llu ('%s'): over %u packets/s for 3 s", (unsigned long long)ss->id, ss->name, (unsigned)s->cfg.rate_pps);
            }
        }
        if (ss->kick) kick(s, ss, ss->kick);
    }

    if (s->lan) {
        const psp_mmo_lan_info info = {
            .game_port = srv_port(s), .players = (uint16_t)count(s, SS_ACTIVE),
            .max_players = (uint16_t)s->cfg.max_players, .version_hash = srv_version_hash(),
        };
        psp_mmo_lan_info i2 = info;
        memcpy(i2.name, s->name, sizeof i2.name);
        s->st.lan_replies += (uint64_t)psp_mmo_lan_responder_poll(s->lan, &i2);
    }
}

void srv_stop(srv *s) {
    if (s->enet) {
        psp_net_enet_disconnect_all(s->enet, PSP_MMO_DISC_SHUTDOWN);
        const psp_net_transport *t = psp_net_enet_transport(s->enet);
        for (int i = 0; i < 10; i++) {                /* ~100 ms for the disconnects to go out */
            t->poll_packets(t->ctx, NULL, NULL);
            clk_sleep_until_ns(clk_now_ns() + 10000000u);
        }
        psp_net_enet_destroy(s->enet);
        s->enet = NULL;
    }
    psp_mmo_lan_responder_close(s->lan);
    s->lan = NULL;
    memset(s->sess, 0, sizeof s->sess);
    s->started = 0;
}

void srv_destroy(srv *s) {
    if (!s) return;
    srv_stop(s);
    loot_free(s->loot);
    free(s);
}

void srv_get_stats(const srv *s, srv_stats *out) {
    *out = s->st;
    out->players = count(s, SS_ACTIVE);
    out->pending = count(s, SS_PENDING);
}

uint16_t srv_port(const srv *s) { return s->enet ? psp_net_enet_port(s->enet) : 0; }
uint16_t srv_lan_port(const srv *s) { return s->lan ? psp_mmo_lan_responder_port(s->lan) : 0; }

/* ---- admin ---------------------------------------------------------------------------- */

static const char *const EVENTS[SRV_EVENT_COUNT] = { "spring", "summer", "halloween", "christmas", "new_year", "anniversary" };
const char *srv_event_name(int i) { return i >= 0 && i < SRV_EVENT_COUNT ? EVENTS[i] : NULL; }
uint32_t srv_events(const srv *s) { return s->events; }
db *srv_db(const srv *s) { return s->cfg.database; }
const char *srv_drop_tables_path(const srv *s) { return s->cfg.drop_tables_path; }
const char *srv_name(const srv *s) { return s->name; }
int srv_max_players(const srv *s) { return s->cfg.max_players; }
int srv_mode_online(const srv *s) { return s->cfg.mode == SRV_MODE_ONLINE; }

int srv_find_ip(const srv *s, uint64_t id, char *ip, size_t cap) {
    char addr[64];
    if (!s->enet || psp_net_enet_peer_address(s->enet, id, addr, sizeof addr) != 0) return -1;
    char *colon = strrchr(addr, ':');
    if (colon) *colon = 0;
    snprintf(ip, cap, "%s", addr);
    return 0;
}

int srv_players(const srv *s, srv_player *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS && n < max; i++) {
        const session *ss = &s->sess[i];
        if (ss->state != SS_ACTIVE) continue;
        srv_player *p = &out[n++];
        memset(p, 0, sizeof *p);
        p->session = ss->id;
        memcpy(p->name, ss->name, sizeof p->name);
        snprintf(p->mac, sizeof p->mac, "%02X:%02X:%02X:%02X:%02X:%02X", ss->mac[0], ss->mac[1], ss->mac[2], ss->mac[3], ss->mac[4], ss->mac[5]);
        psp_net_enet_peer_address(s->enet, ss->id, p->ip, sizeof p->ip);
        p->ping_ms = psp_net_enet_peer_rtt(s->enet, ss->id);
        p->joined_ms = ss->joined_ms;
        snprintf(p->location, sizeof p->location, "-");
    }
    return n;
}

int srv_kick(srv *s, uint64_t id, uint32_t reason) {
    session *ss = find(s, id);
    if (!ss) return -1;
    slog(s, "kicking session %llu ('%s'), reason %u", (unsigned long long)id, ss->name, (unsigned)reason);
    kick(s, ss, reason);
    return 0;
}

int srv_kick_ip(srv *s, const char *ip, uint32_t reason) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *ss = &s->sess[i];
        char pip[64];
        if (ss->state == SS_FREE || srv_find_ip(s, ss->id, pip, sizeof pip) != 0 || strcmp(pip, ip)) continue;
        kick(s, ss, reason);
        n++;
    }
    return n;
}

int srv_ban_session(srv *s, uint64_t id, const char *reason, int64_t minutes, const char *admin, char *ip_out) {
    char ip[64];
    if (!s->cfg.database || !find(s, id) || srv_find_ip(s, id, ip, sizeof ip) != 0) return -1;
    const int64_t expires = minutes > 0 ? (int64_t)time(NULL) + minutes * 60 : 0;
    if (db_ip_ban(s->cfg.database, ip, reason, expires, admin) != DB_OK) return -1;
    if (ip_out) snprintf(ip_out, 64, "%s", ip);
    srv_kick_ip(s, ip, PSP_MMO_DISC_BANNED);
    return 0;
}

int srv_announce(srv *s, const char *text, uint32_t rgb) {
    if (!s->enet || !text || !*text) return -1;
    uint8_t m[PSP_MMO_NOTICE_LEN];
    psp_mmo_put_hdr(m, MSG_S2C_SYSTEM_NOTICE);
    psp_mmo_put_u32(m + 8, rgb & 0xFFFFFFu);
    psp_mmo_put_str(m + 12, text, PSP_MMO_NOTICE_TEXT_LEN);
    const psp_net_transport *t = psp_net_enet_transport(s->enet);
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s->sess[i].state != SS_ACTIVE) continue;
        const psp_net_peer to = { { 0 }, s->sess[i].id };
        n += t->send_packet(t->ctx, &to, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, sizeof m) == PSP_NET_OK;
    }
    slog(s, "announcement (#%06X) to %d players: %s", (unsigned)(rgb & 0xFFFFFFu), n, text);
    return n;
}

int srv_set_event(srv *s, const char *name, int on) {
    for (int i = 0; i < SRV_EVENT_COUNT; i++) {
        if (strcmp(name, EVENTS[i])) continue;
        const uint32_t was = s->events;
        if (on) s->events |= 1u << i; else s->events &= ~(1u << i);
        if (s->events != was) {
            slog(s, "event '%s' %s", name, on ? "on" : "off");
            for (int k = 0; k < MAX_SESSIONS; k++) if (s->sess[k].state == SS_ACTIVE) send_events(s, s->sess[k].id);
        }
        return 0;
    }
    return -1;
}
