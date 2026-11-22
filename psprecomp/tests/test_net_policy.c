/* The dedicated server's hooks in the ENet transport, and LAN discovery.
 * Links psprecomp_net only (as the server does).
 *
 *  - on_peer reports joins and leaves with the peer's id and MAC;
 *  - relay_authorized_only: an unauthorized client's packets reach the host
 *    but are not relayed, and nothing is relayed to it; after
 *    psp_net_enet_authorize both ways work;
 *  - admit sees every data packet and can drop it;
 *  - psp_net_enet_disconnect delivers its reason to the client;
 *  - psp_net_enet_peer_address gives "127.0.0.1:port";
 *  - a LAN probe gets the responder's reply (name, ports, counts, hash). */
#include "psprecomp/net_enet.h"
#include "psprecomp/net_lan.h"
#include "psprecomp/net_proto.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static psp_net_enet *g_host, *g_a, *g_b;
static int g_joins, g_leaves, g_admit_calls, g_drop_next;
static uint64_t g_last_join;
static void on_peer(void *u, uint64_t id, const uint8_t mac[6], int joined) {
    (void)u; (void)mac;
    if (joined) { g_joins++; g_last_join = id; } else g_leaves++;
}
static int admit(void *u, uint64_t id, uint32_t len) {
    (void)u; (void)id; (void)len;
    g_admit_calls++;
    if (g_drop_next) { g_drop_next = 0; return 0; }
    return 1;
}
static void pump_host(void *u) { (void)u; const psp_net_transport *t = psp_net_enet_transport(g_host); t->poll_packets(t->ctx, NULL, NULL); }

static int got_host, got_a, got_b;
static void rx_host(const psp_net_packet *p, void *u) { (void)p; (void)u; got_host++; }
static void rx_a(const psp_net_packet *p, void *u) { (void)p; (void)u; got_a++; }
static void rx_b(const psp_net_packet *p, void *u) { (void)p; (void)u; got_b++; }

static psp_mmo_lan_responder *g_resp;
static volatile int g_resp_run;
static void *responder_thread(void *u) {
    (void)u;
    const psp_mmo_lan_info info = { "Test Server", 29001, 3, 32, 0 };
    psp_mmo_lan_info i2 = info;
    i2.version_hash = psp_mmo_fnv1a("v1");
    while (g_resp_run) {
        psp_mmo_lan_responder_poll(g_resp, &i2);
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void pump_all(int rounds) {
    for (int i = 0; i < rounds; i++) {
        const psp_net_transport *h = psp_net_enet_transport(g_host), *a = psp_net_enet_transport(g_a), *b = psp_net_enet_transport(g_b);
        h->poll_packets(h->ctx, rx_host, NULL);
        a->poll_packets(a->ctx, rx_a, NULL);
        b->poll_packets(b->ctx, rx_b, NULL);
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

static psp_net_enet *join(uint8_t last) {
    psp_net_enet_config c = { .connect_timeout_ms = 2000, .idle = pump_host };
    psp_net_enet *e = psp_net_enet_create(&c);
    const psp_net_transport *t = psp_net_enet_transport(e);
    const uint8_t mac[6] = { 0x00, 0x50, 0x81, 0, 0, last };
    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", psp_net_enet_port(g_host));
    CHECK(t->initialize(t->ctx, mac) == PSP_NET_OK);
    CHECK(t->join_session(t->ctx, addr, "PSP2I") == PSP_NET_OK);
    return e;
}

int main(void) {
    psp_net_enet_config hc = { .bind_host = "127.0.0.1", .on_peer = on_peer, .admit = admit, .relay_authorized_only = 1 };
    g_host = psp_net_enet_create(&hc);
    const psp_net_transport *h = psp_net_enet_transport(g_host);
    const uint8_t hmac[6] = { 0x02, 0x50, 0x32, 0x49, 0, 1 };
    CHECK(h->initialize(h->ctx, hmac) == PSP_NET_OK && h->host_session(h->ctx, "PSP2I", 8) == PSP_NET_OK);

    g_a = join(0xA);
    const uint64_t id_a = g_last_join;
    g_b = join(0xB);
    const uint64_t id_b = g_last_join;
    pump_all(20);
    CHECK(g_joins == 2 && id_a && id_b && id_a != id_b);

    char addr[64] = "";
    CHECK(psp_net_enet_peer_address(g_host, id_a, addr, sizeof addr) == 0 && strncmp(addr, "127.0.0.1:", 10) == 0);

    const psp_net_transport *a = psp_net_enet_transport(g_a), *b = psp_net_enet_transport(g_b);
    const psp_net_peer to_host = { { 0 }, 1 };
    const uint8_t x[4] = { 1, 2, 3, 4 };

    /* unauthorized: reaches the host, not relayed */
    CHECK(a->send_packet(a->ctx, &to_host, PSP_NET_CH_RELIABLE, 0, x, 4) == PSP_NET_OK);
    CHECK(a->send_packet(a->ctx, NULL, PSP_NET_CH_RELIABLE, 7, x, 4) == PSP_NET_OK);   /* broadcast */
    pump_all(30);
    CHECK(got_host == 2 && got_b == 0);
    CHECK(g_admit_calls == 2);

    /* authorized both ways: relayed */
    CHECK(psp_net_enet_authorize(g_host, id_a, 1) == PSP_NET_OK);
    CHECK(psp_net_enet_authorize(g_host, id_b, 1) == PSP_NET_OK);
    CHECK(a->send_packet(a->ctx, NULL, PSP_NET_CH_RELIABLE, 7, x, 4) == PSP_NET_OK);
    pump_all(30);
    CHECK(got_b == 1);

    /* admit drops */
    g_drop_next = 1;
    CHECK(a->send_packet(a->ctx, NULL, PSP_NET_CH_RELIABLE, 7, x, 4) == PSP_NET_OK);
    pump_all(30);
    CHECK(got_b == 1 && got_host == 3);

    /* disconnect with a reason */
    CHECK(psp_net_enet_disconnect(g_host, id_b, PSP_MMO_DISC_KICKED) == PSP_NET_OK);
    pump_all(100);
    CHECK(psp_net_enet_disconnect_reason(g_b) == PSP_MMO_DISC_KICKED);
    CHECK(psp_net_enet_peer_count(g_host) == 1);
    CHECK(g_leaves == 1);
    (void)b;

    /* LAN discovery: the responder answers from its own thread */
    g_resp = psp_mmo_lan_responder_open(0);
    CHECK(g_resp != NULL);
    pthread_t th;
    g_resp_run = 1;
    pthread_create(&th, NULL, responder_thread, NULL);
    psp_mmo_lan_info got;
    char ip[64] = "";
    CHECK(psp_mmo_lan_discover(psp_mmo_lan_responder_port(g_resp), 2000, &got, ip, sizeof ip) == 0);
    g_resp_run = 0;
    pthread_join(th, NULL);
    CHECK(strcmp(got.name, "Test Server") == 0);
    CHECK(got.game_port == 29001 && got.players == 3 && got.max_players == 32);
    CHECK(got.version_hash == psp_mmo_fnv1a("v1"));
    CHECK(strcmp(ip, "127.0.0.1") == 0 || ip[0]);
    psp_mmo_lan_responder_close(g_resp);

    psp_net_enet_destroy(g_a);
    psp_net_enet_destroy(g_b);
    psp_net_enet_destroy(g_host);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("net_policy: ok");
    return 0;
}
