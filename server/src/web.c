/* The embedded web admin panel (see web.h). */
#if !defined(_WIN32) && !defined(__APPLE__)
#  define _DEFAULT_SOURCE
#endif
#include "web.h"

#include "crypto.h"
#include "db.h"
#include "loot.h"
#include "sqlite3.h"

#include <ctype.h>
#include <stdarg.h>
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
#  define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
typedef int socklen_t;
#  define strncasecmp _strnicmp
static int set_nonblock(sock_t s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on); }
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <strings.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
#  define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
static int set_nonblock(sock_t s) { const int f = fcntl(s, F_GETFL, 0); return f < 0 ? -1 : fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

/* the compiled-in page (web_assets.c, generated from server/web/) */
extern const unsigned char WEB_INDEX_HTML[], WEB_APP_JS[], WEB_APP_CSS[];
extern const unsigned int WEB_INDEX_HTML_LEN, WEB_APP_JS_LEN, WEB_APP_CSS_LEN;

#define MAX_CONNS      64
#define MAX_SESSIONS   32
#define MAX_HEADER     16384
#define MAX_BODY       (4u << 20)
#define IDLE_MS        10000
#define SESSION_MS     (8ull * 3600 * 1000)
#define LOGIN_FAILS    5            /* per address per minute */

typedef struct {
    sock_t   fd;
    char     ip[64];
    char    *in;  size_t in_len, in_cap;
    char    *out; size_t out_len, out_off;
    uint64_t last_ms;
    int      sse, done;           /* done: close once out is sent */
} conn;

typedef struct { char token[65]; char user[40]; uint64_t expires_ms; } session;
typedef struct { char ip[64]; uint64_t window_ms; int fails; } throttle;

struct web {
    web_config cfg;
    sock_t     listen;
    uint16_t   port;
    conn       c[MAX_CONNS];
    session    sess[MAX_SESSIONS];
    throttle   thr[32];
    uint64_t   now_ms, last_push_ms;
    int        net_up;
};

static void wlog(web *w, const char *fmt, ...) {
    if (!w->cfg.log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    w->cfg.log(line);
}

/* ---- output ---- */

static void out_add(conn *c, const char *data, size_t n) {
    if (c->out_len + n + 1 > c->out_off + (size_t)-1 / 2) return;
    char *b = realloc(c->out, c->out_len + n + 1);
    if (!b) return;
    c->out = b;
    memcpy(c->out + c->out_len, data, n);
    c->out_len += n;
}

static void respond(conn *c, int code, const char *type, const char *body, size_t n) {
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : code == 413 ? "Payload Too Large"
                       : code == 429 ? "Too Many Requests" : code == 503 ? "Service Unavailable" : "Internal Server Error";
    char h[640];
    const int hn = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; style-src 'self'; script-src 'self'; frame-ancestors 'none'\r\n\r\n",
        code, reason, type, n);
    out_add(c, h, (size_t)hn);
    if (n) out_add(c, body, n);
    c->done = 1;
}
static void respond_json(conn *c, int code, const char *json) { respond(c, code, "application/json; charset=utf-8", json, strlen(json)); }

/* {"ok":bool,"message":"..."} */
static void respond_msg(web *w, conn *c, int code, int ok, const char *msg) {
    db *d = srv_db(w->cfg.admin->s);
    const char *p[2] = { ok ? "1" : "0", msg };
    char *j = d ? db_json(d, "SELECT json_object('ok', json(CASE ?1 WHEN '1' THEN 'true' ELSE 'false' END), 'message', ?2)", 2, p) : NULL;
    respond_json(c, code, j ? j : "{\"ok\":false}");
    free(j);
}

/* JSON string escaping for responses built by hand */
static size_t jesc(char *o, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { o[n++] = '\\'; o[n++] = (char)ch; }
        else if (ch < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", ch);
        else o[n++] = (char)ch;
    }
    o[n] = 0;
    return n;
}

/* ---- request parsing ---- */

typedef struct {
    char method[8], path[256], query[512];
    char auth[160];
    const char *body; size_t body_len;
} request;

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* a query parameter, URL-decoded */
static int qparam(const request *r, const char *key, char *out, size_t cap) {
    const size_t kl = strlen(key);
    for (const char *p = r->query; *p;) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = 0;
            for (const char *v = p + kl + 1; v < end && n + 1 < cap; v++) {
                if (*v == '+') out[n++] = ' ';
                else if (*v == '%' && v + 2 < end && hex(v[1]) >= 0 && hex(v[2]) >= 0) { out[n++] = (char)(hex(v[1]) * 16 + hex(v[2])); v += 2; }
                else out[n++] = *v;
            }
            out[n] = 0;
            return 1;
        }
        p = amp ? amp + 1 : end;
    }
    out[0] = 0;
    return 0;
}

/* JSON body fields through SQLite: text (NULL if absent), number, bool */
typedef struct { db *d; const char *json; } body_t;
static char *bstr(body_t *b, const char *path) {
    const char *p[2] = { b->json, path };
    char *v = db_json(b->d, "SELECT CASE WHEN json_valid(?1) THEN json_extract(?1, ?2) END", 2, p);
    if (v && !strcmp(v, "null")) { free(v); return NULL; }
    return v;
}
static int bnum(body_t *b, const char *path, double *out) {
    const char *p[2] = { b->json, path };
    char *v = db_json(b->d, "SELECT CASE WHEN json_valid(?1) AND json_type(?1, ?2) IN ('integer', 'real') THEN json_extract(?1, ?2) END", 2, p);
    const int ok = v && strcmp(v, "null");
    if (ok) *out = strtod(v, NULL);
    free(v);
    return ok;
}
static int bbool(body_t *b, const char *path, int *out) {
    const char *p[2] = { b->json, path };
    char *v = db_json(b->d, "SELECT CASE WHEN json_valid(?1) THEN json_type(?1, ?2) END", 2, p);
    const int ok = v && (!strcmp(v, "true") || !strcmp(v, "false"));
    if (ok) *out = !strcmp(v, "true");
    free(v);
    return ok;
}

/* ---- sessions ---- */

static session *session_for(web *w, const char *token) {
    if (!token || strlen(token) != 64) return NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session *s = &w->sess[i];
        if (!s->token[0] || s->expires_ms < w->now_ms) continue;
        unsigned char diff = 0;                               /* constant time */
        for (int k = 0; k < 64; k++) diff |= (unsigned char)(s->token[k] ^ token[k]);
        if (!diff) { s->expires_ms = w->now_ms + SESSION_MS; return s; }
    }
    return NULL;
}

static session *session_new(web *w, const char *user) {
    session *s = &w->sess[0];
    for (int i = 0; i < MAX_SESSIONS; i++) {                  /* a free or expired slot, else the oldest */
        if (!w->sess[i].token[0] || w->sess[i].expires_ms < w->now_ms) { s = &w->sess[i]; break; }
        if (w->sess[i].expires_ms < s->expires_ms) s = &w->sess[i];
    }
    uint8_t r[32];
    if (os_random(r, sizeof r) != 0) return NULL;
    for (int i = 0; i < 32; i++) snprintf(s->token + 2 * i, 3, "%02x", r[i]);
    snprintf(s->user, sizeof s->user, "%s", user);
    s->expires_ms = w->now_ms + SESSION_MS;
    return s;
}

static throttle *throttle_for(web *w, const char *ip) {
    throttle *t = &w->thr[0];
    for (int i = 0; i < 32; i++) {
        if (!strcmp(w->thr[i].ip, ip)) return &w->thr[i];
        if (w->thr[i].window_ms < t->window_ms) t = &w->thr[i];
    }
    memset(t, 0, sizeof *t);
    snprintf(t->ip, sizeof t->ip, "%s", ip);
    t->window_ms = w->now_ms;
    return t;
}

/* ---- status JSON (also the event stream) ---- */

static char *status_json(web *w) {
    admin_ctx *a = w->cfg.admin;
    srv_stats st;
    srv_get_stats(a->s, &st);
    const loot_rates r = srv_rates(a->s);
    const srv_telemetry *t = a->tel;
    static srv_player pl[64];
    const int n = srv_players(a->s, pl, 64);
    size_t cap = 4096 + (size_t)n * 512, o = 0;
    char *j = malloc(cap), name[96];
    if (!j) return NULL;
    jesc(name, sizeof name, srv_name(a->s));
    o += (size_t)snprintf(j + o, cap - o,
        "{\"name\":\"%s\",\"mode\":\"%s\",\"max_players\":%d,\"uptime_s\":%llu,\"tick_hz\":%.2f,\"tick_avg_ms\":%.3f,\"tick_max_ms\":%.3f,"
        "\"late_ticks\":%llu,\"cpu_pct\":%.2f,\"rss_mb\":%.1f,\"pending\":%d,\"kills\":%llu,\"drops\":%llu,\"rare_drops\":%llu,"
        "\"packets_in\":%llu,\"packets_dropped\":%llu,\"handshakes_ok\":%llu,\"handshakes_rejected\":%llu,"
        "\"rates\":{\"exp_rate\":%g,\"drop_rate\":%g,\"rare_drop_rate\":%g,\"meseta_rate\":%g,\"type_points_rate\":%g},\"events\":{",
        name, srv_mode_online(a->s) ? "online" : "lan", srv_max_players(a->s), t ? (unsigned long long)t->uptime_s : 0ull,
        t ? t->hz : 0, t ? t->tick_avg_ms : 0, t ? t->tick_max_ms : 0, t ? (unsigned long long)t->late_ticks : 0ull,
        t ? t->cpu_pct : 0, t ? t->rss_mb : 0, st.pending, (unsigned long long)st.kills, (unsigned long long)st.drops,
        (unsigned long long)st.rare_drops, (unsigned long long)st.packets_in, (unsigned long long)st.packets_dropped,
        (unsigned long long)st.handshakes_ok, (unsigned long long)st.handshakes_rejected,
        r.exp, r.drop, r.rare, r.meseta, r.type_points);
    for (int i = 0; srv_event_name(i); i++)
        o += (size_t)snprintf(j + o, cap - o, "%s\"%s\":%s", i ? "," : "", srv_event_name(i), (srv_events(a->s) >> i) & 1 ? "true" : "false");
    o += (size_t)snprintf(j + o, cap - o, "},\"players\":[");
    for (int i = 0; i < n; i++) {
        char nm[96], loc[80], acc[24];
        jesc(nm, sizeof nm, pl[i].name);
        jesc(loc, sizeof loc, pl[i].location);
        if (pl[i].account_id) snprintf(acc, sizeof acc, "%lld", (long long)pl[i].account_id); else snprintf(acc, sizeof acc, "null");
        o += (size_t)snprintf(j + o, cap - o, "%s{\"session\":%llu,\"name\":\"%s\",\"account_id\":%s,\"ip\":\"%s\",\"lan_ip\":\"%s\",\"mac\":\"%s\","
                              "\"ping_ms\":%d,\"online_s\":%llu,\"location\":\"%s\"}",
                              i ? "," : "", (unsigned long long)pl[i].session, nm, acc, pl[i].ip, pl[i].lan_ip, pl[i].mac, pl[i].ping_ms,
                              (unsigned long long)((w->now_ms > pl[i].joined_ms ? w->now_ms - pl[i].joined_ms : 0) / 1000), loc);
    }
    snprintf(j + o, cap - o, "]}");
    return j;
}

/* ---- routes ---- */

static int is(const request *r, const char *method, const char *path) { return !strcmp(r->method, method) && !strcmp(r->path, path); }

static void api(web *w, conn *c, const request *r) {
    admin_ctx *a = w->cfg.admin;
    db *d = srv_db(a->s);
    if (!d) { respond_msg(w, c, 503, 0, "the web panel needs the database"); return; }
    body_t b = { d, r->body ? r->body : "" };
    char msg[2048];

    if (is(r, "POST", "/api/v1/login")) {
        throttle *t = throttle_for(w, c->ip);
        if (w->now_ms - t->window_ms > 60000) { t->window_ms = w->now_ms; t->fails = 0; }
        if (t->fails >= LOGIN_FAILS) { respond_msg(w, c, 429, 0, "too many failed logins: wait a minute"); return; }
        char *u = bstr(&b, "$.username"), *p = bstr(&b, "$.password");
        char why[128] = "";
        const int64_t id = u && p ? db_account_login(d, u, p, c->ip, why, sizeof why) : DB_BAD_CREDS;
        const char *params[1] = { u ? u : "" };
        char *adm = id > 0 ? db_json(d, "SELECT is_admin FROM accounts WHERE username = ?1", 1, params) : NULL;
        const int ok = id > 0 && adm && !strcmp(adm, "1");
        free(adm);
        if (!ok) {
            t->fails++;
            char who[80];
            snprintf(who, sizeof who, "web:%s", u ? u : "?");
            db_audit(d, who, "login_failed", c->ip, NULL);
            free(u); free(p);
            respond_msg(w, c, 401, 0, id == DB_BANNED ? "this account is banned" : "wrong name or password, or not an admin");
            return;
        }
        session *s = session_new(w, u);
        char who[80], j[256], nm[96];
        snprintf(who, sizeof who, "web:%s", u);
        db_audit(d, who, "login", c->ip, NULL);
        jesc(nm, sizeof nm, u);
        snprintf(j, sizeof j, "{\"ok\":true,\"token\":\"%s\",\"user\":\"%s\"}", s ? s->token : "", nm);
        free(u); free(p);
        respond_json(c, s ? 200 : 500, j);
        return;
    }

    /* everything else needs a session */
    const char *tok = !strncmp(r->auth, "Bearer ", 7) ? r->auth + 7 : NULL;
    char qtok[80];
    if (!tok && is(r, "GET", "/api/v1/stream") && qparam(r, "token", qtok, sizeof qtok)) tok = qtok;
    session *s = session_for(w, tok);
    if (!s) { respond_msg(w, c, 401, 0, "log in first"); return; }
    char actor[64];
    snprintf(actor, sizeof actor, "web:%s", s->user);

    if (is(r, "POST", "/api/v1/logout")) { s->token[0] = 0; respond_msg(w, c, 200, 1, "logged out"); return; }
    if (is(r, "GET", "/api/v1/status")) { char *j = status_json(w); respond_json(c, 200, j ? j : "{}"); free(j); return; }
    if (is(r, "GET", "/api/v1/stream")) {
        static const char H[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n"
                                "X-Content-Type-Options: nosniff\r\n\r\nretry: 2000\n\n";
        out_add(c, H, sizeof H - 1);
        c->sse = 1;
        return;
    }
    if (is(r, "POST", "/api/v1/kick")) {
        double sid;
        char *why = bstr(&b, "$.reason");
        const int rc = bnum(&b, "$.session", &sid) ? admin_kick(a, actor, (uint64_t)sid, why, msg, sizeof msg) : -1;
        free(why);
        respond_msg(w, c, rc ? 400 : 200, !rc, rc && !msg[0] ? "session is required" : msg);
        return;
    }
    if (is(r, "POST", "/api/v1/ban")) {
        char *target = bstr(&b, "$.target"), *why = bstr(&b, "$.reason");
        double minutes = 0;
        bnum(&b, "$.minutes", &minutes);
        msg[0] = 0;
        const int rc = target ? admin_ban(a, actor, target, (int64_t)minutes, why, msg, sizeof msg) : -1;
        free(target); free(why);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg[0] ? msg : "target is required");
        return;
    }
    if (is(r, "POST", "/api/v1/unban")) {
        char *target = bstr(&b, "$.target");
        msg[0] = 0;
        const int rc = target ? admin_unban(a, actor, target, msg, sizeof msg) : -1;
        free(target);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg[0] ? msg : "target is required");
        return;
    }
    if (is(r, "POST", "/api/v1/admin")) {
        double id;
        int on;
        msg[0] = 0;
        const int rc = bnum(&b, "$.account_id", &id) && bbool(&b, "$.admin", &on) ? admin_set_admin(a, actor, (int64_t)id, on, msg, sizeof msg) : -1;
        respond_msg(w, c, rc ? 400 : 200, !rc, msg[0] ? msg : "account_id and admin are required");
        return;
    }
    if (is(r, "GET", "/api/v1/rates")) {
        const loot_rates x = srv_rates(a->s);
        char j[256];
        snprintf(j, sizeof j, "{\"exp_rate\":%g,\"drop_rate\":%g,\"rare_drop_rate\":%g,\"meseta_rate\":%g,\"type_points_rate\":%g}",
                 x.exp, x.drop, x.rare, x.meseta, x.type_points);
        respond_json(c, 200, j);
        return;
    }
    if (is(r, "POST", "/api/v1/rates")) {
        static const char *const NAMES[] = { "exp_rate", "drop_rate", "rare_drop_rate", "meseta_rate", "type_points_rate" };
        size_t o = 0;
        int any = 0, rc = 0;
        msg[0] = 0;
        for (int i = 0; i < 5 && !rc; i++) {
            char path[40];
            double v;
            snprintf(path, sizeof path, "$.%s", NAMES[i]);
            if (!bnum(&b, path, &v)) continue;
            any = 1;
            char one[200];
            rc = admin_set_rate(a, actor, NAMES[i], v, one, sizeof one);
            o += (size_t)snprintf(msg + o, sizeof msg - o, "%s%s", o ? "; " : "", one);
        }
        respond_msg(w, c, rc || !any ? 400 : 200, !rc && any, any ? msg : "no rates given");
        return;
    }
    if (is(r, "POST", "/api/v1/events")) {
        char *name = bstr(&b, "$.name");
        int on;
        msg[0] = 0;
        const int rc = name && bbool(&b, "$.on", &on) ? admin_set_event(a, actor, name, on, msg, sizeof msg) : -1;
        free(name);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg[0] ? msg : "name and on are required");
        return;
    }
    if (is(r, "POST", "/api/v1/announce")) {
        char *text = bstr(&b, "$.text"), *col = bstr(&b, "$.color");
        const int64_t rgb = col ? admin_parse_color(col) : 0xFFD84A;
        msg[0] = 0;
        const int rc = text && rgb >= 0 ? admin_announce(a, actor, text, (uint32_t)rgb, msg, sizeof msg) : -1;
        free(text); free(col);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg[0] ? msg : "text (and a valid color) are required");
        return;
    }
    if (is(r, "GET", "/api/v1/drop_tables")) {
        const char *path = srv_drop_tables_path(a->s);
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (!f) { respond_msg(w, c, 404, 0, "no drop tables file"); return; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = n >= 0 && n < (long)MAX_BODY ? malloc((size_t)n + 1) : NULL;
        const size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        if (!buf || got != (size_t)n) { free(buf); respond_msg(w, c, 500, 0, "cannot read the drop tables"); return; }
        respond(c, 200, "application/json; charset=utf-8", buf, (size_t)n);
        free(buf);
        return;
    }
    if (is(r, "POST", "/api/v1/drop_tables")) {
        char *json = malloc(r->body_len + 1);
        if (!json) { respond_msg(w, c, 500, 0, "out of memory"); return; }
        memcpy(json, r->body, r->body_len);
        json[r->body_len] = 0;
        const int rc = admin_save_drop_tables(a, actor, json, msg, sizeof msg);
        free(json);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg);
        return;
    }
    if (is(r, "POST", "/api/v1/reload")) {
        char *what = bstr(&b, "$.what");
        const int rc = admin_reload(a, actor, what ? what : "all", msg, sizeof msg);
        free(what);
        respond_msg(w, c, rc ? 400 : 200, !rc, msg);
        return;
    }
    if (is(r, "POST", "/api/v1/rcon")) {
        char *cmd = bstr(&b, "$.command");
        char ractor[64], out[8192];
        snprintf(ractor, sizeof ractor, "rcon:%s", s->user);
        const int rc = cmd ? admin_exec(a, ractor, cmd, out, sizeof out) : -1;
        free(cmd);
        respond_msg(w, c, cmd || rc == 0 ? 200 : 400, rc == 0, cmd || rc == 0 ? out : "command is required");
        return;
    }
    if (is(r, "GET", "/api/v1/audit")) {
        char q[200], lim[16], off[16];
        qparam(r, "q", q, sizeof q);
        if (!qparam(r, "limit", lim, sizeof lim) || !lim[0]) snprintf(lim, sizeof lim, "100");
        if (!qparam(r, "offset", off, sizeof off) || !off[0]) snprintf(off, sizeof off, "0");
        const char *params[3] = { q, lim, off };
        char *j = db_json(d, "SELECT coalesce(json_group_array(json_object('id', log_id, 'time', timestamp, 'admin', admin_user, "
                             "'action', action_type, 'target', target, 'details', json(details_json))), '[]') FROM ("
                             "SELECT * FROM audit_logs WHERE ?1 = '' OR (admin_user || ' ' || action_type || ' ' || coalesce(target, '') || ' ' || "
                             "coalesce(details_json, '')) LIKE '%' || ?1 || '%' ORDER BY log_id DESC "
                             "LIMIT max(1, min(500, CAST(?2 AS INTEGER))) OFFSET max(0, CAST(?3 AS INTEGER)))", 3, params);
        respond_json(c, j ? 200 : 500, j ? j : "[]");
        free(j);
        return;
    }
    if (is(r, "GET", "/api/v1/accounts")) {
        char q[100];
        qparam(r, "q", q, sizeof q);
        const char *params[1] = { q };
        char *j = db_json(d, "SELECT coalesce(json_group_array(json_object('account_id', account_id, 'username', username, "
                             "'is_admin', json(CASE is_admin WHEN 1 THEN 'true' ELSE 'false' END), "
                             "'is_banned', json(CASE is_banned WHEN 1 THEN 'true' ELSE 'false' END), 'ban_reason', ban_reason, "
                             "'ban_expires_at', ban_expires_at, 'last_ip', last_ip, 'created_at', created_at)), '[]') FROM ("
                             "SELECT * FROM accounts WHERE ?1 = '' OR username LIKE '%' || ?1 || '%' OR last_ip LIKE '%' || ?1 || '%' "
                             "ORDER BY account_id LIMIT 200)", 1, params);
        respond_json(c, j ? 200 : 500, j ? j : "[]");
        free(j);
        return;
    }
    if (is(r, "GET", "/api/v1/bans")) {
        char *j = db_json(d, "SELECT coalesce(json_group_array(json_object('ip', ip, 'reason', reason, 'expires_at', expires_at, "
                             "'created_at', created_at, 'admin', admin_user)), '[]') FROM (SELECT * FROM ip_bans ORDER BY created_at DESC)", 0, NULL);
        respond_json(c, j ? 200 : 500, j ? j : "[]");
        free(j);
        return;
    }
    respond_msg(w, c, 404, 0, "no such API");
}

static void route(web *w, conn *c, const request *r) {
    if (!strncmp(r->path, "/api/", 5)) { api(w, c, r); return; }
    if (strcmp(r->method, "GET")) { respond(c, 405, "text/plain", "", 0); return; }
    if (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html")) respond(c, 200, "text/html; charset=utf-8", (const char *)WEB_INDEX_HTML, WEB_INDEX_HTML_LEN);
    else if (!strcmp(r->path, "/app.js")) respond(c, 200, "text/javascript; charset=utf-8", (const char *)WEB_APP_JS, WEB_APP_JS_LEN);
    else if (!strcmp(r->path, "/app.css")) respond(c, 200, "text/css; charset=utf-8", (const char *)WEB_APP_CSS, WEB_APP_CSS_LEN);
    else respond(c, 404, "text/plain", "not found", 9);
}

/* Parse what has arrived; route a complete request. */
static void handle_input(web *w, conn *c) {
    char *end = NULL;
    for (size_t i = 3; i < c->in_len; i++)
        if (c->in[i - 3] == '\r' && c->in[i - 2] == '\n' && c->in[i - 1] == '\r' && c->in[i] == '\n') { end = c->in + i + 1; break; }
    if (!end) {
        if (c->in_len > MAX_HEADER) respond(c, 413, "text/plain", "", 0);
        return;
    }
    request r;
    memset(&r, 0, sizeof r);
    size_t content_len = 0;
    char *line = c->in, *nl = strstr(line, "\r\n");
    if (!nl) { respond(c, 400, "text/plain", "", 0); return; }
    *nl = 0;
    char target[800] = "";
    if (sscanf(line, "%7s %799s", r.method, target) != 2) { respond(c, 400, "text/plain", "", 0); return; }
    char *qm = strchr(target, '?');
    if (qm) { *qm = 0; snprintf(r.query, sizeof r.query, "%s", qm + 1); }
    snprintf(r.path, sizeof r.path, "%s", target);
    for (char *h = nl + 2; h < end - 2;) {
        char *e = strstr(h, "\r\n");
        if (!e || e >= end) break;
        *e = 0;
        if (!strncasecmp(h, "Content-Length:", 15)) content_len = strtoul(h + 15, NULL, 10);
        else if (!strncasecmp(h, "Authorization:", 14)) { const char *v = h + 14; while (*v == ' ') v++; snprintf(r.auth, sizeof r.auth, "%s", v); }
        h = e + 2;
    }
    if (content_len > MAX_BODY) { respond(c, 413, "text/plain", "", 0); return; }
    const size_t head = (size_t)(end - c->in);
    if (c->in_len < head + content_len) return;               /* wait for the body */
    r.body = end;
    r.body_len = content_len;
    end[content_len] = 0;                                      /* in_cap keeps room for this */
    route(w, c, &r);
}

/* ---- connections ---- */

static void conn_close(conn *c) {
    if (c->fd != BAD_SOCK) sock_close(c->fd);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = BAD_SOCK;
}

web *web_create(const web_config *cfg) {
    web *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->cfg = *cfg;
    for (int i = 0; i < MAX_CONNS; i++) w->c[i].fd = BAD_SOCK;
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { free(w); return NULL; }
#else
    signal(SIGPIPE, SIG_IGN);                                 /* a browser closing mid-write */
#endif
    w->net_up = 1;
    w->listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int on = 1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg->port);
    const char *bind_ip = cfg->bind && *cfg->bind ? cfg->bind : "127.0.0.1";
    if (w->listen == BAD_SOCK || inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1 ||
        setsockopt(w->listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on) != 0 ||
        bind(w->listen, (struct sockaddr *)&a, sizeof a) != 0 || listen(w->listen, 16) != 0 || set_nonblock(w->listen) != 0) {
        wlog(w, "web: cannot listen on %s:%u", bind_ip, (unsigned)cfg->port);
        web_destroy(w);
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(w->listen, (struct sockaddr *)&a, &al);
    w->port = ntohs(a.sin_port);
    wlog(w, "web: admin panel at http://%s:%u/%s", bind_ip, (unsigned)w->port,
         strcmp(bind_ip, "127.0.0.1") ? " (reachable from the network: use strong admin passwords, ideally behind TLS)" : "");
    return w;
}

uint16_t web_port(const web *w) { return w ? w->port : 0; }

void web_tick(web *w, uint64_t now_ms) {
    if (!w) return;
    w->now_ms = now_ms;
    /* accept */
    for (int k = 0; k < 16; k++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const sock_t fd = accept(w->listen, (struct sockaddr *)&from, &fl);
        if (fd == BAD_SOCK) break;
        conn *c = NULL;
        for (int i = 0; i < MAX_CONNS && !c; i++) if (w->c[i].fd == BAD_SOCK) c = &w->c[i];
        if (!c || set_nonblock(fd) != 0) { sock_close(fd); continue; }
        memset(c, 0, sizeof *c);
        c->fd = fd;
        c->last_ms = now_ms;
        inet_ntop(AF_INET, &from.sin_addr, c->ip, sizeof c->ip);
    }
    /* the event stream: status once a second */
    const int push = now_ms - w->last_push_ms >= 1000;
    char *status = NULL;
    if (push) w->last_push_ms = now_ms;
    for (int i = 0; i < MAX_CONNS; i++) {
        conn *c = &w->c[i];
        if (c->fd == BAD_SOCK) continue;
        /* read */
        while (!c->done && !c->sse) {
            if (c->in_cap - c->in_len < 4097) {
                const size_t ncap = c->in_cap ? c->in_cap * 2 : 8192;
                if (ncap > MAX_HEADER + MAX_BODY + 8192) { respond(c, 413, "text/plain", "", 0); break; }
                char *b = realloc(c->in, ncap);
                if (!b) { c->done = 1; break; }
                c->in = b;
                c->in_cap = ncap;
            }
            const int n = (int)recv(c->fd, c->in + c->in_len, (int)(c->in_cap - c->in_len - 1), 0);
            if (n > 0) { c->in_len += (size_t)n; c->in[c->in_len] = 0; c->last_ms = now_ms; handle_input(w, c); continue; }
            if (n == 0 || !WOULD_BLOCK()) { conn_close(c); break; }
            break;
        }
        if (c->fd == BAD_SOCK) continue;
        if (c->sse && push) {
            if (!status) status = status_json(w);
            if (status) { out_add(c, "data: ", 6); out_add(c, status, strlen(status)); out_add(c, "\n\n", 2); }
        }
        /* write */
        while (c->out_off < c->out_len) {
            const int n = (int)send(c->fd, c->out + c->out_off, (int)(c->out_len - c->out_off), 0);
            if (n > 0) { c->out_off += (size_t)n; c->last_ms = now_ms; continue; }
            if (n < 0 && WOULD_BLOCK()) break;
            conn_close(c);
            break;
        }
        if (c->fd == BAD_SOCK) continue;
        if (c->out_off == c->out_len && c->out_len) { c->out_off = c->out_len = 0; }
        if ((c->done && c->out_len == 0) || (!c->sse && now_ms - c->last_ms > IDLE_MS) || (c->sse && now_ms - c->last_ms > 60000))
            conn_close(c);
    }
    free(status);
}

void web_destroy(web *w) {
    if (!w) return;
    for (int i = 0; i < MAX_CONNS; i++) if (w->c[i].fd != BAD_SOCK) conn_close(&w->c[i]);
    if (w->listen != BAD_SOCK) sock_close(w->listen);
#ifdef _WIN32
    if (w->net_up) WSACleanup();
#endif
    free(w);
}
