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
    srv_config lc = { .mode = SRV_MODE_LAN, .bind = "127.0.0.1", .port = 0, .lan_port = 0, .name = "Test LAN",
                      .max_players = 8, .handshake_timeout_ms = 400, .log = getenv("TEST_SERVER_LOG") ? loud : quiet };
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

    CHECK(cl_join(&c, port, 0xC));                  /* same MAC as Ann */
    cl_handshake(&c, 0xA, "Dup");
    CHECK(c.acked && c.status == PSP_MMO_ERR_DUPLICATE);
    CHECK(cl_wait_disc(&c, 1000) == PSP_MMO_DISC_HANDSHAKE);
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
    CHECK(st.players == 2 && st.handshakes_ok == 2 && st.handshakes_rejected == 2 && st.kicked_timeout == 1);
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
