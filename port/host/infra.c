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
#define SUB_LOGIN       0x01      /* when entering the lobby -> RcvInfraLogin, same sub */

#define SUB_TERMS       0x10      /* the terms of use (EULA); +12 u32 ticket length, +16 the NP ticket */
/* The login reply (not yet seen asked for): sub 0x01 -> RcvInfraLogin (0x08A62F5C), payload
 * +12 u32 user number, +16..+35 session words, +40 u64, +72 u32 event flags (PUYO_SEGA_%02d). */

#define SUB_NEWS        0x0F      /* the Notices text: raw body (parsed by 0x0888FBB0) */
#define SUB_NEWS_PAGE   0x19      /* news items by index from 1, until an empty body */
#define SUB_RAW_0x18    0x18      /* after the notices: raw body, meaning unknown */
#define SUB_PATCH_FILE  0x0E      /* after the terms of use -> RcvInfraPatchFile, same sub */

#define KEEPALIVE_SECS 60

/* Texts the server shows, from the data folder; UTF-8 there, UTF-16LE for the game. */
typedef struct { const char *file; char *utf16; size_t len; } infra_text;
static infra_text g_terms   = { "terms_of_use.txt", NULL, 0 };   /* the Terms of Use window */
static infra_text g_notices = { "notices.txt", NULL, 0 };        /* the Notices window */

static void load_text(const char *base, infra_text *t) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", base, t->file);
    FILE *f = fopen(path, "rb");
    if (!f) { psp_net_log("infra: %s not found; a placeholder is shown", path); return; }
    static unsigned char raw[16384];
    const size_t got = fread(raw, 1, sizeof raw, f);
    fclose(f);
    t->utf16 = (char *)malloc(got * 2 + 2);
    if (!t->utf16) return;
    size_t o = 0;
    for (size_t i = 0; i < got;) {                        /* lines end with \n; a \r is dropped */
        uint32_t c = raw[i++];
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        while (more-- && i < got) c = (c << 6) | (raw[i++] & 0x3Fu);
        if (c == '\r' || c == 0xFEFF || c > 0xFFFF) continue;   /* BOM, outside the BMP: skipped */
        t->utf16[o++] = (char)(c & 0xFF); t->utf16[o++] = (char)(c >> 8);
    }
    t->len = o;
    psp_net_log("infra: %s (%u characters)", path, (unsigned)(o / 2));
}

/* A raw text reply (no packet header): UTF-16LE ending with a 0 character. */
static int text_reply(psp_http_response *resp, const infra_text *t, const char *placeholder) {
    const size_t n = t->len ? t->len : strlen(placeholder) * 2;
    uint8_t *p = (uint8_t *)calloc(1, n + 2);
    if (!p) return (int)0x80431063u;
    if (t->len) memcpy(p, t->utf16, n);
    else for (size_t i = 0; placeholder[i]; i++) p[2 * i] = (uint8_t)placeholder[i];
    resp->status = 200; resp->body = p; resp->len = (uint32_t)n + 2;
    psp_net_log("infra: %s -> %u characters", t->file, (unsigned)(n / 2));
    return 0;
}

static void hexlog(const char *what, const uint8_t *p, uint32_t n) {
    char hex[3 * 128 + 1];
    size_t o = 0;
    for (uint32_t i = 0; i < n && i < 128; i++) o += (size_t)snprintf(hex + o, sizeof hex - o, "%02X ", p[i]);
    hex[o] = '\0';
    psp_net_log("infra: %s (%u bytes): %s", what, n, hex);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

/* Append a reply packet (header, status 200, payload) to the response: one POST can carry
 * several requests, each answered in order. */
static int reply(psp_http_response *resp, const uint8_t *req, uint8_t sub, const uint8_t *payload, uint32_t n) {
    const uint32_t len = 12 + n;
    uint8_t *body = (uint8_t *)realloc(resp->body, resp->len + len);
    if (!body) return (int)0x80431063u;
    uint8_t *p = body + resp->len;
    memset(p, 0, len);
    put32(p, len);
    p[4] = COM_REPLY; p[5] = sub; p[6] = req[6]; p[7] = req[7];
    put16(p + 8, 200);
    put16(p + 10, 0);
    if (n) memcpy(p + 12, payload, n);
    resp->status = 200; resp->body = body; resp->len += len;
    hexlog("reply", p, len);
    return 0;
}

static uint32_t g_user_no;                                  /* this player's user number (from the name) */

/* One request packet -> its reply packet. */
static int answer(psp_http_response *resp, const uint8_t *b) {
    const uint8_t sub = b[5];
    switch (sub) {
    case SUB_FRONT: {
        /* sInfraServerInfo (16 bytes, SetServerInfo): +0 s8 state (0 open, 1/2 closed),
         * +2 s16 keep-alive seconds; the rest left 0. */
        uint8_t info[16] = { 0 };
        put16(info + 2, KEEPALIVE_SECS);
        psp_net_log("infra: front request -> server info (open, keep-alive %d s)", KEEPALIVE_SECS);
        return reply(resp, b, SUB_FRONT_REPLY, info, sizeof info);
    }
    case SUB_LOGIN: {
        /* RcvInfraLogin (0x08A62F5C), 64-byte payload: +12 u32 user number, +16..+35 session
         * words, +36 4 bytes, +40 u64, +48..+69 settings, +72 u32 event flags. */
        uint8_t pl[64] = { 0 };
        put32(pl, g_user_no);
        /* +68: players in a room the game creates (default 12); +69: the lobby joins a room
         * found with fewer members than this, else creates one (default 8; 0x08CAA088).
         * 12 and 12: lobby rooms fill to 12, as on the original servers. */
        pl[68 - 12] = 12;
        pl[69 - 12] = 12;
        psp_net_log("infra: login -> user number %u", (unsigned)g_user_no);
        return reply(resp, b, SUB_LOGIN, pl, sizeof pl);
    }
    case SUB_PATCH_FILE: {
        /* RcvInfraPatchFile (0x08A63274): +12 u32, nonzero = a patch file to fetch. */
        uint8_t pl[4] = { 0 };
        psp_net_log("infra: patch file request -> none");
        return reply(resp, b, SUB_PATCH_FILE, pl, sizeof pl);
    }
    default: {
        /* The reply carries the request's sub (docs/infra_protocol.md); counts and lists empty. */
        uint8_t pl[16] = { 0 };
        psp_net_log("infra: request sub 0x%02X -> empty reply packet", sub);
        return reply(resp, b, sub, pl, sizeof pl);
    }
    }
}

/* The matching server menu's button labels are Japanese in the English game ("ロビーへ",
 * "カウンターへ"); the online mode was never translated. They are UTF-16 in the game's data,
 * drawn by 0x08892244 / 0x08892300: replaced in place with English that fits the slots
 * (12 bytes, and 12 + 4 bytes of padding), once the game is loaded. */
static void translate_menu(void) {
    static int done;
    if (done) return;
    done = 1;
    static const struct { uint32_t addr; const char *en; uint32_t cap; } T[] = {
        { 0x08E06600u, "Lobby", 12 },     /* ロビーへ */
        { 0x08E0660Cu, "Counter", 16 },   /* カウンターへ */
    };
    static const uint16_t JP[2][7] = { { 0x30ED, 0x30D3, 0x30FC, 0x3078, 0 }, { 0x30AB, 0x30A6, 0x30F3, 0x30BF, 0x30FC, 0x3078, 0 } };
    for (int k = 0; k < 2; k++) {
        int same = 1;
        for (int i = 0; JP[k][i]; i++) if (psp_read16(T[k].addr + 2u * (uint32_t)i) != JP[k][i]) same = 0;
        if (!same) { psp_net_log("infra: menu label at 0x%08X is not the expected text; left alone", T[k].addr); continue; }
        uint32_t i = 0;
        for (; T[k].en[i] && 2 * (i + 1) < T[k].cap; i++) psp_write16(T[k].addr + 2 * i, (uint8_t)T[k].en[i]);
        for (; 2 * i < T[k].cap; i++) psp_write16(T[k].addr + 2 * i, 0);
    }
    psp_net_log("infra: matching server menu labels in English");
}

static int infra_send(const psp_http_request *req, psp_http_response *resp) {
    translate_menu();
    psp_net_log("infra: %s %s://%s:%u%s", req->method, req->scheme, req->host, (unsigned)req->port, req->path);
    if (!req->body || req->body_len < 8) { psp_net_log("infra: no packet; answering 404"); resp->status = 404; return 0; }
    const uint8_t *b = req->body;
    hexlog("request", b, req->body_len);
    const uint8_t sub = b[5];
    /* Requests answered with a raw body (no packet): they come alone. */
    if (sub == SUB_TERMS)          /* the request carries the NP ticket (+12 length, +16 ticket) */
        return text_reply(resp, &g_terms, "No terms of use are set for this server.");
    if (sub == SUB_NEWS)
        return text_reply(resp, &g_notices, "No notices.");
    if (sub == SUB_RAW_0x18 || sub == SUB_NEWS_PAGE) {
        /* 0x18: after the notices, meaning unknown (a packet here made the game read through
         * a null pointer). 0x19: news item +12 u32 index from 1, stored by 0x089976C4; the
         * game asks for the next until a reply is empty. Both: empty. */
        psp_net_log("infra: request sub 0x%02X -> empty body", sub);
        resp->status = 200; resp->body = NULL; resp->len = 0;
        return 0;
    }
    resp->body = NULL; resp->len = 0;
    for (uint32_t off = 0; off + 8 <= req->body_len;) {
        const uint32_t len = (uint32_t)b[off] | (uint32_t)b[off + 1] << 8 | (uint32_t)b[off + 2] << 16 | (uint32_t)b[off + 3] << 24;
        if (len < 8 || off + len > req->body_len || b[off + 4] != COM_REQUEST) {
            psp_net_log("infra: bad packet at %u (length %u)", (unsigned)off, (unsigned)len);
            break;
        }
        const int rc = answer(resp, b + off);
        if (rc) return rc;
        off += len;
    }
    if (!resp->len) resp->status = 200;
    return 0;
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

void infra_init(const char *base, const char *player_name) {
    uint32_t h = 2166136261u;                             /* FNV-1a of the name: a stable user number */
    for (const char *p = player_name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    g_user_no = (h & 0x7FFFFFFF) | 1;
    load_text(base, &g_terms);
    load_text(base, &g_notices);
    static const psp_http_transport T = { infra_send, infra_log };
    psp_http_set_transport(&T);
    if (getenv("PSP2I_GAMELOG")) psp_hook_set(FN_GAME_PRINTF, hook_printf);
}
