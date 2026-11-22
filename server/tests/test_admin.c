/* Task 2.4: the web admin panel, its REST API, RCON and the console.
 *
 * The server (game + web) ticks at 30 Hz on its own thread; this thread is
 * the browser (plain HTTP over sockets) and a game client (ENet).
 *
 *  - the page, script and styles are served, with a Content-Security-Policy;
 *    the API refuses calls without a session;
 *  - login: wrong password and non-admin accounts refused; repeated failures
 *    throttled (429); an admin gets a token;
 *  - GATE 1: rare_drop_rate 1.0 -> 3.0 through the API: GET /rates and the
 *    server's memory say 3.0, and the client's next 1,000 kills show ~3x the
 *    rare drops of the 1,000 before;
 *  - GATE 2: a ban through the API: the client gets the BANNED disconnect
 *    reason within 1 s, is refused (ACK status BANNED) when it reconnects, and
 *    gets back in after an unban;
 *  - announcements and event toggles reach the client; the drop-table editor
 *    rejects a broken table (line and column) and applies a good one; the
 *    audit log records it all; RCON and the console run commands; the event
 *    stream delivers status. */
#include "admin.h"
#include "clock.h"
#include "db.h"
#include "server.h"
#include "web.h"

#include <psprecomp/net_enet.h>
#include <psprecomp/net_proto.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void quiet(const char *l) { (void)l; }
static void sleep_ms(unsigned ms) { clk_sleep_until_ns(clk_now_ns() + (uint64_t)ms * 1000000u); }

/* ---- the server thread ---- */
static srv *g_s;
static web *g_w;
static srv_telemetry g_tel = { 30.0, 0.02, 0.1, 0.1, 3.0, 1, 0 };
static volatile int g_run = 1;
static void *server_main(void *u) {
    (void)u;
    uint64_t next = clk_now_ns();
    while (g_run) {
        const uint64_t now = clk_now_ns() / 1000000u;
        srv_tick(g_s, now);
        web_tick(g_w, now);
        next += 1000000000ull / 30;
        clk_sleep_until_ns(next);
    }
    return NULL;
}

/* ---- HTTP client ---- */
static uint16_t g_port;
typedef struct { int code; char body[65536]; char head[4096]; } resp;
static int http(const char *method, const char *path, const char *token, const char *body, resp *r) {
    memset(r, 0, sizeof *r);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    static char req[70000];
    const size_t bl = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: localhost\r\n%s%s%sContent-Length: %zu\r\n\r\n", method, path,
                     token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "", bl);
    if (bl) { memcpy(req + n, body, bl); n += (int)bl; }
    send(fd, req, (size_t)n, 0);
    static char buf[80000];
    size_t got = 0;
    for (;;) {
        const ssize_t k = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    buf[got] = 0;
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) return -1;
    *sep = 0;
    snprintf(r->head, sizeof r->head, "%s", buf);
    snprintf(r->body, sizeof r->body, "%s", sep + 4);
    sscanf(buf, "HTTP/1.1 %d", &r->code);
    return r->code;
}
static void json_str(const char *body, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(body, pat);
    out[0] = 0;
    if (!p) return;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

/* ---- game client ---- */
typedef struct {
    psp_net_enet *e;
    int acked, rewards, drops, rares, notices, events_msgs;
    uint32_t status, last_rgb, last_events;
    char last_notice[PSP_MMO_NOTICE_TEXT_LEN + 1];
} client;
static void cl_rx(const psp_net_packet *p, void *u) {
    client *c = u;
    if (p->port != PSP_MMO_CTRL_PORT) return;
    switch (psp_mmo_type(p->data, p->len)) {
    case MSG_S2C_HANDSHAKE_ACK: c->acked = 1; c->status = psp_mmo_get_u32(p->data + 8); break;
    case MSG_S2C_KILL_REWARD: c->rewards++; break;
    case MSG_S2C_SPAWN_DROP: c->drops++; if (psp_mmo_get_u32(p->data + 28) & PSP_MMO_DROP_RARE) c->rares++; break;
    case MSG_S2C_SYSTEM_NOTICE:
        c->notices++;
        c->last_rgb = psp_mmo_get_u32(p->data + 8);
        memcpy(c->last_notice, p->data + 12, PSP_MMO_NOTICE_TEXT_LEN);
        break;
    case MSG_S2C_SERVER_EVENTS: c->events_msgs++; c->last_events = psp_mmo_get_u32(p->data + 8); break;
    }
}
static void cl_pump(client *c, unsigned ms) {
    const uint64_t end = clk_now_ns() + (uint64_t)ms * 1000000u;
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    do { t->poll_packets(t->ctx, cl_rx, c); sleep_ms(1); } while (clk_now_ns() < end);
}
static int cl_connect(client *c, uint16_t port) {
    memset(c, 0, sizeof *c);
    psp_net_enet_config cfg = { .connect_timeout_ms = 3000 };
    c->e = psp_net_enet_create(&cfg);
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const uint8_t mac[6] = { 0x00, 0x50, 0x81, 0x77, 0x00, 0x01 };
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    if (t->initialize(t->ctx, mac) != PSP_NET_OK || t->join_session(t->ctx, addr, "PSP2I") != PSP_NET_OK) return 0;
    uint8_t hs[PSP_MMO_HANDSHAKE_LEN] = { 0 };
    psp_mmo_put_hdr(hs, MSG_C2S_HANDSHAKE);
    memcpy(hs + 8, mac, 6);
    memcpy(hs + 14, "Watcher", 7);
    const psp_net_peer host = { { 0 }, 1 };
    t->send_packet(t->ctx, &host, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, hs, sizeof hs);
    for (int i = 0; i < 300 && !c->acked; i++) cl_pump(c, 5);
    return c->acked;
}
static void cl_kills(client *c, int n) {
    const psp_net_transport *t = psp_net_enet_transport(c->e);
    const psp_net_peer host = { { 0 }, 1 };
    uint8_t k[PSP_MMO_ENEMY_KILLED_LEN] = { 0 };
    psp_mmo_put_hdr(k, MSG_C2S_ENEMY_KILLED);
    psp_mmo_put_u32(k + 8, 5);
    const int want = c->rewards + n;
    for (int i = 0; i < n; i++) {
        t->send_packet(t->ctx, &host, PSP_NET_CH_RELIABLE, PSP_MMO_CTRL_PORT, k, sizeof k);
        if (i % 50 == 49) cl_pump(c, 20);
    }
    for (int i = 0; i < 500 && c->rewards < want; i++) cl_pump(c, 10);
}

static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "wb"); if (f) { fputs(text, f); fclose(f); } }

int main(void) {
    char dir[256], dbp[300], drops[300], rates[300];
    snprintf(dir, sizeof dir, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(dbp, sizeof dbp, "%s/psp2i_admin_%d.db", dir, (int)getpid());
    snprintf(drops, sizeof drops, "%s/psp2i_admin_drops_%d.json", dir, (int)getpid());
    snprintf(rates, sizeof rates, "%s/psp2i_admin_rates_%d.toml", dir, (int)getpid());
    write_file(drops, "{\"enemies\":[{\"id\":5,\"exp\":10,\"rares\":[{\"id\":900,\"chance\":0.1}]}]}");
    write_file(rates, "rare_drop_rate = 1.0\n");
    char err[256];
    db *d = db_open(dbp, quiet, err, sizeof err);
    CHECK(d != NULL);
    CHECK(db_account_create(d, "boss", "correct horse", 1) > 0 && db_account_create(d, "pleb", "password123", 0) > 0);

    srv_config cfg = { .mode = SRV_MODE_LAN, .bind = "127.0.0.1", .port = 0, .lan_port = 0, .name = "Admin Test", .max_players = 8,
                       .log = getenv("TEST_LOG") ? (void (*)(const char *))puts : quiet, .database = d,
                       .drop_tables_path = drops, .rates_path = rates, .kills_per_sec = 1000, .rng_seed = 7 };
    g_s = srv_create(&cfg);
    CHECK(srv_start(g_s) == 0);
    static volatile int quit;
    admin_ctx actx = { g_s, &g_tel, &quit };
    web_config wc = { "127.0.0.1", 0, &actx, cfg.log };
    g_w = web_create(&wc);
    CHECK(g_w != NULL);
    g_port = web_port(g_w);
    pthread_t th;
    pthread_create(&th, NULL, server_main, NULL);

    resp r;
    /* the page */
    CHECK(http("GET", "/", NULL, NULL, &r) == 200 && strstr(r.body, "<title>PSP2i Server Admin</title>") && strstr(r.head, "Content-Security-Policy"));
    CHECK(http("GET", "/app.js", NULL, NULL, &r) == 200 && strstr(r.head, "text/javascript") && strstr(r.body, "EventSource"));
    CHECK(http("GET", "/app.css", NULL, NULL, &r) == 200 && strstr(r.head, "text/css"));
    CHECK(http("GET", "/nope", NULL, NULL, &r) == 404);
    CHECK(http("GET", "/api/v1/status", NULL, NULL, &r) == 401);
    CHECK(http("GET", "/api/v1/status", "0000000000000000000000000000000000000000000000000000000000000000", NULL, &r) == 401);

    /* login */
    CHECK(http("POST", "/api/v1/login", NULL, "{\"username\":\"boss\",\"password\":\"wrong\"}", &r) == 401);
    CHECK(http("POST", "/api/v1/login", NULL, "{\"username\":\"pleb\",\"password\":\"password123\"}", &r) == 401);   /* not an admin */
    CHECK(http("POST", "/api/v1/login", NULL, "{\"username\":\"boss\",\"password\":\"correct horse\"}", &r) == 200);
    char tok[80];
    json_str(r.body, "token", tok, sizeof tok);
    CHECK(strlen(tok) == 64);

    /* a player */
    client c;
    CHECK(cl_connect(&c, srv_port(g_s)) && c.status == PSP_MMO_OK && c.events_msgs == 1);
    CHECK(http("GET", "/api/v1/status", tok, NULL, &r) == 200 && strstr(r.body, "\"name\":\"Watcher\"") && strstr(r.body, "\"ping_ms\":"));
    char session[24] = "";
    { const char *p = strstr(r.body, "\"session\":"); if (p) snprintf(session, sizeof session, "%llu", strtoull(p + 10, NULL, 10)); }
    CHECK(session[0] && strcmp(session, "0"));

    /* ---- GATE 1: rare_drop_rate 1.0 -> 3.0 from the panel ---- */
    cl_kills(&c, 1000);
    const int before = c.rares, before_kills = c.rewards;
    CHECK(http("POST", "/api/v1/rates", tok, "{\"rare_drop_rate\":3.0}", &r) == 200 && strstr(r.body, "\"ok\":true"));
    CHECK(http("GET", "/api/v1/rates", tok, NULL, &r) == 200 && strstr(r.body, "\"rare_drop_rate\":3"));
    cl_kills(&c, 1000);
    const int after = c.rares - before, after_kills = c.rewards - before_kills;
    printf("admin gate: rare drops in 1,000 kills: %d at 1.0x (10%% expected), %d after setting 3.0x from the panel (30%% expected)\n", before, after);
    CHECK(before_kills == 1000 && after_kills == 1000);
    CHECK(before >= 70 && before <= 130 && after >= 255 && after <= 345);
    CHECK(http("POST", "/api/v1/rates", tok, "{\"rare_drop_rate\":500}", &r) == 400);
    CHECK(http("POST", "/api/v1/rates", tok, "{\"bogus\":1}", &r) == 400);

    /* announcements, events */
    CHECK(http("POST", "/api/v1/announce", tok, "{\"text\":\"Restart in 10 min\",\"color\":\"#ff8800\"}", &r) == 200);
    cl_pump(&c, 150);
    CHECK(c.notices == 1 && c.last_rgb == 0xFF8800 && !strcmp(c.last_notice, "Restart in 10 min"));
    CHECK(http("POST", "/api/v1/events", tok, "{\"name\":\"halloween\",\"on\":true}", &r) == 200);
    cl_pump(&c, 150);
    CHECK(c.events_msgs == 2 && c.last_events == 1u << 2);
    CHECK(http("POST", "/api/v1/events", tok, "{\"name\":\"easter\",\"on\":true}", &r) == 400);

    /* drop tables editor */
    CHECK(http("POST", "/api/v1/drop_tables", tok, "{\"enemies\":[{\"id\":5,,}]}", &r) == 400 && strstr(r.body, "line 1, column"));
    CHECK(http("POST", "/api/v1/drop_tables", tok, "{\"enemies\":[{\"id\":5,\"exp\":11,\"rares\":[{\"id\":901,\"chance\":0.2}]}]}", &r) == 200);
    CHECK(http("GET", "/api/v1/drop_tables", tok, NULL, &r) == 200 && strstr(r.body, "\"id\":901"));
    CHECK(srv_enemy_count(g_s) == 1);

    /* RCON */
    CHECK(http("POST", "/api/v1/rcon", tok, "{\"command\":\"/status\"}", &r) == 200 && strstr(r.body, "Admin Test") && strstr(r.body, "rare 3.00"));
    CHECK(http("POST", "/api/v1/rcon", tok, "{\"command\":\"/frobnicate\"}", &r) == 200 && strstr(r.body, "\"ok\":false"));
    CHECK(http("POST", "/api/v1/rcon", tok, "{\"command\":\"/event spring on\"}", &r) == 200 && strstr(r.body, "\"ok\":true"));

    /* the event stream */
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET; a.sin_port = htons(g_port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        struct timeval tv = { 3, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
        char req[256], buf[16384] = "";
        snprintf(req, sizeof req, "GET /api/v1/stream?token=%s HTTP/1.1\r\nHost: x\r\n\r\n", tok);
        send(fd, req, strlen(req), 0);
        size_t got = 0;
        while (!strstr(buf, "data: {") && got < sizeof buf - 1) {
            const ssize_t k = recv(fd, buf + got, sizeof buf - 1 - got, 0);
            if (k <= 0) break;
            got += (size_t)k; buf[got] = 0;
        }
        CHECK(strstr(buf, "text/event-stream") && strstr(buf, "data: {\"name\":\"Admin Test\""));
        close(fd);
    }

    /* ---- GATE 2: ban from the panel ---- */
    char body[200];
    snprintf(body, sizeof body, "{\"target\":\"session:%s\",\"minutes\":0,\"reason\":\"griefing\"}", session);
    const uint64_t t0 = clk_now_ns();
    CHECK(http("POST", "/api/v1/ban", tok, body, &r) == 200 && strstr(r.body, "\"ok\":true"));
    uint32_t why = 0;
    while (!why && clk_now_ns() - t0 < 3000000000ull) { cl_pump(&c, 5); why = psp_net_enet_disconnect_reason(c.e); }
    const double ms = (double)(clk_now_ns() - t0) / 1e6;
    printf("admin gate: ban from the panel -> client disconnected with reason %u (BANNED = %d) after %.0f ms\n", why, PSP_MMO_DISC_BANNED, ms);
    CHECK(why == PSP_MMO_DISC_BANNED && ms < 1000);
    psp_net_enet_destroy(c.e);
    client c2;
    cl_connect(&c2, srv_port(g_s));                                     /* back again: refused */
    CHECK(c2.acked && c2.status == PSP_MMO_ERR_BANNED);
    for (int i = 0; i < 100 && !psp_net_enet_disconnect_reason(c2.e); i++) cl_pump(&c2, 10);
    CHECK(psp_net_enet_disconnect_reason(c2.e) == PSP_MMO_DISC_BANNED);
    psp_net_enet_destroy(c2.e);
    CHECK(http("GET", "/api/v1/bans", tok, NULL, &r) == 200 && strstr(r.body, "\"ip\":\"127.0.0.1\"") && strstr(r.body, "griefing"));
    CHECK(http("POST", "/api/v1/unban", tok, "{\"target\":\"ip:127.0.0.1\"}", &r) == 200);
    CHECK(cl_connect(&c2, srv_port(g_s)) && c2.status == PSP_MMO_OK);  /* unbanned: in */

    /* moderation of accounts */
    CHECK(http("GET", "/api/v1/accounts?q=ple", tok, NULL, &r) == 200 && strstr(r.body, "\"username\":\"pleb\"") && !strstr(r.body, "boss"));
    CHECK(http("POST", "/api/v1/admin", tok, "{\"account_id\":2,\"admin\":true}", &r) == 200);
    CHECK(http("POST", "/api/v1/ban", tok, "{\"target\":\"account:2\",\"minutes\":30,\"reason\":\"test\"}", &r) == 200);
    CHECK(http("POST", "/api/v1/login", NULL, "{\"username\":\"pleb\",\"password\":\"password123\"}", &r) == 401 && strstr(r.body, "banned"));
    CHECK(http("POST", "/api/v1/kick", tok, "{\"session\":999}", &r) == 400);

    /* the audit log */
    CHECK(http("GET", "/api/v1/audit?q=setrate", tok, NULL, &r) == 200 && strstr(r.body, "\"admin\":\"web:boss\"") &&
          strstr(r.body, "\"action\":\"setrate\"") && strstr(r.body, "\"to\":\"3\""));
    CHECK(http("GET", "/api/v1/audit", tok, NULL, &r) == 200 && strstr(r.body, "\"action\":\"announce\"") && strstr(r.body, "\"action\":\"ban\"") &&
          strstr(r.body, "\"action\":\"drop_tables\"") && strstr(r.body, "\"action\":\"event_on\"") && strstr(r.body, "rcon:boss") &&
          strstr(r.body, "\"action\":\"login\"") && strstr(r.body, "\"action\":\"promote\""));

    /* logout, throttling */
    CHECK(http("POST", "/api/v1/logout", tok, NULL, &r) == 200);
    CHECK(http("GET", "/api/v1/status", tok, NULL, &r) == 401);
    int code = 0;
    for (int i = 0; i < 7; i++) code = http("POST", "/api/v1/login", NULL, "{\"username\":\"boss\",\"password\":\"nope\"}", &r);
    CHECK(code == 429);

    g_run = 0;
    pthread_join(th, NULL);

    /* the console, on the same commands */
    char out[4096];
    CHECK(admin_exec(&actx, "console", "/help", out, sizeof out) == 0 && strstr(out, "/setrate"));
    CHECK(admin_exec(&actx, "console", "/setrate exp_rate 2.5", out, sizeof out) == 0 && srv_rates(g_s).exp == 2.5);
    CHECK(admin_exec(&actx, "console", "/setrate exp_rate lots", out, sizeof out) == -1);
    CHECK(admin_exec(&actx, "console", "/ban ip:10.1.2.3 60 spamming the lobby", out, sizeof out) == 0);
    CHECK(db_ip_banned(d, "10.1.2.3", out, sizeof out) == 1 && !strcmp(out, "spamming the lobby"));
    CHECK(admin_exec(&actx, "console", "/announce red Hello everyone", out, sizeof out) == 0);
    CHECK(admin_exec(&actx, "console", "/event christmas on", out, sizeof out) == 0 && (srv_events(g_s) & (1u << 3)));
    CHECK(admin_exec(&actx, "console", "/account add newbie hunter2hunter2 admin", out, sizeof out) == 0);
    CHECK(admin_exec(&actx, "console", "/audit spamming", out, sizeof out) == 0 && strstr(out, "console") && strstr(out, "10.1.2.3"));
    CHECK(admin_exec(&actx, "console", "/nope", out, sizeof out) == -1);
    CHECK(admin_exec(&actx, "console", "/shutdown", out, sizeof out) == 0 && quit == 1);
    CHECK(srv_rates(g_s).rare == 3.0);                                  /* still 3x in memory */

    psp_net_enet_destroy(c2.e);
    web_destroy(g_w);
    srv_destroy(g_s);
    db_close(d);
    remove(dbp); remove(drops); remove(rates);
    char side[320];
    snprintf(side, sizeof side, "%s-wal", dbp); remove(side);
    snprintf(side, sizeof side, "%s-shm", dbp); remove(side);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("admin: ok");
    return 0;
}
