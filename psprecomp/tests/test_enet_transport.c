/* Task 1.2 gate: ENet transport on 127.0.0.1.
 *
 *  1. a host and client A exchange 1000 simulated ad hoc payloads on each channel,
 *     both directions; channel 0 must arrive 100% intact and in order, channel 1
 *     intact and in sequence (drops allowed, counted);
 *  2. round-trip latency over 200 sequential pings: mean and p99 < 5 ms;
 *  3. client A -> client B goes through the host's relay, with the source
 *     stamped by the host (MAC and server-assigned id), not by the sender;
 *  4. a raw client forging another player's MAC/id is delivered as itself. */
#include "psprecomp/net_enet.h"

#include <enet/enet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define N_PAYLOADS 1000
#define N_PINGS    200
#define ADHOC_PORT 0x5050      /* a PDP port, as a game would address */

static double now_ms(void) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Deterministic payload for (channel, seq): 4-byte seq header then bytes from an LCG. */
static uint32_t payload_len(int ch, uint32_t seq) { return 16 + (seq * 37u + (uint32_t)ch * 101u) % 1200u; }
static void make_payload(int ch, uint32_t seq, uint8_t *out, uint32_t len) {
    uint32_t x = seq * 2654435761u + (uint32_t)ch;
    memcpy(out, &seq, 4);
    for (uint32_t i = 4; i < len; i++) { x = x * 1664525u + 1013904223u; out[i] = (uint8_t)(x >> 24); }
}

typedef struct {
    uint32_t next[2];          /* next expected seq per channel */
    uint32_t got[2], bad[2], out_of_order[2];
    psp_net_peer last_from;
    int      pongs; uint32_t pong_seq;
} rx_state;

static void on_rx(const psp_net_packet *p, void *u) {
    rx_state *st = u;
    st->last_from = p->from;
    if (p->port == ADHOC_PORT + 1) { memcpy(&st->pong_seq, p->data, 4); st->pongs++; return; }   /* ping/pong */
    const int ch = p->channel;
    if (ch > 1 || p->len < 4) { st->bad[ch & 1]++; return; }
    uint32_t seq; memcpy(&seq, p->data, 4);
    uint8_t expect[2048];
    const uint32_t len = payload_len(ch, seq);
    make_payload(ch, seq, expect, len);
    if (p->len != len || memcmp(p->data, expect, len) != 0 || p->port != ADHOC_PORT) st->bad[ch]++;
    if (seq != st->next[ch]) {
        if (ch == 0 || seq < st->next[ch]) st->out_of_order[ch]++;   /* reliable: exact; unreliable: never backwards */
    }
    st->next[ch] = seq + 1;
    st->got[ch]++;
}

/* The host echoes pings back to whoever sent them. */
static psp_net_enet *g_host_for_echo;
static void on_host_rx(const psp_net_packet *p, void *u) {
    if (p->port == ADHOC_PORT + 1) {
        const psp_net_transport *t = psp_net_enet_transport(g_host_for_echo);
        t->send_packet(t->ctx, &p->from, PSP_NET_CH_RELIABLE, ADHOC_PORT + 1, p->data, p->len);
        return;
    }
    on_rx(p, u);
}

static void pump(psp_net_enet *const *e, rx_state *const *st, int n, psp_net_packet_fn host_fn) {
    for (int i = 0; i < n; i++) {
        const psp_net_transport *t = psp_net_enet_transport(e[i]);
        t->poll_packets(t->ctx, i == 0 ? host_fn : on_rx, st[i]);
    }
}

typedef struct { psp_net_enet *const *all; rx_state *const *st; } host_pump_ctx;
static void pump_host_only(void *u) { host_pump_ctx *c = u; pump(c->all, c->st, 1, on_host_rx); }

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void send_all(psp_net_enet *from, const psp_net_peer *to, psp_net_enet *const *all, rx_state *const *st, int n) {
    const psp_net_transport *t = psp_net_enet_transport(from);
    uint8_t buf[2048];
    for (uint32_t seq = 0; seq < N_PAYLOADS; seq++) {
        for (int ch = 0; ch < 2; ch++) {
            const uint32_t len = payload_len(ch, seq);
            make_payload(ch, seq, buf, len);
            CHECK(t->send_packet(t->ctx, to, ch, ADHOC_PORT, buf, len) == PSP_NET_OK);
        }
        if (seq % 16 == 0) pump(all, st, n, on_host_rx);   /* keep the queues moving, as a game tick would */
    }
}

static void wait_reliable(psp_net_enet *const *all, rx_state *const *st, int n, rx_state *dst) {
    const double t0 = now_ms();
    while (dst->got[0] < N_PAYLOADS && now_ms() - t0 < 10000) pump(all, st, n, on_host_rx);
    for (int i = 0; i < 20; i++) pump(all, st, n, on_host_rx);    /* stragglers on channel 1 */
}

static void report(const char *dir, const rx_state *s) {
    printf("  %-14s ch0 %4u/%d (bad %u, order %u)   ch1 %4u/%d (bad %u, backwards %u)\n", dir,
           s->got[0], N_PAYLOADS, s->bad[0], s->out_of_order[0], s->got[1], N_PAYLOADS, s->bad[1], s->out_of_order[1]);
}

int main(void) {
    const uint8_t mac_h[6] = { 0x02, 0, 0, 0, 0, 0x01 }, mac_a[6] = { 0x02, 0, 0, 0, 0, 0x0A }, mac_b[6] = { 0x02, 0, 0, 0, 0, 0x0B };
    psp_net_enet_config hc = { .bind_host = "127.0.0.1" };
    psp_net_enet *h = psp_net_enet_create(&hc);
    g_host_for_echo = h;
    rx_state sh = { 0 }, sa = { 0 }, sb = { 0 };
    psp_net_enet *all[3] = { h, NULL, NULL };
    rx_state *st[3] = { &sh, &sa, &sb };
    /* the clients service the host while they wait to be welcomed (one thread) */
    host_pump_ctx hp = { all, st };
    psp_net_enet_config cc = { .connect_timeout_ms = 2000, .idle = pump_host_only, .idle_user = &hp };
    psp_net_enet *a = psp_net_enet_create(&cc), *b = psp_net_enet_create(&cc);
    all[1] = a; all[2] = b;
    const psp_net_transport *th = psp_net_enet_transport(h), *ta = psp_net_enet_transport(a), *tb = psp_net_enet_transport(b);

    CHECK(th->initialize(th->ctx, mac_h) == PSP_NET_OK);
    CHECK(ta->initialize(ta->ctx, mac_a) == PSP_NET_OK);
    CHECK(tb->initialize(tb->ctx, mac_b) == PSP_NET_OK);
    CHECK(th->host_session(th->ctx, "PSP2ITST", 4) == PSP_NET_OK);
    char addr[64]; snprintf(addr, sizeof addr, "127.0.0.1:%u", psp_net_enet_port(h));
    const int joined_a = ta->join_session(ta->ctx, addr, "PSP2ITST") == PSP_NET_OK;
    const int joined_b = tb->join_session(tb->ctx, addr, "PSP2ITST") == PSP_NET_OK;
    CHECK(joined_a && joined_b);
    for (int i = 0; i < 10; i++) pump(all, st, 3, on_host_rx);
    CHECK(psp_net_enet_self_id(h) == 1);
    CHECK(psp_net_enet_self_id(a) >= 2 && psp_net_enet_self_id(b) >= 2 && psp_net_enet_self_id(a) != psp_net_enet_self_id(b));
    CHECK(psp_net_enet_peer_count(h) == 2);
    if (fails) return 1;
    printf("joined: host id 1, A id %llu, B id %llu, port %u\n",
           (unsigned long long)psp_net_enet_self_id(a), (unsigned long long)psp_net_enet_self_id(b), psp_net_enet_port(h));

    /* 1. A -> host, host -> A */
    psp_net_peer to_h = { { 0 }, 0 }; memcpy(to_h.mac, mac_h, 6);
    psp_net_peer to_a = { { 0 }, 0 }; memcpy(to_a.mac, mac_a, 6);
    psp_net_peer to_b = { { 0 }, 0 }; memcpy(to_b.mac, mac_b, 6);
    send_all(a, &to_h, all, st, 3);
    wait_reliable(all, st, 3, &sh);
    send_all(h, &to_a, all, st, 3);
    wait_reliable(all, st, 3, &sa);
    printf("payloads (%d per channel, %u-%u bytes):\n", N_PAYLOADS, 16u, 16u + 1199u);
    report("A -> host", &sh);
    report("host -> A", &sa);
    CHECK(sh.got[0] == N_PAYLOADS && sh.bad[0] == 0 && sh.out_of_order[0] == 0);
    CHECK(sa.got[0] == N_PAYLOADS && sa.bad[0] == 0 && sa.out_of_order[0] == 0);
    CHECK(sh.bad[1] == 0 && sh.out_of_order[1] == 0 && sa.bad[1] == 0 && sa.out_of_order[1] == 0);
    CHECK(sh.last_from.id == psp_net_enet_self_id(a) && memcmp(sh.last_from.mac, mac_a, 6) == 0);

    /* 3. A -> B through the relay */
    send_all(a, &to_b, all, st, 3);
    wait_reliable(all, st, 3, &sb);
    report("A -> B (relay)", &sb);
    CHECK(sb.got[0] == N_PAYLOADS && sb.bad[0] == 0 && sb.out_of_order[0] == 0);
    CHECK(sb.bad[1] == 0 && sb.out_of_order[1] == 0);
    CHECK(sb.last_from.id == psp_net_enet_self_id(a) && memcmp(sb.last_from.mac, mac_a, 6) == 0);

    /* 2. latency: A pings the host, one in flight at a time */
    double rtt[N_PINGS];
    for (uint32_t i = 0; i < N_PINGS; i++) {
        sa.pongs = 0;
        const double t0 = now_ms();
        ta->send_packet(ta->ctx, &to_h, PSP_NET_CH_RELIABLE, ADHOC_PORT + 1, &i, 4);
        while (!sa.pongs && now_ms() - t0 < 1000) pump(all, st, 2, on_host_rx);
        rtt[i] = now_ms() - t0;
        CHECK(sa.pongs == 1 && sa.pong_seq == i);
    }
    double sum = 0; for (int i = 0; i < N_PINGS; i++) sum += rtt[i];
    qsort(rtt, N_PINGS, sizeof rtt[0], cmp_d);
    const double mean = sum / N_PINGS, p99 = rtt[N_PINGS * 99 / 100], max = rtt[N_PINGS - 1];
    printf("rtt over %d pings: mean %.3f ms, p99 %.3f ms, max %.3f ms\n", N_PINGS, mean, p99, max);
    CHECK(mean < 5.0 && p99 < 5.0);

    /* 4. a forged source: a raw ENet client says HELLO as mac_x, then sends DATA
     *    whose header claims to be A. The host must deliver it as from mac_x. */
    {
        const uint8_t mac_x[6] = { 0x02, 0, 0, 0, 0, 0xEE };
        ENetHost *raw = enet_host_create(NULL, 1, 2, 0, 0);
        ENetAddress ha; enet_address_set_host_ip(&ha, "127.0.0.1"); ha.port = psp_net_enet_port(h);
        ENetPeer *rp = enet_host_connect(raw, &ha, 2, 0);
        ENetEvent ev; int welcomed = 0; const double t0 = now_ms();
        while (!welcomed && now_ms() - t0 < 2000) {
            pump(all, st, 1, on_host_rx);
            if (enet_host_service(raw, &ev, 1) > 0) {
                if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                    uint8_t hello[7] = { 1 }; memcpy(hello + 1, mac_x, 6);
                    enet_peer_send(rp, 0, enet_packet_create(hello, 7, ENET_PACKET_FLAG_RELIABLE));
                } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) { welcomed = ev.packet->data[0] == 2; enet_packet_destroy(ev.packet); }
            }
        }
        CHECK(welcomed);
        uint8_t forged[23 + 4] = { 3 };
        memcpy(forged + 1, mac_a, 6);                                  /* claimed source: A */
        const uint64_t aid = psp_net_enet_self_id(a);
        for (int i = 0; i < 8; i++) forged[7 + i] = (uint8_t)(aid >> (8 * i));
        memcpy(forged + 15, mac_h, 6);
        forged[21] = (uint8_t)ADHOC_PORT; forged[22] = (uint8_t)(ADHOC_PORT >> 8);
        enet_peer_send(rp, 0, enet_packet_create(forged, sizeof forged, ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(raw);
        memset(&sh.last_from, 0, sizeof sh.last_from);
        for (int i = 0; i < 200 && !sh.last_from.id; i++) { pump(all, st, 1, on_host_rx); enet_host_service(raw, &ev, 1); }
        CHECK(memcmp(sh.last_from.mac, mac_x, 6) == 0 && sh.last_from.id != aid && sh.last_from.id != 0);
        printf("forged source: claimed A (id %llu), host delivered it as ..:%02X (id %llu)\n",
               (unsigned long long)aid, sh.last_from.mac[5], (unsigned long long)sh.last_from.id);
        enet_peer_disconnect_now(rp, 0); enet_host_destroy(raw);
    }

    psp_net_enet_destroy(a); psp_net_enet_destroy(b); psp_net_enet_destroy(h);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("enet_transport: ok");
    return 0;
}
