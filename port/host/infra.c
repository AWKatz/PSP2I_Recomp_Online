/* Stand-in for SEGA's infrastructure server (Internet Multi Mode), MMO builds.
 *
 * The game's TPsuInfraClient talks to SEGA's front server with binary
 * packets POSTed to /psp2i/jp/cgi/front.fcgi (sceHttp, HTTPS). Those servers
 * are gone; this module is installed as the sceHttp transport and answers
 * the packets itself. See docs/infra_protocol.md for the layouts.
 *
 * Packets (little endian):
 *   +0  u32 length (whole packet)
 *   +4  u8  com      (requests 0x1E, replies 0x1F)
 *   +5  u8  sub
 *   +6  u8  [2]      (echoed)
 *   replies only:
 *   +8  s16 status   (0 or 200 = ok)
 *   +10 s16 error    (0 = none)
 *   +12 payload
 * The client dispatches replies in ExecCom (0x08A6B08C) through the com/sub
 * table at 0x08E38590.
 *
 * PSP2I_GAMELOG=1 also logs the game's own debug printf (0x0886EA30), which
 * narrates the infra client's steps. */
#include "infra.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/net.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN_GAME_PRINTF 0x0886EA30u

#define COM_REQUEST 0x1E
#define COM_REPLY   0x1F
#define SUB_FRONT   0x80          /* the first request: server info */
#define SUB_FRONT_REPLY 0x81      /* -> SetServerInfo (0x08A6ADEC) */

#define SUB_LOGIN       0x10      /* +8 4 bytes, +12 u32 ticket length, +16 the NP ticket */
#define SUB_LOGIN_REPLY 0x01      /* -> RcvInfraLogin (0x08A62F5C): 64-byte payload */

#define SUB_PATCH_FILE  0x0E      /* after the terms of use -> RcvInfraPatchFile, same sub */

#define KEEPALIVE_SECS 60

static void hexlog(const char *what, const uint8_t *p, uint32_t n) {
    char hex[3 * 128 + 1];
    size_t o = 0;
    for (uint32_t i = 0; i < n && i < 128; i++) o += (size_t)snprintf(hex + o, sizeof hex - o, "%02X ", p[i]);
    hex[o] = '\0';
    psp_net_log("infra: %s (%u bytes): %s", what, n, hex);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

/* A reply: header + status 200 + payload. */
static int reply(psp_http_response *resp, const uint8_t *req, uint8_t sub, const uint8_t *payload, uint32_t n) {
    const uint32_t len = 12 + n;
    uint8_t *p = (uint8_t *)calloc(1, len);
    if (!p) return (int)0x80431063u;
    put32(p, len);
    p[4] = COM_REPLY; p[5] = sub; p[6] = req[6]; p[7] = req[7];
    put16(p + 8, 200);
    put16(p + 10, 0);
    if (n) memcpy(p + 12, payload, n);
    resp->status = 200; resp->body = p; resp->len = len;
    hexlog("reply", p, len);
    return 0;
}

static int infra_send(const psp_http_request *req, psp_http_response *resp) {
    psp_net_log("infra: %s %s://%s:%u%s", req->method, req->scheme, req->host, (unsigned)req->port, req->path);
    if (!req->body || req->body_len < 8) { psp_net_log("infra: no packet; answering 404"); resp->status = 404; return 0; }
    const uint8_t *b = req->body;
    hexlog("request", b, req->body_len);
    const uint8_t com = b[4], sub = b[5];
    if (com == COM_REQUEST && sub == SUB_FRONT) {
        /* sInfraServerInfo (16 bytes, SetServerInfo): +0 s8 state (0 open, 1/2 closed),
         * +2 s16 keep-alive seconds; the rest left 0. */
        uint8_t info[16] = { 0 };
        put16(info + 2, KEEPALIVE_SECS);
        psp_net_log("infra: front request -> server info (open, keep-alive %d s)", KEEPALIVE_SECS);
        return reply(resp, b, SUB_FRONT_REPLY, info, sizeof info);
    }
    if (com == COM_REQUEST && sub == SUB_LOGIN) {
        /* RcvInfraLogin payload (offsets from the packet start): +12 u32 user number,
         * +16..+35 session words, +36 4 bytes, +40 u64 (time?), +48..+69 settings,
         * +72 u32 event flags (bit n: PUYO_SEGA_%02d). All 0 but the user number for now. */
        char online_id[17] = { 0 };
        if (req->body_len >= 48) memcpy(online_id, b + 32, 16);   /* +12 u32 ticket length, +16 ticket (ours: online id at +16) */
        uint32_t user = 2166136261u;
        for (const char *p = online_id; *p; p++) user = (user ^ (uint8_t)*p) * 16777619u;
        user &= 0x7FFFFFFF;
        uint8_t pl[64] = { 0 };
        put32(pl, user);
        psp_net_log("infra: login '%s' -> user number %u", online_id, (unsigned)user);
        return reply(resp, b, SUB_LOGIN_REPLY, pl, sizeof pl);
    }
    if (com == COM_REQUEST && sub == SUB_PATCH_FILE) {
        /* RcvInfraPatchFile (0x08A63274): +12 u32, nonzero = a patch file to fetch. */
        uint8_t pl[4] = { 0 };
        psp_net_log("infra: patch file request -> none");
        return reply(resp, b, SUB_PATCH_FILE, pl, sizeof pl);
    }
    /* Otherwise the reply carries the request's sub (see the table in docs/infra_protocol.md). */
    uint8_t pl[4] = { 0 };
    psp_net_log("infra: unhandled request com 0x%02X sub 0x%02X; answering sub 0x%02X with a zero word", com, sub, sub);
    return reply(resp, b, sub, pl, sizeof pl);
}

static void infra_log(const char *line) { psp_net_log("http: %s", line); }

/* ---- the game's debug printf ------------------------------------------------------ */

static uint32_t parg(int *slot) {
    const int s = (*slot)++;
    if (s < 7) return psp_cpu.r[5 + s];                     /* $a1..$a3, $t0..$t3 */
    return psp_read32(psp_cpu.r[29] + (uint32_t)(s - 7) * 4u);
}

static void hook_printf(void (*original)(void)) {
    char fmt[400], out[700];
    psp_str(psp_cpu.r[4], fmt, sizeof fmt);
    int slot = 0;
    size_t n = 0;
    for (const char *p = fmt; *p && n + 1 < sizeof out;) {
        if (*p != '%') { out[n++] = *p++; continue; }
        char spec[16]; size_t k = 0;
        spec[k++] = *p++;
        while (*p && strchr("-+ #0123456789.", *p) && k < 10) spec[k++] = *p++;
        while (*p == 'l' || *p == 'h') p++;
        const char c = *p ? *p++ : '\0';
        char piece[300] = "";
        if (c == '%') snprintf(piece, sizeof piece, "%%");
        else if (c == 's') { char s[256]; psp_str(parg(&slot), s, sizeof s); spec[k++] = 's'; spec[k] = 0; snprintf(piece, sizeof piece, spec, s); }
        else if (c == 'c') { piece[0] = (char)parg(&slot); piece[1] = 0; }
        else if (c == 'f') { parg(&slot); snprintf(piece, sizeof piece, "<f>"); }
        else if (c) { spec[k++] = c; spec[k] = 0; snprintf(piece, sizeof piece, spec, parg(&slot)); }
        n += (size_t)snprintf(out + n, sizeof out - n, "%s", piece);
    }
    out[n < sizeof out ? n : sizeof out - 1] = '\0';
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    if (n) psp_net_log("game: %s", out);
    original();
}

void infra_init(void) {
    static const psp_http_transport T = { infra_send, infra_log };
    psp_http_set_transport(&T);
    if (getenv("PSP2I_GAMELOG")) psp_hook_set(FN_GAME_PRINTF, hook_printf);
}
