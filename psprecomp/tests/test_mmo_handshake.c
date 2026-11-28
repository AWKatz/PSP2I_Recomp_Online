/* Task 1.3 integration: the game's entry into multiplayer, through the real HLE
 * dispatcher, reaches a server and completes the handshake.
 *
 *  - the WLAN switch reads on (forced), whatever the host's connection;
 *  - the PSN/NP stubs keep offline.c's answers but are traced (MMO build);
 *  - sceNetAdhocctlInit(stack, prio, product) brings the installed ENet
 *    transport up, joins the configured server and sends MSG_C2S_HANDSHAKE
 *    (MAC, player name, version); the server's MSG_S2C_HANDSHAKE_ACK arrives
 *    through psp_net_mmo_pump. */
#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/net.h"
#include "psprecomp/net_enet.h"
#include "psprecomp/net_proto.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2) {
    psp_cpu.r[PSP_REG_A0] = a0; psp_cpu.r[PSP_REG_A1] = a1; psp_cpu.r[PSP_REG_A2] = a2;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

/* the server */
static psp_net_enet *g_srv;
static int g_handshakes, g_old_server, g_refused;
static uint32_t g_hs_len;                 /* the length of the last handshake accepted */
static char g_name[PSP_MMO_NAME_LEN + 1];
static uint8_t g_mac[6];
static void srv_rx(const psp_net_packet *p, void *u) {
    (void)u;
    if (p->port != PSP_MMO_CTRL_PORT || psp_mmo_type(p->data, p->len) != MSG_C2S_HANDSHAKE) return;
    const psp_net_transport *t = psp_net_enet_transport(g_srv);
    uint8_t ack[PSP_MMO_HANDSHAKE_ACK_LEN] = { 0 };
    psp_mmo_put_hdr(ack, MSG_S2C_HANDSHAKE_ACK);
    if (p->len != PSP_MMO_HANDSHAKE_LEN && (g_old_server || p->len != PSP_MMO_HANDSHAKE_LAN_LEN)) {
        /* as an older server does: status 1 (ERR_VERSION), then the player is dropped */
        ack[PSP_MMO_HDR_LEN] = PSP_MMO_ERR_VERSION;
        t->send_packet(t->ctx, &p->from, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, ack, sizeof ack);
        psp_net_enet_disconnect(g_srv, p->from.id, 2);
        g_refused++;
        return;
    }
    g_handshakes++;
    g_hs_len = p->len;
    memcpy(g_mac, p->data + PSP_MMO_HDR_LEN, 6);
    memcpy(g_name, p->data + PSP_MMO_HDR_LEN + 6, PSP_MMO_NAME_LEN);
    for (int i = 0; i < 8; i++) ack[PSP_MMO_HDR_LEN + 4 + i] = (uint8_t)(p->from.id >> (8 * i));
    t->send_packet(t->ctx, &p->from, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, ack, sizeof ack);
}
static void srv_pump(void *u) { (void)u; const psp_net_transport *t = psp_net_enet_transport(g_srv); t->poll_packets(t->ctx, srv_rx, NULL); }

/* the client's network log: look for the ACK line */
static int g_acked, g_traced_psn;
static void log_line(const char *l) {
    if (strstr(l, "MSG_S2C_HANDSHAKE_ACK status 0")) g_acked = 1;
    if (strstr(l, "mmo: sceUtilityPsnInitStart")) g_traced_psn = 1;
}

int main(void) {
    CHECK(psp_mem_init() == 0);
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x08810000u;
    psp_hle_init();
    psp_net_set_log(log_line);

    /* the server, on a free port */
    psp_net_enet_config sc = { .bind_host = "127.0.0.1" };
    g_srv = psp_net_enet_create(&sc);
    const psp_net_transport *st = psp_net_enet_transport(g_srv);
    const uint8_t srv_mac[6] = { 0x02, 0x50, 0x32, 0x49, 0, 1 };
    CHECK(st->initialize(st->ctx, srv_mac) == PSP_NET_OK && st->host_session(st->ctx, "PSP2I", 8) == PSP_NET_OK);
    char server[64]; snprintf(server, sizeof server, "127.0.0.1:%u", psp_net_enet_port(g_srv));

    /* the client, as mmo_client_init sets it up (the idle hook services the
     * in-process server while the join waits) */
    psp_net_enet_config cc = { .connect_timeout_ms = 2000, .idle = srv_pump };
    psp_net_enet *cli = psp_net_enet_create(&cc);
    psp_net_set_transport(psp_net_enet_transport(cli));
    psp_net_mmo_config mc = { server, "Tester", "test" };
    psp_net_mmo_configure(&mc);
    psp_net_set_forced_off(1);            /* even with the host "offline"... */
    psp_net_set_wlan_forced_on(1);        /* ...the WLAN switch reads on */

    CHECK(call(0xD7763699, 0, 0, 0) == 1);                      /* sceWlanGetSwitchState */
    CHECK(call(0xA7BB7C67, 0, 0, 0) == 0x80020001u && g_traced_psn);   /* sceUtilityPsnInitStart: still unavailable, now traced */
    CHECK(call(0xE1D621D7, 0, 0, 0) == 0);                      /* sceNetAdhocInit */
    CHECK(call(0xE26F226E, 0x2000, 0x30, 0) == 0);              /* sceNetAdhocctlInit -> join + handshake */
    for (int i = 0; i < 200 && !g_handshakes; i++) srv_pump(NULL);
    CHECK(g_handshakes == 1);
    CHECK(g_hs_len == PSP_MMO_HANDSHAKE_LAN_LEN);               /* with our own address */
    CHECK(strcmp(g_name, "Tester") == 0);
    uint8_t mac[6]; psp_net_ether_addr(mac);
    CHECK(memcmp(g_mac, mac, 6) == 0);
    for (int i = 0; i < 200 && !g_acked; i++) { srv_pump(NULL); psp_net_mmo_pump(); }
    CHECK(g_acked);
    CHECK(call(0xE26F226E, 0x2000, 0x30, 0) == 0);              /* a second Init does not join twice */
    for (int i = 0; i < 50; i++) srv_pump(NULL);
    CHECK(g_handshakes == 1);

    /* An older server refuses the longer handshake: the client connects again at once with
     * the older one, and is in. */
    g_old_server = 1;
    psp_net_enet_disconnect_all(g_srv, 6);
    for (int i = 0; i < 200 && psp_net_mmo_connected(); i++) { srv_pump(NULL); psp_net_mmo_pump(); }
    CHECK(!psp_net_mmo_connected());
    g_acked = 0;
    CHECK(call(0xE26F226E, 0x2000, 0x30, 0) == 0);              /* the game's next way in: connects again */
    for (int i = 0; i < 400 && !(g_handshakes == 2 && g_acked); i++) { srv_pump(NULL); psp_net_mmo_pump(); }
    CHECK(g_refused == 1);
    CHECK(g_handshakes == 2);
    CHECK(g_hs_len == PSP_MMO_HANDSHAKE_LEN);
    CHECK(g_acked);

    psp_net_set_transport(NULL);
    psp_net_enet_destroy(cli);
    psp_net_enet_destroy(g_srv);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("mmo_handshake: ok (handshake from %02X:%02X:%02X:%02X:%02X:%02X '%s', ACK received)\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], g_name);
    return 0;
}
