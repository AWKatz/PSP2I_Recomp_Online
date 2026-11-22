/* mmo_listen [port] [seconds] — a stand-in server for checking the client's
 * handshake (Task 1.3) until the dedicated server exists (Phase 2).
 * Hosts on 0.0.0.0:port, prints every MSG_C2S_HANDSHAKE, answers with
 * MSG_S2C_HANDSHAKE_ACK. Exits 0 after `seconds` if at least one handshake
 * arrived (or right after the first one when seconds = 0), else 1. */
#include "psprecomp/net_enet.h"
#include "psprecomp/net_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static psp_net_enet *g_e;
static int g_handshakes;

static void on_packet(const psp_net_packet *p, void *user) {
    (void)user;
    if (p->port != PSP_MMO_CTRL_PORT || psp_mmo_type(p->data, p->len) != MSG_C2S_HANDSHAKE || p->len < PSP_MMO_HANDSHAKE_LEN) {
        printf("mmo_listen: %u bytes on port %u from id %llu (not a handshake)\n", p->len, p->port, (unsigned long long)p->from.id);
        return;
    }
    const uint8_t *d = p->data + PSP_MMO_HDR_LEN;
    char name[PSP_MMO_NAME_LEN + 1] = { 0 }, ver[PSP_MMO_VERSION_LEN + 1] = { 0 };
    memcpy(name, d + 6, PSP_MMO_NAME_LEN);
    memcpy(ver, d + 6 + PSP_MMO_NAME_LEN, PSP_MMO_VERSION_LEN);
    printf("mmo_listen: MSG_C2S_HANDSHAKE from id %llu mac %02X:%02X:%02X:%02X:%02X:%02X name '%s' version '%s'\n",
           (unsigned long long)p->from.id, d[0], d[1], d[2], d[3], d[4], d[5], name, ver);
    fflush(stdout);
    g_handshakes++;

    uint8_t ack[PSP_MMO_HANDSHAKE_ACK_LEN];
    psp_mmo_put_hdr(ack, MSG_S2C_HANDSHAKE_ACK);
    memset(ack + PSP_MMO_HDR_LEN, 0, 4);                                        /* status 0: accepted */
    for (int i = 0; i < 8; i++) ack[PSP_MMO_HDR_LEN + 4 + i] = (uint8_t)(p->from.id >> (8 * i));   /* session = peer id */
    const psp_net_transport *t = psp_net_enet_transport(g_e);
    t->send_packet(t->ctx, &p->from, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, ack, sizeof ack);
}

static void log_line(const char *line) { printf("%s\n", line); fflush(stdout); }

int main(int argc, char **argv) {
    const int port = argc > 1 ? atoi(argv[1]) : 29001;
    const int seconds = argc > 2 ? atoi(argv[2]) : 60;
    psp_net_enet_config cfg = { NULL, (uint16_t)port, 0, log_line, NULL, NULL };
    g_e = psp_net_enet_create(&cfg);
    const psp_net_transport *t = psp_net_enet_transport(g_e);
    const uint8_t server_mac[6] = { 0x02, 0x50, 0x32, 0x49, 0x00, 0x01 };
    if (t->initialize(t->ctx, server_mac) != PSP_NET_OK || t->host_session(t->ctx, "PSP2I", 33) != PSP_NET_OK) {
        fprintf(stderr, "mmo_listen: cannot host on port %d\n", port);
        return 2;
    }
    printf("mmo_listen: listening on udp %u\n", psp_net_enet_port(g_e)); fflush(stdout);
    const time_t end = time(NULL) + seconds;
    while (seconds == 0 ? !g_handshakes : time(NULL) < end) {
        t->poll_packets(t->ctx, on_packet, NULL);
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < 20; i++) { t->poll_packets(t->ctx, on_packet, NULL); struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL); }
    psp_net_enet_destroy(g_e);
    return g_handshakes ? 0 : 1;
}
