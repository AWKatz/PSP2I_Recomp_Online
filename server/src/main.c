/* psp2i_server: the PSP2i dedicated server (headless).
 *
 *   psp2i_server --lan    [--port 29001] [--lan-port 29000] [--name "..."]
 *   psp2i_server --online [--port 29001] [--bind ADDR] [--rate-limit 120]
 *
 * A fixed tick, 60 Hz by default (16.67 ms; --tick-rate): each tick reads the
 * network, runs session upkeep and answers LAN probes, then waits on the
 * network until the next deadline (game traffic is relayed as it arrives). Ctrl-C or
 * SIGTERM shuts down cleanly. See --help. */
#include "admin.h"
#include "clock.h"
#include "console.h"
#include "web.h"
#include "db.h"
#include "sqlite3.h"
#include "server.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_HZ_DEFAULT 60                /* the clients run at 60 fps */
static unsigned g_tick_hz = TICK_HZ_DEFAULT;
#define TICK_NS (1000000000ull / g_tick_hz)

static volatile int g_quit;
static volatile sig_atomic_t g_reload;
static void on_signal(int sig) { (void)sig; g_quit = 1; }
#ifdef SIGHUP
static void on_hup(int sig) { (void)sig; g_reload = 1; }
#endif

static void log_line(const char *line) {
    char ts[32];
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s] %s\n", ts, line);
    fflush(stdout);
}

static void logf_(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_line(line);
}

static void usage(void) {
    puts("psp2i_server -- PSP2i dedicated server\n"
         "\n"
         "  --lan                  LAN party: bind 0.0.0.0, answer discovery probes (default)\n"
         "  --online               public server: strict rate limits, no discovery\n"
         "  --port N               game port, UDP (default 29001)\n"
         "  --bind ADDR            address to bind (default: all interfaces)\n"
         "  --lan-port N           LAN discovery port, UDP (default 29000)\n"
         "  --name NAME            server name shown to LAN clients\n"
         "  --max-players N        1..12 (default 12, as the original servers)\n"
         "  --rate-limit N         online: packets per second per player (default 120, burst 2x)\n"
         "  --handshake-timeout S  seconds a new connection has to send its handshake (default 5)\n"
         "  --tick-rate N          server ticks per second, 10..120 (default 60)\n"
         "  --stats S              print a status line every S seconds (default 60, 0 = off)\n"
         "  --run-seconds S        exit after S seconds (tests, measurements)\n"
         "  --db PATH              the database (default psp2i_server.db; SQLite, WAL)\n"
         "  --drop-tables PATH     loot tables, JSON (default config/drop_tables.json; reloaded on change)\n"
         "  --rates PATH           rate multipliers, TOML (default config/rates.toml; reloaded on change)\n"
         "  --kill-limit N         enemy kill reports per second per player (default 10)\n"
         "  --web-port N           serve the admin panel on this TCP port (off by default)\n"
         "  --web-bind ADDR        the panel's address (default 127.0.0.1: this machine only)\n"
         "  --no-console           do not read commands from stdin\n"
         "  --add-admin NAME PASS  create an admin account in the database and exit\n"
         "\n"
         "SIGHUP reloads the drop tables and rates at once.\n"
         "  --help");
}

static int need(int i, int argc, const char *flag) {
    if (i + 1 >= argc) { fprintf(stderr, "psp2i_server: %s needs a value\n", flag); exit(2); }
    return i + 1;
}

static long num(const char *s, const char *flag, long lo, long hi) {
    char *end;
    const long v = strtol(s, &end, 10);
    if (*s == 0 || *end || v < lo || v > hi) {
        fprintf(stderr, "psp2i_server: %s: '%s' is not a number from %ld to %ld\n", flag, s, lo, hi);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv) {
    srv_config cfg = { .mode = SRV_MODE_LAN, .port = 29001, .lan_port = 29000, .log = log_line };
    int mode_set = 0;
    long stats_s = 60, run_s = 0;
    const char *db_path = "psp2i_server.db";
    const char *web_bind = "127.0.0.1", *add_name = NULL, *add_pass = NULL;
    long web_port = -1;
    int use_console = 1;
    cfg.drop_tables_path = "config/drop_tables.json";
    cfg.rates_path = "config/rates.toml";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--lan") || !strcmp(a, "--online")) {
            const srv_mode m = a[2] == 'l' ? SRV_MODE_LAN : SRV_MODE_ONLINE;
            if (mode_set && cfg.mode != m) { fputs("psp2i_server: --lan and --online are exclusive\n", stderr); return 2; }
            cfg.mode = m;
            mode_set = 1;
        }
        else if (!strcmp(a, "--port")) { i = need(i, argc, a); cfg.port = (uint16_t)num(argv[i], a, 0, 65535); }
        else if (!strcmp(a, "--bind")) { i = need(i, argc, a); cfg.bind = argv[i]; }
        else if (!strcmp(a, "--lan-port")) { i = need(i, argc, a); cfg.lan_port = (uint16_t)num(argv[i], a, 0, 65535); }
        else if (!strcmp(a, "--name")) { i = need(i, argc, a); cfg.name = argv[i]; }
        else if (!strcmp(a, "--max-players")) { i = need(i, argc, a); cfg.max_players = (int)num(argv[i], a, 1, SRV_MAX_PLAYERS); }
        else if (!strcmp(a, "--rate-limit")) { i = need(i, argc, a); cfg.rate_pps = (uint32_t)num(argv[i], a, 1, 100000); }
        else if (!strcmp(a, "--handshake-timeout")) { i = need(i, argc, a); cfg.handshake_timeout_ms = (uint32_t)num(argv[i], a, 1, 3600) * 1000u; }
        else if (!strcmp(a, "--tick-rate")) { i = need(i, argc, a); g_tick_hz = (unsigned)num(argv[i], a, 10, 120); }
        else if (!strcmp(a, "--stats")) { i = need(i, argc, a); stats_s = num(argv[i], a, 0, 86400); }
        else if (!strcmp(a, "--run-seconds")) { i = need(i, argc, a); run_s = num(argv[i], a, 1, 31536000); }
        else if (!strcmp(a, "--db")) { i = need(i, argc, a); db_path = argv[i]; }
        else if (!strcmp(a, "--drop-tables")) { i = need(i, argc, a); cfg.drop_tables_path = argv[i]; }
        else if (!strcmp(a, "--rates")) { i = need(i, argc, a); cfg.rates_path = argv[i]; }
        else if (!strcmp(a, "--kill-limit")) { i = need(i, argc, a); cfg.kills_per_sec = (uint32_t)num(argv[i], a, 1, 1000); }
        else if (!strcmp(a, "--web-port")) { i = need(i, argc, a); web_port = num(argv[i], a, 0, 65535); }
        else if (!strcmp(a, "--web-bind")) { i = need(i, argc, a); web_bind = argv[i]; }
        else if (!strcmp(a, "--no-console")) use_console = 0;
        else if (!strcmp(a, "--add-admin")) { i = need(i, argc, a); add_name = argv[i]; i = need(i, argc, a); add_pass = argv[i]; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "psp2i_server: unknown option '%s' (see --help)\n", a); return 2; }
    }
    if (cfg.mode == SRV_MODE_LAN && cfg.lan_port && cfg.lan_port == cfg.port) {
        fputs("psp2i_server: --port and --lan-port must differ\n", stderr);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGHUP
    signal(SIGHUP, on_hup);
#endif

    char err[512];
    db *database = db_open(db_path, log_line, err, sizeof err);
    if (!database) { logf_("database: %s", err); return 1; }
    {
        sqlite3_stmt *st;
        long long acc = 0, chr = 0, items = 0;
        if (sqlite3_prepare_v2(db_handle(database), "SELECT (SELECT count(*) FROM accounts), (SELECT count(*) FROM characters), "
                               "(SELECT count(*) FROM item_ledger)", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            acc = sqlite3_column_int64(st, 0); chr = sqlite3_column_int64(st, 1); items = sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
        logf_("database: %s (SQLite %s, WAL, schema v%d): %lld accounts, %lld characters, %lld items",
              db_path, sqlite3_libversion(), DB_SCHEMA_VERSION, acc, chr, items);
    }

    cfg.database = database;
    srv *s = srv_create(&cfg);
    srv_telemetry tel = { 0 };
    admin_ctx actx = { s, &tel, &g_quit };
    char out[8192];
    if (add_name) {                                   /* --add-admin: no network, just the account */
        const int rc = admin_account_add(&actx, "console", add_name, add_pass, 1, out, sizeof out);
        logf_("%s", out);
        srv_destroy(s);
        db_close(database);
        return rc ? 1 : 0;
    }
    if (!s || srv_start(s) != 0) { srv_destroy(s); db_close(database); return 1; }
    web *wp = NULL;
    if (web_port >= 0) {
        web_config wc = { web_bind, (uint16_t)web_port, &actx, log_line };
        wp = web_create(&wc);
        if (!wp) { srv_destroy(s); db_close(database); return 1; }
        const char *none[1] = { NULL };
        char *admins = db_json(database, "SELECT count(*) FROM accounts WHERE is_admin = 1", 0, none);
        if (admins && !strcmp(admins, "0"))
            logf_("web: no admin accounts yet: create one with --add-admin NAME PASSWORD or /account add NAME PASSWORD admin");
        free(admins);
    }
    if (use_console && console_start() == 0) logf_("console: type /help");

    /* the fixed-timestep loop */
    const uint64_t t0 = clk_now_ns();
    uint64_t next = t0, ticks = 0, late = 0, skipped = 0, work_max = 0, work_sum = 0;
    uint64_t stat_t = t0, stat_cpu = clk_cpu_ns(), stat_ticks = 0;
    uint64_t tel_t = t0, tel_cpu = stat_cpu, tel_ticks = 0, tel_sum = 0, tel_max = 0;
    while (!g_quit) {
        const uint64_t start = clk_now_ns();
        if (g_reload) { g_reload = 0; srv_reload(s); }
        srv_tick(s, start / 1000000u);
        web_tick(wp, start / 1000000u);
        char line[1024];
        while (console_poll(line, sizeof line)) {
            admin_exec(&actx, "console", line, out, sizeof out);
            if (out[0]) { fputs(out, stdout); fputc('\n', stdout); fflush(stdout); }
        }
        const uint64_t done = clk_now_ns(), work = done - start;
        ticks++;
        work_sum += work;
        if (work > work_max) work_max = work;

        next += TICK_NS;
        if (done > next) {                            /* the tick overran its slot */
            late++;
            if (done - next > 5 * TICK_NS) {          /* far behind (e.g. suspended): resync, no burst */
                skipped += (done - next) / TICK_NS;
                next = done;
            }
        }
        if (run_s && done - t0 >= (uint64_t)run_s * 1000000000u) break;

        /* telemetry for /status and the panel, once a second */
        tel_sum += work;
        if (work > tel_max) tel_max = work;
        if (done - tel_t >= 1000000000u) {
            const double secs = (double)(done - tel_t) / 1e9;
            const uint64_t cpu = clk_cpu_ns(), n = ticks - tel_ticks;
            tel.hz = (double)n / secs;
            tel.tick_avg_ms = n ? (double)tel_sum / (double)n / 1e6 : 0;
            tel.tick_max_ms = (double)tel_max / 1e6;
            tel.cpu_pct = (double)(cpu - tel_cpu) / 1e9 / secs * 100.0;
            tel.rss_mb = (double)clk_rss_bytes() / 1048576.0;
            tel.uptime_s = (done - t0) / 1000000000u;
            tel.late_ticks = late;
            tel_t = done; tel_cpu = cpu; tel_ticks = ticks; tel_sum = 0; tel_max = 0;
        }

        if (stats_s && done - stat_t >= (uint64_t)stats_s * 1000000000u) {
            srv_stats st;
            srv_get_stats(s, &st);
            const uint64_t cpu = clk_cpu_ns();
            const double secs = (double)(done - stat_t) / 1e9;
            logf_("status: %d players, %d pending | %.1f Hz, tick work avg %.3f ms max %.3f ms, %llu late | "
                  "cpu %.2f%% | rss %.1f MB | in %llu, dropped %llu, lan replies %llu | kills %llu, drops %llu (rare %llu)",
                  st.players, st.pending, (double)(ticks - stat_ticks) / secs,
                  (double)work_sum / (double)ticks / 1e6, (double)work_max / 1e6, (unsigned long long)late,
                  (double)(cpu - stat_cpu) / 1e9 / secs * 100.0, (double)clk_rss_bytes() / 1048576.0,
                  (unsigned long long)st.packets_in, (unsigned long long)st.packets_dropped,
                  (unsigned long long)st.lan_replies, (unsigned long long)st.kills, (unsigned long long)st.drops,
                  (unsigned long long)st.rare_drops);
            stat_t = done; stat_cpu = cpu; stat_ticks = ticks;
        }
        /* until the next tick: wait on the network, relaying as packets arrive */
        for (uint64_t now = clk_now_ns(); now < next; now = clk_now_ns()) {
            const uint64_t left_ms = (next - now) / 1000000u;
            if (left_ms == 0) { clk_sleep_until_ns(next); break; }
            srv_wait(s, (uint32_t)left_ms);
        }
    }

    const double up = (double)(clk_now_ns() - t0) / 1e9;
    logf_("shutting down after %.1f s: %llu ticks (%.2f Hz), %llu late, %llu skipped, tick work avg %.3f ms max %.3f ms, "
          "cpu %.2f%%, rss %.1f MB",
          up, (unsigned long long)ticks, (double)ticks / up, (unsigned long long)late, (unsigned long long)skipped,
          ticks ? (double)work_sum / (double)ticks / 1e6 : 0.0, (double)work_max / 1e6,
          (double)clk_cpu_ns() / 1e9 / up * 100.0, (double)clk_rss_bytes() / 1048576.0);
    web_destroy(wp);
    srv_destroy(s);
    db_close(database);                               /* checkpoints the WAL */
    return 0;
}
