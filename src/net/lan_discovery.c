/* psprecomp — LAN discovery (see include/psprecomp/net_lan.h). */
#include "psprecomp/net_lan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
typedef int socklen_t;
static int net_up(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1; }
static void net_down(void) { WSACleanup(); }
static int set_nonblock(sock_t s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on); }
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
static int net_up(void) { return 0; }
static void net_down(void) {}
static int set_nonblock(sock_t s) { const int f = fcntl(s, F_GETFL, 0); return f < 0 ? -1 : fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

struct psp_mmo_lan_responder {
    sock_t   s;
    uint16_t port;
};

static sock_t udp_socket(uint16_t port, int reuse) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return BAD_SOCK;
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    if (reuse) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || set_nonblock(s) != 0) { sock_close(s); return BAD_SOCK; }
    return s;
}

psp_mmo_lan_responder *psp_mmo_lan_responder_open(uint16_t port) {
    if (net_up() != 0) return NULL;
    psp_mmo_lan_responder *r = calloc(1, sizeof *r);
    if (!r) { net_down(); return NULL; }
    r->s = udp_socket(port, 1);
    if (r->s == BAD_SOCK) { free(r); net_down(); return NULL; }
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    r->port = getsockname(r->s, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : port;
    return r;
}

uint16_t psp_mmo_lan_responder_port(const psp_mmo_lan_responder *r) { return r ? r->port : 0; }

int psp_mmo_lan_responder_poll(psp_mmo_lan_responder *r, const psp_mmo_lan_info *info) {
    if (!r) return 0;
    int answered = 0;
    for (int guard = 0; guard < 64; guard++) {          /* bounded per call: a flood cannot stall the tick */
        uint8_t b[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = (int)recvfrom(r->s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        if (n != PSP_MMO_LAN_PROBE_LEN || psp_mmo_type(b, (uint32_t)n) != MSG_LAN_PROBE) continue;
        uint8_t rep[PSP_MMO_LAN_REPLY_LEN];
        uint8_t *p = rep + PSP_MMO_HDR_LEN;
        psp_mmo_put_hdr(rep, MSG_LAN_REPLY);
        memcpy(p, b + PSP_MMO_HDR_LEN, 4);               /* the probe's nonce */
        psp_mmo_put_u16(p + 4, info->game_port);
        psp_mmo_put_u16(p + 6, info->players);
        psp_mmo_put_u16(p + 8, info->max_players);
        psp_mmo_put_u32(p + 10, info->version_hash);
        psp_mmo_put_str(p + 14, info->name, PSP_MMO_NAME_LEN);
        if (sendto(r->s, (const char *)rep, sizeof rep, 0, (struct sockaddr *)&from, fl) == (int)sizeof rep) answered++;
    }
    return answered;
}

void psp_mmo_lan_responder_close(psp_mmo_lan_responder *r) {
    if (!r) return;
    sock_close(r->s);
    free(r);
    net_down();
}

static uint32_t now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

int psp_mmo_lan_discover(uint16_t port, uint32_t timeout_ms, psp_mmo_lan_info *out, char *ip, size_t ip_cap) {
    if (net_up() != 0) return -1;
    sock_t s = udp_socket(0, 0);
    if (s == BAD_SOCK) { net_down(); return -1; }

    uint8_t probe[PSP_MMO_LAN_PROBE_LEN];
    psp_mmo_put_hdr(probe, MSG_LAN_PROBE);
    const uint32_t nonce = (uint32_t)rand() ^ now_ms() ^ (uint32_t)(uintptr_t)&probe;
    psp_mmo_put_u32(probe + PSP_MMO_HDR_LEN, nonce);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);

    int rc = -1;
    const uint32_t start = now_ms();
    while (rc != 0) {
        const uint32_t spent = now_ms() - start;
        if (spent >= timeout_ms) break;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = { (long)((timeout_ms - spent) / 1000), (long)((timeout_ms - spent) % 1000) * 1000 };
        if (select((int)s + 1, &rd, NULL, NULL, &tv) <= 0) break;
        uint8_t b[128];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = (int)recvfrom(s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n != PSP_MMO_LAN_REPLY_LEN || psp_mmo_type(b, (uint32_t)n) != MSG_LAN_REPLY) continue;
        const uint8_t *p = b + PSP_MMO_HDR_LEN;
        if (psp_mmo_get_u32(p) != nonce) continue;
        memset(out, 0, sizeof *out);
        out->game_port = psp_mmo_get_u16(p + 4);
        out->players = psp_mmo_get_u16(p + 6);
        out->max_players = psp_mmo_get_u16(p + 8);
        out->version_hash = psp_mmo_get_u32(p + 10);
        memcpy(out->name, p + 14, PSP_MMO_NAME_LEN);
        out->name[PSP_MMO_NAME_LEN] = 0;
        if (ip && ip_cap) inet_ntop(AF_INET, &from.sin_addr, ip, (socklen_t)ip_cap);
        rc = 0;
    }
    sock_close(s);
    net_down();
    return rc;
}
