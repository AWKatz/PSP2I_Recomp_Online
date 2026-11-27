/* psp2i_server core, end to end: the server ticks at 30 Hz on its own thread;
 * real ENet clients connect from the main thread.
 *
 *  - srv_check_handshake: accepts a good one; rejects bad length/magic,
 *    empty/control/invalid-UTF-8/unterminated names, zero and multicast MACs;
 *  - LAN mode: a probe gets name, game port, player counts and version hash;
 *  - a good handshake is acknowledged (status 0, session id) and counted;
 *  - a duplicate MAC and a bad name are rejected (ACK status, then the
 *    HANDSHAKE disconnect reason);
 *  - a peer that never sends a handshake is dropped (TIMEOUT);
 *  - relaying: a pending peer's broadcast does not reach players; players
 *    reach each other;
 *  - online mode: traffic before the handshake -> PROTOCOL; flooding past the
 *    rate limit for 3 s -> RATE;
 *  - stopping the server tells clients (SHUTDOWN). */
#include "clock.h"
#include "server.h"

#include <psprecomp/net_enet.h>
#include <psprecomp/net_lan.h>
#include <psprecomp/net_proto.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void quiet(const char *l) { (void)l; }
static void loud(const char *l) { printf("  server: %s\n", l); }

/* ---- the server thread ---- */
typedef struct { srv *s; volatile int run; pthread_t th; } srv_thread;
static void *srv_main(void *u) {
    srv_thread *t = u;
    uint64_t next = clk_now_ns();
    while (t->run) {
        srv_tick(t->s, clk_now_ns() / 1000000u);
        next += 1000000000ull / 30;
        clk_sleep_until_ns(next);
    }
    return NULL;
}
static void srv_go(srv_thread *t, const srv_config *c) {
    t->s = srv_create(c);
    CHECK(t->s && srv_start(t->s) == 0);
    t->run = 1;
    pthread_create(&t->th, NULL, srv_main, t);
}
static void srv_halt(srv_thread *t, srv_stats *st) {
    t->run = 0;
    pthread_join(t->th, NULL);
    if (st) srv_get_stats(t->s, st);
}

static void sleep_ms(unsigned ms) { clk_sleep_until_ns(clk_now_ns() + (uint64_t)ms * 1000000u); }

/* ---- a client ---- */
typedef struct {
    psp_net_enet *e;
    int acked;
    uint32_t status;
    uint64_t session;
    int game_rx;
    int rewards, drops;
    uint32_t last_exp, last_item, last_flags;
    int party_lists, party_players, party_count, party_members, party_max, party_open;
    char party_leader[33];
    int warps;
    int32_t warp_dest;
    int texts;                  /* MSG_S2C_SERVER_TEXT messages */
    char terms[64];             /* the terms text, if one part arrived */
    int notices_parts;          /* -1 until the notices message arrives */
} client;

static void cl_rx(const psp_net_packet *p, void *u) {
    client *c = u;
    if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_HANDSHAKE_ACK && p->len == PSP_MMO_HANDSHAKE_ACK_LEN) {
        c->acked = 1;
        c->status = psp_mmo_get_u32(p->data + PSP_MMO_HDR_LEN);
        c->session = (uint64_t)psp_mmo_get_u32(p->data + PSP_MMO_HDR_LEN + 4) | ((uint64_t)psp_mmo_get_u32(p->data + PSP_MMO_HDR_LEN + 8) << 32);
    } else if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_KILL_REWARD && p->len == PSP_MMO_KILL_REWARD_LEN) {
        c->rewards++;
        c->last_exp = psp_mmo_get_u32(p->data + 12);
    } else if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_SPAWN_DROP && p->len == PSP_MMO_SPAWN_DROP_LEN) {
        c->drops++;
        c->last_item = psp_mmo_get_u32(p->data + 20);
        c->last_flags = psp_mmo_get_u32(p->data + 28);
    } else if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_PARTY_LIST && p->len >= PSP_MMO_PARTY_LIST_HDR_LEN) {
        c->party_lists++;
        c->party_players = psp_mmo_get_u16(p->data + 8);
        c->party_count = psp_mmo_get_u16(p->data + 10);
        c->party_leader[0] = 0;
        if (c->party_count && p->len >= PSP_MMO_PARTY_LIST_HDR_LEN + PSP_MMO_PARTY_ENTRY_LEN) {
            const uint8_t *e = p->data + PSP_MMO_PARTY_LIST_HDR_LEN;
            c->party_members = e[6]; c->party_max = e[7]; c->party_open = e[8];
            memcpy(c->party_leader, e + 10, 32); c->party_leader[32] = 0;
        }
    } else if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_PARTY_WARP && p->len == PSP_MMO_PARTY_WARP_LEN) {
        c->warps++;
        c->warp_dest = (int32_t)psp_mmo_get_u32(p->data + 8);
    } else if (p->port == PSP_MMO_CTRL_PORT && psp_mmo_type(p->data, p->len) == MSG_S2C_SERVER_TEXT && p->len >= PSP_MMO_TEXT_HDR_LEN) {
        c->texts++;
        const uint32_t n = psp_mmo_get_u16(p->data + 12);
        if (p->data[8] == PSP_MMO_TEXT_TERMS && n < sizeof c->terms) { memcpy(c->terms, p->data + PSP_MMO_TEXT_HDR_LEN, n); c->terms[n] = 0; }
        if (p->data[8] == PSP_MMO_TEXT_NOTICES) c->notices_parts = p->data[10];
    } else if (p->port != PSP_MMO_CTRL_PORT) {
        c->game_rx++;
    }
}
static void cl_pump(client *c, unsigned ms) {
    const uint64_t end = clk_now_ns() + (uint64_t)ms * 1000000u;
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    do { t->poll_packets(t->ctx, cl_rx, c); sleep_ms(2); } while (clk_now_ns() < end);
}
static int cl_join(client *c, uint16_t port, uint8_t mac_last) {
    memset(c, 0, sizeof *c);
    c->notices_parts = -1;
    psp_net_enet_config cfg = { .connect_timeout_ms = 3000 };
    c->e = psp_net_enet_create(&cfg);
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const uint8_t mac[6] = { 0x00, 0x50, 0x81, 0x00, 0x00, mac_last };
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    return t->initialize(t->ctx, mac) == PSP_NET_OK && t->join_session(t->ctx, addr, "PSP2I") == PSP_NET_OK;
}
static void make_hs(uint8_t *b, const uint8_t mac[6], const char *name) {
    memset(b, 0, PSP_MMO_HANDSHAKE_LEN);
    psp_mmo_put_hdr(b, MSG_C2S_HANDSHAKE);
    memcpy(b + PSP_MMO_HDR_LEN, mac, 6);
    if (name) memcpy(b + PSP_MMO_HDR_LEN + 6, name, strlen(name));
    psp_mmo_put_str(b + PSP_MMO_HDR_LEN + 6 + PSP_MMO_NAME_LEN, "test", PSP_MMO_VERSION_LEN);
}
static void cl_handshake(client *c, uint8_t mac_last, const char *name) {
    uint8_t hs[PSP_MMO_HANDSHAKE_LEN];
    const uint8_t mac[6] = { 0x00, 0x50, 0x81, 0x00, 0x00, mac_last };
    make_hs(hs, mac, name);
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const psp_net_peer host = { { 0 }, 1 };
    t->send_packet(t->ctx, &host, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, hs, sizeof hs);
    for (int i = 0; i < 100 && !c->acked; i++) cl_pump(c, 10);
}
static void cl_broadcast(client *c) {
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const uint8_t x[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    t->send_packet(t->ctx, NULL, PSP_NET_CH_RELIABLE, 0x309, x, sizeof x);
}
static uint32_t cl_wait_disc(client *c, unsigned ms) {
    for (unsigned i = 0; i < ms / 10 && !psp_net_enet_disconnect_reason(c->e); i++) cl_pump(c, 10);
    return psp_net_enet_disconnect_reason(c->e);
}
static void cl_ctrl(client *c, const uint8_t *m, uint32_t len) {
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const psp_net_peer host = { { 0 }, 1 };
    t->send_packet(t->ctx, &host, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, m, len);
}
static void cl_party(client *c, uint8_t role, uint8_t members, uint8_t max, uint8_t open) {
    uint8_t m[PSP_MMO_PARTY_STATE_LEN] = { 0 };
    psp_mmo_put_hdr(m, MSG_C2S_PARTY_STATE);
    m[8] = role; m[9] = members; m[10] = max; m[11] = open;
    cl_ctrl(c, m, sizeof m);
}
static void cl_member_of(client *c, uint8_t leader_mac_last) {
    uint8_t m[PSP_MMO_PARTY_STATE_LEN] = { 0 };
    psp_mmo_put_hdr(m, MSG_C2S_PARTY_STATE);
    m[8] = PSP_MMO_PARTY_MEMBER; m[10] = 4;
    const uint8_t mac[6] = { 0x00, 0x50, 0x81, 0x00, 0x00, leader_mac_last };
    memcpy(m + 12, mac, 6);
    cl_ctrl(c, m, sizeof m);
}
static void cl_warp(client *c, int32_t dest, int16_t entry) {
    uint8_t m[PSP_MMO_PARTY_WARP_LEN] = { 0 };
    psp_mmo_put_hdr(m, MSG_C2S_PARTY_WARP);
    psp_mmo_put_u32(m + 8, (uint32_t)dest);
    psp_mmo_put_u16(m + 14, 2000);
    psp_mmo_put_u16(m + 16, (uint16_t)entry);
    cl_ctrl(c, m, sizeof m);
}
static void cl_party_list(client *c) {
    uint8_t m[PSP_MMO_PARTY_LIST_REQ_LEN];
    psp_mmo_put_hdr(m, MSG_C2S_PARTY_LIST_REQ);
    const int before = c->party_lists;
    cl_ctrl(c, m, sizeof m);
    for (int i = 0; i < 100 && c->party_lists == before; i++) cl_pump(c, 10);
}
static void cl_kill(client *c, uint32_t enemy) {
    uint8_t k[PSP_MMO_ENEMY_KILLED_LEN] = { 0 };
    psp_mmo_put_hdr(k, MSG_C2S_ENEMY_KILLED);
    psp_mmo_put_u32(k + 8, enemy);
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const psp_net_peer host = { { 0 }, 1 };
    t->send_packet(t->ctx, &host, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, k, sizeof k);
}
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}
static void cl_free(client *c) { psp_net_enet_destroy(c->e); c->e = NULL; }

/* ---- handshake checks ---- */
static uint32_t check(const uint8_t mac[6], const char *name, int tweak) {
    uint8_t b[PSP_MMO_HANDSHAKE_LEN + 1];
    make_hs(b, mac, name);
    uint32_t len = PSP_MMO_HANDSHAKE_LEN;
    if (tweak == 1) len--;                                    /* short */
    if (tweak == 2) b[0] ^= 1;                                /* bad magic */
    if (tweak == 3) memset(b + PSP_MMO_HDR_LEN + 6, 'A', PSP_MMO_NAME_LEN);   /* no NUL */
    char out[PSP_MMO_NAME_LEN + 1];
    uint8_t m[6];
    return srv_check_handshake(b, len, out, m);
}

int main(void) {
    const uint8_t good[6] = { 0x00, 0x50, 0x81, 1, 2, 3 }, zero[6] = { 0 }, multi[6] = { 0x01, 0, 0x5E, 0, 0, 1 };
    CHECK(check(good, "Ann", 0) == PSP_MMO_OK);
    CHECK(check(good, "\xE3\x82\xA2\xE3\x83\xB3", 0) == PSP_MMO_OK);   /* Japanese, UTF-8 */
    CHECK(check(good, "Ann", 1) == PSP_MMO_ERR_VERSION);
    CHECK(check(good, "Ann", 2) == PSP_MMO_ERR_VERSION);
    CHECK(check(good, "Ann", 3) == PSP_MMO_ERR_NAME);
    CHECK(check(good, "", 0) == PSP_MMO_ERR_NAME);
    CHECK(check(good, "A\x01" "b", 0) == PSP_MMO_ERR_NAME);
    CHECK(check(good, "\xC3", 0) == PSP_MMO_ERR_NAME);                  /* truncated UTF-8 */
    CHECK(check(good, "\xC0\xAF", 0) == PSP_MMO_ERR_NAME);              /* overlong */
    CHECK(check(zero, "Ann", 0) == PSP_MMO_ERR_MAC);
    CHECK(check(multi, "Ann", 0) == PSP_MMO_ERR_MAC);

    /* ---- LAN mode ---- */
    char tpath[256];                                  /* the Terms of Use the server sends; no notices file */
    snprintf(tpath, sizeof tpath, "%s/psp2i_terms_%d.txt", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    write_file(tpath, "Be kind.");
    srv_config lc = { .mode = SRV_MODE_LAN, .bind = "127.0.0.1", .port = 0, .lan_port = 0, .name = "Test LAN",
                      .max_players = 8, .handshake_timeout_ms = 400, .log = getenv("TEST_SERVER_LOG") ? loud : quiet,
                      .terms_path = tpath, .notices_path = "/nonexistent/notices.txt" };
    srv_thread lt;
    srv_go(&lt, &lc);
    const uint16_t port = srv_port(lt.s), lan_port = srv_lan_port(lt.s);
    CHECK(port && lan_port);

    psp_mmo_lan_info info;
    char ip[64] = "";
    CHECK(psp_mmo_lan_discover(lan_port, 1000, &info, ip, sizeof ip) == 0);
    CHECK(strcmp(info.name, "Test LAN") == 0 && info.game_port == port && info.players == 0 && info.max_players == 8);
    CHECK(info.version_hash == psp_mmo_fnv1a(PSP_MMO_VERSION_STRING) && info.version_hash == srv_version_hash());

    client a, b, c, d;
    CHECK(cl_join(&a, port, 0xA));
    cl_handshake(&a, 0xA, "Ann");
    CHECK(a.acked && a.status == PSP_MMO_OK && a.session == psp_net_enet_self_id(a.e));
    for (int i = 0; i < 100 && a.texts < 2; i++) cl_pump(&a, 10);
    CHECK(a.texts == 2 && strcmp(a.terms, "Be kind.") == 0 && a.notices_parts == 0);   /* the file's text; none */
    remove(tpath);
    CHECK(psp_mmo_lan_discover(lan_port, 1000, &info, ip, sizeof ip) == 0 && info.players == 1);

    CHECK(cl_join(&b, port, 0xB));                  /* pending: its broadcast is not relayed */
    cl_broadcast(&b);
    cl_pump(&a, 150);
    CHECK(a.game_rx == 0);
    cl_handshake(&b, 0xB, "Bob");
    CHECK(b.acked && b.status == PSP_MMO_OK);
    cl_broadcast(&b);
    cl_pump(&b, 50);
    cl_pump(&a, 150);
    CHECK(a.game_rx == 1);

    /* the party directory: Ann leads, Bob looks */
    cl_party_list(&b);
    CHECK(b.party_lists == 1 && b.party_players == 2 && b.party_count == 0);
    cl_party(&a, PSP_MMO_PARTY_LEADER, 1, 4, 1);
    cl_pump(&a, 50);
    cl_party_list(&b);
    CHECK(b.party_count == 1 && strcmp(b.party_leader, "Ann") == 0 && b.party_members == 1 && b.party_max == 4 && b.party_open == 1);
    cl_party(&a, PSP_MMO_PARTY_LEADER, 4, 4, 1);                 /* full: listed, not open */
    cl_pump(&a, 50);
    cl_party_list(&b);
    CHECK(b.party_count == 1 && b.party_members == 4 && b.party_open == 0);
    /* party warps: the leader's reaches the leader and its members; a member's is ignored */
    cl_member_of(&b, 0xA);
    cl_pump(&b, 50);
    cl_warp(&a, 2010000, 3);
    for (int i = 0; i < 100 && (a.warps < 1 || b.warps < 1); i++) { cl_pump(&a, 5); cl_pump(&b, 5); }
    CHECK(a.warps == 1 && b.warps == 1 && b.warp_dest == 2010000);
    cl_warp(&b, 2010000, 4);
    for (int i = 0; i < 30; i++) { cl_pump(&a, 5); cl_pump(&b, 5); }
    CHECK(a.warps == 1 && b.warps == 1);
    cl_party(&b, PSP_MMO_PARTY_NONE, 0, 0, 0);
    cl_party(&a, PSP_MMO_PARTY_NONE, 0, 0, 0);
    cl_pump(&a, 50);
    cl_party_list(&b);
    CHECK(b.party_count == 0);

    /* Ann's link dies without the server noticing and she comes back from the same
     * address on a new connection: the new session replaces the stale one. */
    CHECK(cl_join(&c, port, 0xC));
    cl_handshake(&c, 0xA, "Ann");
    CHECK(c.acked && c.status == PSP_MMO_OK);
    CHECK(cl_wait_disc(&a, 1000) == PSP_MMO_DISC_KICKED);
    /* The dropped client's transport has let the dead connection go: the same
     * transport joins again, and that session replaces the one before it. */
    {
        const psp_net_transport *t = psp_net_enet_transport(a.e);
        CHECK(t->poll_packets(t->ctx, cl_rx, &a) == PSP_NET_ERR_NOT_INIT);
        char addr[32];
        snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
        CHECK(t->join_session(t->ctx, addr, "PSP2I") == PSP_NET_OK);
        CHECK(psp_net_enet_disconnect_reason(a.e) == 0);
        a.acked = 0;
        cl_handshake(&a, 0xA, "Ann");
        CHECK(a.acked && a.status == PSP_MMO_OK);
        CHECK(cl_wait_disc(&c, 1000) == PSP_MMO_DISC_KICKED);
    }
    cl_free(&c);

    CHECK(cl_join(&c, port, 0xC));
    cl_handshake(&c, 0xC, "bad\x07name");
    CHECK(c.acked && c.status == PSP_MMO_ERR_NAME);
    CHECK(cl_wait_disc(&c, 1000) == PSP_MMO_DISC_HANDSHAKE);
    cl_free(&c);

    CHECK(cl_join(&d, port, 0xD));                  /* says nothing */
    CHECK(cl_wait_disc(&d, 2000) == PSP_MMO_DISC_TIMEOUT);
    cl_free(&d);

    srv_stats st;
    srv_halt(&lt, &st);
    CHECK(st.players == 2 && st.handshakes_ok == 4 && st.handshakes_rejected == 1 && st.kicked_timeout == 1);
    CHECK(st.lan_replies >= 2);   /* a probe goes to the broadcast address and to loopback */
    srv_stop(lt.s);                                   /* tells the players */
    CHECK(cl_wait_disc(&a, 1000) == PSP_MMO_DISC_SHUTDOWN);
    CHECK(cl_wait_disc(&b, 1000) == PSP_MMO_DISC_SHUTDOWN);
    cl_free(&a);
    cl_free(&b);
    srv_destroy(lt.s);

    /* ---- loot: kills, drops, hot reload ---- */
    {
        char dpath[256], rpath[256];
        snprintf(dpath, sizeof dpath, "%s/psp2i_drops_%d.json", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
        snprintf(rpath, sizeof rpath, "%s/psp2i_rates_%d.toml", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
        write_file(dpath, "{\"enemies\":[{\"id\":5,\"exp\":10,\"drop_chance\":1,\"items\":[{\"id\":111,\"weight\":1}]}]}");
        write_file(rpath, "exp_rate = 1.0\n");
        srv_config kc = lc;
        kc.drop_tables_path = dpath;
        kc.rates_path = rpath;
        kc.kills_per_sec = 5;
        kc.rng_seed = 99;
        kc.handshake_timeout_ms = 5000;
        srv_thread kt;
        srv_go(&kt, &kc);
        client k1, k2;
        CHECK(cl_join(&k1, srv_port(kt.s), 0x11) && cl_join(&k2, srv_port(kt.s), 0x12));
        cl_handshake(&k1, 0x11, "Killer");
        cl_handshake(&k2, 0x12, "Watcher");
        cl_kill(&k1, 5);
        cl_pump(&k1, 200); cl_pump(&k2, 100);
        CHECK(k1.rewards == 1 && k1.last_exp == 10 && k1.drops == 1 && k1.last_item == 111);
        CHECK(k2.drops == 1 && k2.rewards == 0);                       /* everyone sees the drop; only the killer the reward */
        cl_kill(&k1, 404);                                             /* not in the table: nothing */
        cl_pump(&k1, 150);
        CHECK(k1.rewards == 1 && k1.drops == 1);

        /* hot reload: new item and a rare, 3x exp -- nobody disconnected */
        write_file(dpath, "{\"enemies\":[{\"id\":5,\"exp\":10,\"rares\":[{\"id\":222,\"chance\":1}]}]}");
        write_file(rpath, "exp_rate = 3.0\nrare_drop_rate = 1.0\n");
        for (int i = 0; i < 160; i++) { cl_pump(&k1, 5); cl_pump(&k2, 5); }   /* ~1.6 s: the once-a-second check */
        cl_kill(&k1, 5);
        cl_pump(&k1, 200); cl_pump(&k2, 100);
        CHECK(k1.rewards == 2 && k1.last_exp == 30 && k1.last_item == 222 && (k1.last_flags & PSP_MMO_DROP_RARE));
        CHECK(psp_net_enet_disconnect_reason(k1.e) == 0 && psp_net_enet_disconnect_reason(k2.e) == 0);

        /* a broken file: the old tables stay */
        write_file(dpath, "{\"enemies\":[{\"id\":5,,}]}");
        for (int i = 0; i < 160; i++) { cl_pump(&k1, 5); cl_pump(&k2, 5); }
        cl_kill(&k1, 5);
        cl_pump(&k1, 200);
        CHECK(k1.rewards == 3 && k1.last_item == 222);

        /* kill reports are rate limited (5/s, burst 10) */
        for (int i = 0; i < 40; i++) cl_kill(&k1, 5);
        cl_pump(&k1, 300);
        CHECK(k1.rewards < 3 + 15 && k1.rewards > 3 + 5);
        CHECK(psp_net_enet_disconnect_reason(k1.e) == 0);

        srv_halt(&kt, &st);
        CHECK(st.reloads >= 4 && st.reload_errors == 1 && st.kills_dropped >= 20 && st.rare_drops >= 1);
        CHECK(srv_set_rate(kt.s, "drop_rate", 2.5) == 0 && srv_rates(kt.s).drop == 2.5 && srv_set_rate(kt.s, "nope", 1) == -1);
        srv_destroy(kt.s);
        cl_free(&k1);
        cl_free(&k2);
        remove(dpath);
        remove(rpath);
    }

    /* ---- online mode ---- */
    srv_config oc = { .mode = SRV_MODE_ONLINE, .bind = "127.0.0.1", .port = 0, .max_players = 8, .rate_pps = 20,
                      .handshake_timeout_ms = 5000, .log = lc.log };
    srv_thread ot;
    srv_go(&ot, &oc);
    const uint16_t oport = srv_port(ot.s);
    CHECK(srv_lan_port(ot.s) == 0);                  /* no discovery online */

    CHECK(cl_join(&a, oport, 0xA));                  /* game traffic before the handshake */
    cl_broadcast(&a);
    CHECK(cl_wait_disc(&a, 1000) == PSP_MMO_DISC_PROTOCOL);
    cl_free(&a);

    CHECK(cl_join(&b, oport, 0xB));
    cl_handshake(&b, 0xB, "Bob");
    CHECK(b.acked && b.status == PSP_MMO_OK);
    uint32_t why = 0;                                /* 100 packets/s against a 20/s limit */
    for (int i = 0; i < 600 && !why; i++) { cl_broadcast(&b); cl_pump(&b, 10); why = psp_net_enet_disconnect_reason(b.e); }
    CHECK(why == PSP_MMO_DISC_RATE);
    cl_free(&b);

    CHECK(cl_join(&c, oport, 0xC));                  /* a well-behaved player stays */
    cl_handshake(&c, 0xC, "Cat");
    for (int i = 0; i < 40; i++) { cl_broadcast(&c); cl_pump(&c, 100); }   /* 10 packets/s for 4 s */
    CHECK(c.acked && c.status == PSP_MMO_OK && psp_net_enet_disconnect_reason(c.e) == 0);

    srv_halt(&ot, &st);
    CHECK(st.kicked_protocol == 1 && st.kicked_rate == 1 && st.players == 1);
    srv_destroy(ot.s);
    cl_free(&c);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("server: ok");
    return 0;
}
