/* The transport registry: the null default, install / restore, shutdown on swap. */
#include "psprecomp/net_transport.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int shutdowns, sent;
static int  t_init(void *c, const uint8_t m[6]) { (void)c; (void)m; return PSP_NET_OK; }
static int  t_host(void *c, const char *g, int n) { (void)c; (void)g; (void)n; return PSP_NET_OK; }
static int  t_join(void *c, const char *a, const char *g) { (void)c; (void)a; (void)g; return PSP_NET_OK; }
static int  t_send(void *c, const psp_net_peer *to, int ch, uint16_t port, const void *d, uint32_t len) {
    (void)c; (void)to; (void)ch; (void)port; (void)d; sent += (int)len; return PSP_NET_OK;
}
static int  t_poll(void *c, psp_net_packet_fn fn, void *u) {
    (void)c;
    static const uint8_t payload[4] = { 1, 2, 3, 4 };
    psp_net_packet p; memset(&p, 0, sizeof p);
    p.channel = PSP_NET_CH_RELIABLE; p.port = 1; p.len = 4; p.data = payload;
    fn(&p, u); return 1;
}
static void t_shutdown(void *c) { (void)c; shutdowns++; }
static const psp_net_transport FAKE = { "fake", NULL, t_init, t_host, t_join, t_send, t_poll, t_shutdown };

static void on_packet(const psp_net_packet *p, void *u) { *(uint32_t *)u += p->len; }

int main(void) {
    const uint8_t mac[6] = { 0 };
    const psp_net_transport *t = psp_net_get_transport();
    CHECK(t == psp_net_null_transport());
    CHECK(strcmp(t->name, "null") == 0);
    CHECK(t->initialize(t->ctx, mac) == PSP_NET_ERR_UNSUPPORTED);
    CHECK(t->host_session(t->ctx, "grp", 4) == PSP_NET_ERR_UNSUPPORTED);
    CHECK(t->join_session(t->ctx, "127.0.0.1:29000", "grp") == PSP_NET_ERR_UNSUPPORTED);
    CHECK(t->send_packet(t->ctx, NULL, PSP_NET_CH_RELIABLE, 1, "x", 1) == PSP_NET_ERR_UNSUPPORTED);

    psp_net_set_transport(&FAKE);
    t = psp_net_get_transport();
    CHECK(t == &FAKE);
    CHECK(t->send_packet(t->ctx, NULL, PSP_NET_CH_UNRELIABLE, 1, "abc", 3) == PSP_NET_OK && sent == 3);
    uint32_t got = 0;
    CHECK(t->poll_packets(t->ctx, on_packet, &got) == 1 && got == 4);

    psp_net_set_transport(NULL);                 /* back to null: the fake is shut down */
    CHECK(shutdowns == 1);
    CHECK(psp_net_get_transport() == psp_net_null_transport());

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("net_transport: ok");
    return 0;
}
