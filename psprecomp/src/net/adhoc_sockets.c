/* psprecomp — ad hoc sockets over the MMO transport (ENABLE_MMO_NET):
 * PDP (datagrams) and PTP (streams), the API of sceNetAdhoc.
 *
 * Every message travels on the transport with the destination ad hoc port
 * as its port, and starts with: kind u8, source port u16 (little-endian).
 *   PDP      datagram (unreliable channel)
 *   SYN      a PTP connect; a listener on the port answers SYNACK at once
 *            (the connection then waits in its backlog for PtpAccept), or
 *            RST when nothing listens
 *   DATA     PTP bytes (reliable, in order: the transport guarantees both)
 *   FIN      the peer closed
 * Blocking calls wait the way the firmware's do: they poll the network and
 * sleep the calling PSP thread (psp_sched_sleep_until), so the game's other
 * threads keep running, until data arrives or the timeout passes. */

#include "adhoc_internal.h"

#include "psprecomp/hle.h"
#include "psprecomp/net.h"
#include "psprecomp/net_transport.h"

#include <stdlib.h>
#include <psprecomp/cpu.h>
#include <string.h>

#define E_INVALID_SOCKET  0x80410701u
#define E_INVALID_ADDR    0x80410702u
#define E_INVALID_PORT    0x80410703u
#define E_INVALID_DATALEN 0x80410705u
#define E_WOULD_BLOCK     0x80410709u
#define E_PORT_IN_USE     0x8041070Au
#define E_NOT_CONNECTED   0x8041070Bu
#define E_DISCONNECTED    0x8041070Cu
#define E_ID_NOT_AVAIL    0x8041070Fu
#define E_INVALID_ARG     0x80410711u
#define E_TIMEOUT         0x80410715u
#define E_REFUSED         0x80410718u

enum { K_PDP = 0x41, K_SYN, K_SYNACK, K_DATA, K_FIN, K_RST };   /* distinct from matching message types */
enum { T_PDP = 1, T_PTP };
enum { S_CLOSED = 0, S_LISTEN = 1, S_SYN_SENT = 2, S_SYN_RECEIVED = 3, S_ESTABLISHED = 4 };

#define MAX_SOCKS 64
#define HDR 3
#define CHUNK (PSP_NET_MAX_PAYLOAD - HDR)

typedef struct { uint8_t mac[6]; uint16_t port; uint32_t len; uint8_t *data; } dgram;

typedef struct {
    int used, type, state, alert;
    uint16_t lport, pport;               /* local, peer */
    uint8_t pmac[6];                     /* PTP peer */
    uint32_t bufsize;
    /* PDP: queued datagrams; PTP: a byte ring */
    dgram *q; int qn, qcap;
    uint8_t *rb; uint32_t rlen;          /* PTP received bytes (rlen used of bufsize) */
    int peer_closed, refused;
    /* PTP listener: established connections waiting for Accept */
    int backlog[16]; int nback, maxback;
} sock;

static sock S[MAX_SOCKS];                /* id = index + 1 */
static uint16_t g_next_eph = 0x9000;
static int trace_on(void) { static int on = -1; if (on < 0) on = getenv("PSP2I_NETTRACE") != NULL; return on; }
static uint64_t g_sent[3], g_recv[3];      /* netprof: [0] PDP, [1] PTP data, [2] PTP data with no socket */

static sock *get(uint32_t id) { return id >= 1 && id <= MAX_SOCKS && S[id - 1].used ? &S[id - 1] : NULL; }
static int new_sock(void) { for (int i = 0; i < MAX_SOCKS; i++) if (!S[i].used) { memset(&S[i], 0, sizeof S[i]); S[i].used = 1; return i + 1; } return 0; }

static int port_used(int type, uint16_t port) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (S[i].used && S[i].type == type && S[i].lport == port && (type == T_PDP || S[i].state == S_LISTEN)) return 1;
    return 0;
}
static uint16_t ephemeral(int type) {
    for (int n = 0; n < 0x1000; n++) { uint16_t p = g_next_eph++; if (g_next_eph >= 0xA000) g_next_eph = 0x9000; if (!port_used(type, p)) return p; }
    return 0;
}

static int send_msg(const uint8_t mac[6], uint16_t dport, int kind, uint16_t sport, const void *data, uint32_t len, int reliable) {
    uint8_t buf[PSP_NET_MAX_PAYLOAD];
    if (len > CHUNK) len = CHUNK;
    buf[0] = (uint8_t)kind; buf[1] = (uint8_t)sport; buf[2] = (uint8_t)(sport >> 8);
    if (len) memcpy(buf + HDR, data, len);
    uint8_t self[6];
    psp_net_ether_addr(self);
    if (!memcmp(mac, self, 6)) {                       /* to ourselves (the host uses its own listeners): no network */
        psp_net_packet p;
        memcpy(p.from.mac, self, 6);
        p.from.id = 0;
        p.channel = PSP_NET_CH_RELIABLE;
        p.port = dport;
        p.len = len + HDR;
        p.data = buf;
        mmo_sockets_on_packet(&p);
        return PSP_NET_OK;
    }
    if (!mmo_net_ready()) return -1;
    const psp_net_transport *t = psp_net_get_transport();
    psp_net_peer to;
    const int bcast = mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF && mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
    if (!bcast) { memcpy(to.mac, mac, 6); to.id = 0; }
    return t->send_packet(t->ctx, bcast ? NULL : &to, reliable ? PSP_NET_CH_RELIABLE : PSP_NET_CH_UNRELIABLE, dport, buf, len + HDR);
}

/* the connected PTP socket for (local port, peer mac, peer port) */
static sock *ptp_conn(uint16_t lport, const uint8_t mac[6], uint16_t pport) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock *s = &S[i];
        if (s->used && s->type == T_PTP && s->state != S_LISTEN && s->lport == lport && s->pport == pport && !memcmp(s->pmac, mac, 6)) return s;
    }
    return NULL;
}

/* ---- incoming (adhoc_mmo.c routes every non-control, non-matching packet here) ---- */

void mmo_sockets_on_packet(const psp_net_packet *p) {
    if (p->len < HDR) return;
    const int kind = p->data[0];
    const uint16_t sport = (uint16_t)(p->data[1] | p->data[2] << 8), dport = p->port;
    const uint8_t *d = p->data + HDR;
    const uint32_t n = p->len - HDR;
    const uint8_t *mac = p->from.mac;
    if (kind == K_PDP) g_recv[0] += n;
    if (kind == K_DATA) { if (ptp_conn(dport, mac, sport)) g_recv[1] += n; else g_recv[2] += n; }
    if (kind == K_PDP) {
        for (int i = 0; i < MAX_SOCKS; i++) {
            sock *s = &S[i];
            if (!s->used || s->type != T_PDP || s->lport != dport) continue;
            if (s->qn == s->qcap) { int nc = s->qcap ? s->qcap * 2 : 16; if (nc > 256) return; dgram *nq = realloc(s->q, (size_t)nc * sizeof *nq); if (!nq) return; s->q = nq; s->qcap = nc; }
            dgram *g = &s->q[s->qn];
            g->data = malloc(n ? n : 1);
            if (!g->data) return;
            memcpy(g->mac, mac, 6); g->port = sport; g->len = n; memcpy(g->data, d, n);
            s->qn++;
            return;
        }
        return;
    }
    if (kind == K_SYN) {
        sock *l = NULL;
        for (int i = 0; i < MAX_SOCKS && !l; i++) if (S[i].used && S[i].type == T_PTP && S[i].state == S_LISTEN && S[i].lport == dport) l = &S[i];
        if (!l || l->nback >= l->maxback) { send_msg(mac, sport, K_RST, dport, NULL, 0, 1); return; }
        if (ptp_conn(dport, mac, sport)) return;                    /* a repeated SYN */
        const int id = new_sock();
        if (!id) { send_msg(mac, sport, K_RST, dport, NULL, 0, 1); return; }
        sock *c = &S[id - 1];
        c->type = T_PTP; c->state = S_ESTABLISHED; c->lport = dport; c->pport = sport; memcpy(c->pmac, mac, 6);
        c->bufsize = l->bufsize; c->rb = malloc(c->bufsize);
        l->backlog[l->nback++] = id;
        send_msg(mac, sport, K_SYNACK, dport, NULL, 0, 1);
        return;
    }
    sock *s = ptp_conn(dport, mac, sport);
    if (kind == K_SYNACK) {
        /* the reply to our connect: find our SYN_SENT socket on dport to this mac (peer port fixed now) */
        for (int i = 0; i < MAX_SOCKS && !s; i++)
            if (S[i].used && S[i].type == T_PTP && S[i].state == S_SYN_SENT && S[i].lport == dport && !memcmp(S[i].pmac, mac, 6) && S[i].pport == sport) s = &S[i];
        if (s) s->state = S_ESTABLISHED;
        return;
    }
    if (!s) return;
    if (kind == K_RST && s->state == S_SYN_SENT) { s->refused = 1; s->state = S_CLOSED; return; }
    if (kind == K_DATA && s->state == S_ESTABLISHED) {
        uint32_t room = s->bufsize - s->rlen, take = n < room ? n : room;   /* overflow: the rest is lost (the game sized the buffer) */
        if (take && s->rb) { memcpy(s->rb + s->rlen, d, take); s->rlen += take; }
        return;
    }
    if (kind == K_FIN || kind == K_RST) s->peer_closed = 1;
}

/* ---- waiting ---- */

/* PSP2I_NETPROF=1: every 3 s, per call, how often it ran and how long it blocked. */
static struct { uint32_t nid; uint64_t calls, blocked_us, timeouts; } g_prof[24];
static uint64_t g_prof_t;
static void prof(uint64_t us, int timed_out) {
    static int on = -1;
    if (on < 0) on = getenv("PSP2I_NETPROF") != NULL;
    if (!on) return;
    const uint32_t nid = psp_hle_current_nid();
    for (int i = 0; i < 24; i++) {
        if (g_prof[i].nid && g_prof[i].nid != nid) continue;
        g_prof[i].nid = nid; g_prof[i].calls++; g_prof[i].blocked_us += us; g_prof[i].timeouts += timed_out;
        break;
    }
    const uint64_t now = psp_sched_now_us();
    if (now - g_prof_t < 3000000) return;
    g_prof_t = now;
    psp_net_log("netprof: bytes sent PDP %llu PTP %llu; received PDP %llu PTP %llu, PTP with no socket %llu",
                (unsigned long long)g_sent[0], (unsigned long long)g_sent[1], (unsigned long long)g_recv[0],
                (unsigned long long)g_recv[1], (unsigned long long)g_recv[2]);
    memset(g_sent, 0, sizeof g_sent); memset(g_recv, 0, sizeof g_recv);
    for (int i = 0; i < 24 && g_prof[i].nid; i++) {
        const char *n = psp_hle_name_of(g_prof[i].nid);
        psp_net_log("netprof: %-26s %6llu calls, %5llu timeouts, blocked %7.1f ms", n ? n : "?", (unsigned long long)g_prof[i].calls,
                    (unsigned long long)g_prof[i].timeouts, (double)g_prof[i].blocked_us / 1000.0);
        g_prof[i].calls = g_prof[i].blocked_us = g_prof[i].timeouts = 0;
    }
}

/* Wait until ready() or the timeout (microseconds, 0 = forever) passes; nonblock: one check. */
static int wait_for(int (*ready)(sock *), sock *s, uint32_t timeout_us, int nonblock) {
    const uint64_t t0 = psp_sched_now_us();
    mmo_net_poll();
    if (ready(s)) { prof(0, 0); return 1; }
    if (nonblock) { prof(0, 1); return 0; }
    const uint64_t end = timeout_us ? t0 + timeout_us : 0;
    for (;;) {
        psp_sched_sleep_until(psp_sched_now_us() + 1000);
        mmo_net_poll();
        if (!s->used || ready(s)) { prof(psp_sched_now_us() - t0, 0); return s->used; }
        if (end && psp_sched_now_us() >= end) { prof(psp_sched_now_us() - t0, 1); return 0; }
    }
}

static int r_pdp(sock *s) { return s->qn > 0; }
static int r_ptp_data(sock *s) { return s->rlen > 0 || s->peer_closed; }
static int r_connected(sock *s) { return s->state != S_SYN_SENT; }
static int r_backlog(sock *s) { return s->nback > 0; }

/* ---- PDP ---- */

/* PdpCreate(mac*, port, bufsize, unk) */
static void hle_PdpCreate(void) {
    uint16_t port = (uint16_t)psp_arg(1);
    if (!port) port = ephemeral(T_PDP);
    if (port_used(T_PDP, port)) { psp_ret(E_PORT_IN_USE); return; }
    const int id = new_sock();
    if (!id) { psp_ret(E_ID_NOT_AVAIL); return; }
    sock *s = &S[id - 1];
    s->type = T_PDP; s->lport = port; s->bufsize = psp_arg(2);
    psp_net_log("mmo: sceNetAdhocPdpCreate(port %u, buf %u) -> %d", port, s->bufsize, id);
    psp_ret((uint32_t)id);
}

/* PdpSend(id, destmac*, port, data*, len, timeout, flag) */
static void hle_PdpSend(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PDP) { psp_ret(E_INVALID_SOCKET); return; }
    const uint32_t len = psp_arg(4);
    if (len > CHUNK) { psp_ret(E_INVALID_DATALEN); return; }
    uint8_t mac[6], data[PSP_NET_MAX_PAYLOAD];
    if (!psp_arg(1)) { psp_ret(E_INVALID_ADDR); return; }
    psp_mem_read_block(mac, psp_arg(1), 6);
    if (len) psp_mem_read_block(data, psp_arg(3), len);
    send_msg(mac, (uint16_t)psp_arg(2), K_PDP, s->lport, data, len, 0);
    g_sent[0] += len;
    psp_ret(0);
}

/* PdpRecv(id, srcmac*, port*, buf*, len*, timeout, flag) */
static void hle_PdpRecv(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PDP) { psp_ret(E_INVALID_SOCKET); return; }
    const uint32_t macp = psp_arg(1), portp = psp_arg(2), buf = psp_arg(3), lenp = psp_arg(4);
    if (!wait_for(r_pdp, s, psp_arg(5), psp_arg(6) != 0)) { psp_ret(psp_arg(6) ? E_WOULD_BLOCK : E_TIMEOUT); return; }
    dgram g = s->q[0];
    const uint32_t cap = lenp ? psp_read32(lenp) : 0;
    if (cap < g.len) {                                  /* too small: the firmware reports the size and keeps it */
        if (lenp) psp_write32(lenp, g.len);
        psp_ret(E_INVALID_DATALEN);
        return;
    }
    memmove(s->q, s->q + 1, (size_t)(s->qn - 1) * sizeof *s->q);
    s->qn--;
    if (buf && g.len) psp_mem_write_block(buf, g.data, g.len);
    if (lenp) psp_write32(lenp, g.len);
    if (macp) psp_mem_write_block(macp, g.mac, 6);
    if (portp) { psp_write8(portp, (uint8_t)g.port); psp_write8(portp + 1, (uint8_t)(g.port >> 8)); }
    free(g.data);
    psp_ret(0);
}

static void free_sock(sock *s) {
    for (int i = 0; i < s->qn; i++) free(s->q[i].data);
    free(s->q); free(s->rb);
    memset(s, 0, sizeof *s);
}

static void hle_PdpDelete(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PDP) { psp_ret(E_INVALID_SOCKET); return; }
    free_sock(s);
    psp_ret(0);
}

/* GetPdpStat(int *buflen, SceNetAdhocPdpStat *buf): next, id, mac[6], port u16, rcv_sb_cc */
static void hle_GetPdpStat(void) {
    const uint32_t lenp = psp_arg(0), buf = psp_arg(1);
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; i++) n += S[i].used && S[i].type == T_PDP;
    if (!lenp) { psp_ret(E_INVALID_ARG); return; }
    if (!buf) { psp_write32(lenp, (uint32_t)n * 20); psp_ret(0); return; }
    const uint32_t cap = psp_read32(lenp) / 20;
    uint8_t mac[6];
    psp_net_ether_addr(mac);
    uint32_t w = 0;
    for (int i = 0; i < MAX_SOCKS && w < cap; i++) {
        sock *s = &S[i];
        if (!s->used || s->type != T_PDP) continue;
        const uint32_t e = buf + w * 20;
        uint32_t queued = 0;
        for (int k = 0; k < s->qn; k++) queued += s->q[k].len;
        psp_write32(e, 0); psp_write32(e + 4, (uint32_t)(i + 1));
        psp_mem_write_block(e + 8, mac, 6);
        psp_write8(e + 14, (uint8_t)s->lport); psp_write8(e + 15, (uint8_t)(s->lport >> 8));
        psp_write32(e + 16, queued);
        if (w) psp_write32(e - 20, e);                     /* link the previous entry */
        w++;
    }
    psp_write32(lenp, w * 20);
    psp_ret(0);
}

/* ---- PTP ---- */

/* PtpOpen(srcmac*, srcport, destmac*, destport, bufsize, rexmt_int, rexmt_cnt, flag) */
static void hle_PtpOpen(void) {
    if (!psp_arg(2)) { psp_ret(E_INVALID_ADDR); return; }
    uint16_t lport = (uint16_t)psp_arg(1);
    if (!lport) lport = ephemeral(T_PTP);
    const int id = new_sock();
    if (!id) { psp_ret(E_ID_NOT_AVAIL); return; }
    sock *s = &S[id - 1];
    s->type = T_PTP; s->state = S_CLOSED; s->lport = lport; s->pport = (uint16_t)psp_arg(3);
    psp_mem_read_block(s->pmac, psp_arg(2), 6);
    s->bufsize = psp_arg(4) ? psp_arg(4) : 8192;
    s->rb = malloc(s->bufsize);
    psp_net_log("mmo: sceNetAdhocPtpOpen(%u -> %02X:%02X:%02X:%02X:%02X:%02X:%u) -> %d", lport,
                s->pmac[0], s->pmac[1], s->pmac[2], s->pmac[3], s->pmac[4], s->pmac[5], s->pport, id);
    psp_ret((uint32_t)id);
}

/* PtpConnect(id, timeout, flag) */
static void hle_PtpConnect(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PTP || s->state == S_LISTEN) { psp_ret(E_INVALID_SOCKET); return; }
    if (s->state == S_ESTABLISHED) { psp_ret(0); return; }
    if (s->state == S_CLOSED) { s->refused = 0; s->state = S_SYN_SENT; send_msg(s->pmac, s->pport, K_SYN, s->lport, NULL, 0, 1); }
    if (!wait_for(r_connected, s, psp_arg(1), psp_arg(2) != 0)) { psp_ret(psp_arg(2) ? E_WOULD_BLOCK : E_TIMEOUT); return; }
    if (s->refused || s->state != S_ESTABLISHED) { s->state = S_CLOSED; psp_ret(E_REFUSED); return; }
    psp_net_log("mmo: sceNetAdhocPtpConnect(%d): established", (int)psp_arg(0));
    psp_ret(0);
}

/* PtpListen(srcmac*, srcport, bufsize, rexmt_int, rexmt_cnt, backlog, flag) */
static void hle_PtpListen(void) {
    const uint16_t port = (uint16_t)psp_arg(1);
    if (!port) { psp_ret(E_INVALID_PORT); return; }
    if (port_used(T_PTP, port)) { psp_ret(E_PORT_IN_USE); return; }
    const int id = new_sock();
    if (!id) { psp_ret(E_ID_NOT_AVAIL); return; }
    sock *s = &S[id - 1];
    s->type = T_PTP; s->state = S_LISTEN; s->lport = port;
    s->bufsize = psp_arg(2) ? psp_arg(2) : 8192;
    s->maxback = (int)psp_arg(5); if (s->maxback < 1 || s->maxback > 16) s->maxback = 16;
    psp_net_log("mmo: sceNetAdhocPtpListen(port %u, buf %u, backlog %d) -> %d", port, s->bufsize, s->maxback, id);
    psp_ret((uint32_t)id);
}

/* PtpAccept(id, mac*, port*, timeout, flag) */
static void hle_PtpAccept(void) {
    sock *l = get(psp_arg(0));
    if (!l || l->type != T_PTP || l->state != S_LISTEN) { psp_ret(E_INVALID_SOCKET); return; }
    if (!wait_for(r_backlog, l, psp_arg(3), psp_arg(4) != 0)) { psp_ret(psp_arg(4) ? E_WOULD_BLOCK : E_TIMEOUT); return; }
    const int id = l->backlog[0];
    memmove(l->backlog, l->backlog + 1, (size_t)(l->nback - 1) * sizeof l->backlog[0]);
    l->nback--;
    sock *c = &S[id - 1];
    if (psp_arg(1)) psp_mem_write_block(psp_arg(1), c->pmac, 6);
    if (psp_arg(2)) { psp_write8(psp_arg(2), (uint8_t)c->pport); psp_write8(psp_arg(2) + 1, (uint8_t)(c->pport >> 8)); }
    psp_net_log("mmo: sceNetAdhocPtpAccept(%d) -> %d from %02X:%02X:%02X:%02X:%02X:%02X:%u", (int)psp_arg(0), id,
                c->pmac[0], c->pmac[1], c->pmac[2], c->pmac[3], c->pmac[4], c->pmac[5], c->pport);
    psp_ret((uint32_t)id);
}

/* PtpSend(id, data*, int *len, timeout, flag) */
static void hle_PtpSend(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PTP) { psp_ret(E_INVALID_SOCKET); return; }
    if (s->state != S_ESTABLISHED) { psp_ret(E_NOT_CONNECTED); return; }
    if (s->peer_closed) { psp_ret(E_DISCONNECTED); return; }
    const uint32_t lenp = psp_arg(2);
    uint32_t len = lenp ? psp_read32(lenp) : 0, sent = 0;
    if (trace_on()) psp_net_log("trace: PtpSend(%u) len %u (state %d, peer_closed %d)", psp_arg(0), len, s->state, s->peer_closed);
    uint8_t chunk[CHUNK];
    {   /* research (PSP2I_PKT_TRACE=<cmd><sub> hex, e.g. 0105): the game's stack
         * when it sends a session message of that type */
        static int want = -2;
        if (want == -2) { const char *e = getenv("PSP2I_PKT_TRACE"); want = !e ? -1 : !strcmp(e, "all") ? 0x10000 : (int)strtol(e, NULL, 16); }
        static uint8_t seen[0x10000];
        if (want >= 0 && len >= 8 && len <= 4096) {
            const uint32_t b = psp_arg(1);
            for (uint32_t k = 0; k + 6 <= len;) {
                const uint32_t ml = psp_read32(b + k);
                const int type = (psp_read8(b + k + 4) << 8) | psp_read8(b + k + 5);
                if (type == want || (want == 0x10000 && !seen[type])) {
                    seen[type] = 1;
                    char line[900];
                    int o = snprintf(line, sizeof line, "pkt: send %04X (%u bytes) stack:", type, ml);
                    for (uint32_t q = 0; q < 0x300 && o < (int)sizeof line - 12; q += 4) {
                        const uint32_t w = psp_read32(psp_cpu.r[29] + q);
                        if (w >= 0x08804000u && w < 0x08DF0000u && !(w & 3)) o += snprintf(line + o, sizeof line - o, " %08X", w);
                    }
                    psp_net_log("%s", line);
                }
                if (ml < 8 || ml > len - k) break;
                k += ml;
            }
        }
    }
    while (sent < len) {
        const uint32_t n = len - sent < CHUNK ? len - sent : CHUNK;
        psp_mem_read_block(chunk, psp_arg(1) + sent, n);
        g_sent[1] += n;
        if (send_msg(s->pmac, s->pport, K_DATA, s->lport, chunk, n, 1) != PSP_NET_OK) break;
        sent += n;
    }
    if (lenp) psp_write32(lenp, sent);
    psp_ret(sent || !len ? 0 : E_DISCONNECTED);
}

/* PtpRecv(id, buf*, int *len, timeout, flag) */
static void hle_PtpRecv(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PTP) { psp_ret(E_INVALID_SOCKET); return; }
    if (s->state != S_ESTABLISHED) { psp_ret(E_NOT_CONNECTED); return; }
    const uint32_t lenp = psp_arg(2);
    if (!wait_for(r_ptp_data, s, psp_arg(3), psp_arg(4) != 0)) { psp_ret(psp_arg(4) ? E_WOULD_BLOCK : E_TIMEOUT); return; }
    if (!s->rlen && s->peer_closed) { psp_ret(E_DISCONNECTED); return; }
    const uint32_t cap = lenp ? psp_read32(lenp) : 0, n = cap < s->rlen ? cap : s->rlen;
    if (n) psp_mem_write_block(psp_arg(1), s->rb, n);
    memmove(s->rb, s->rb + n, s->rlen - n);
    s->rlen -= n;
    if (lenp) psp_write32(lenp, n);
    psp_ret(0);
}

static void hle_PtpFlush(void) { mmo_net_poll(); psp_ret(get(psp_arg(0)) ? 0 : E_INVALID_SOCKET); }

static void hle_PtpClose(void) {
    sock *s = get(psp_arg(0));
    if (!s || s->type != T_PTP) { psp_ret(E_INVALID_SOCKET); return; }
    if (s->state == S_ESTABLISHED && !s->peer_closed) send_msg(s->pmac, s->pport, K_FIN, s->lport, NULL, 0, 1);
    if (s->state == S_LISTEN)                             /* connections never accepted go too */
        for (int i = 0; i < s->nback; i++) { sock *c = &S[s->backlog[i] - 1]; send_msg(c->pmac, c->pport, K_FIN, c->lport, NULL, 0, 1); free_sock(c); }
    free_sock(s);
    psp_ret(0);
}

/* GetPtpStat(int *buflen, SceNetAdhocPtpStat *buf): next, id, laddr[6], paddr[6], lport, pport, snd_sb_cc, rcv_sb_cc, state */
static void hle_GetPtpStat(void) {
    const uint32_t lenp = psp_arg(0), buf = psp_arg(1);
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; i++) n += S[i].used && S[i].type == T_PTP;
    if (!lenp) { psp_ret(E_INVALID_ARG); return; }
    if (!buf) { psp_write32(lenp, (uint32_t)n * 36); psp_ret(0); return; }
    const uint32_t cap = psp_read32(lenp) / 36;
    uint8_t mac[6];
    psp_net_ether_addr(mac);
    uint32_t w = 0;
    for (int i = 0; i < MAX_SOCKS && w < cap; i++) {
        sock *s = &S[i];
        if (!s->used || s->type != T_PTP) continue;
        const uint32_t e = buf + w * 36;
        psp_write32(e, 0); psp_write32(e + 4, (uint32_t)(i + 1));
        psp_mem_write_block(e + 8, mac, 6);
        psp_mem_write_block(e + 14, s->pmac, 6);
        psp_write8(e + 20, (uint8_t)s->lport); psp_write8(e + 21, (uint8_t)(s->lport >> 8));
        psp_write8(e + 22, (uint8_t)s->pport); psp_write8(e + 23, (uint8_t)(s->pport >> 8));
        psp_write32(e + 24, 0); psp_write32(e + 28, s->rlen); psp_write32(e + 32, (uint32_t)s->state);
        if (w) psp_write32(e - 36, e);
        w++;
    }
    psp_write32(lenp, w * 36);
    psp_ret(0);
}

static void hle_SetSocketAlert(void) {
    sock *s = get(psp_arg(0));
    if (s) s->alert = (int)psp_arg(1);
    psp_ret(s ? 0 : E_INVALID_SOCKET);
}


/* PSP2I_NETTRACE=1: every socket call with its arguments and result. */
#define TRACED(fn) static void fn##_traced(void) { \
    const uint32_t a0 = psp_arg(0), a1 = psp_arg(1), a2 = psp_arg(2), a3 = psp_arg(3), a4 = psp_arg(4); \
    fn(); \
    if (trace_on()) psp_net_log("trace: %s(0x%X, 0x%X, 0x%X, 0x%X, 0x%X) -> 0x%08X", &(#fn)[4], a0, a1, a2, a3, a4, psp_cpu.r[PSP_REG_V0]); }
TRACED(hle_PdpCreate)
TRACED(hle_PdpSend)
TRACED(hle_PdpRecv)
TRACED(hle_PdpDelete)
TRACED(hle_GetPdpStat)
TRACED(hle_PtpOpen)
TRACED(hle_PtpListen)
TRACED(hle_PtpAccept)
TRACED(hle_PtpConnect)
TRACED(hle_PtpSend)
TRACED(hle_PtpRecv)
TRACED(hle_PtpFlush)
TRACED(hle_PtpClose)
TRACED(hle_GetPtpStat)
TRACED(hle_SetSocketAlert)

void mmo_sockets_register(void) {
    psp_hle_register(0x6F92741B, "sceNetAdhoc", "sceNetAdhocPdpCreate", hle_PdpCreate_traced);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend", hle_PdpSend_traced);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv", hle_PdpRecv_traced);
    psp_hle_register(0x7F27BB5E, "sceNetAdhoc", "sceNetAdhocPdpDelete", hle_PdpDelete_traced);
    psp_hle_register(0xC7C1FC57, "sceNetAdhoc", "sceNetAdhocGetPdpStat", hle_GetPdpStat_traced);
    psp_hle_register(0x877F6D66, "sceNetAdhoc", "sceNetAdhocPtpOpen", hle_PtpOpen_traced);
    psp_hle_register(0xE08BDAC1, "sceNetAdhoc", "sceNetAdhocPtpListen", hle_PtpListen_traced);
    psp_hle_register(0x9DF81198, "sceNetAdhoc", "sceNetAdhocPtpAccept", hle_PtpAccept_traced);
    psp_hle_register(0xFC6FC07B, "sceNetAdhoc", "sceNetAdhocPtpConnect", hle_PtpConnect_traced);
    psp_hle_register(0x4DA4C788, "sceNetAdhoc", "sceNetAdhocPtpSend", hle_PtpSend_traced);
    psp_hle_register(0x8BEA2B3E, "sceNetAdhoc", "sceNetAdhocPtpRecv", hle_PtpRecv_traced);
    psp_hle_register(0x9AC2EEAC, "sceNetAdhoc", "sceNetAdhocPtpFlush", hle_PtpFlush_traced);
    psp_hle_register(0x157E6225, "sceNetAdhoc", "sceNetAdhocPtpClose", hle_PtpClose_traced);
    psp_hle_register(0xB9685118, "sceNetAdhoc", "sceNetAdhocGetPtpStat", hle_GetPtpStat_traced);
    psp_hle_register(0x73BFD52D, "sceNetAdhoc", "sceNetAdhocSetSocketAlert", hle_SetSocketAlert_traced);
}
