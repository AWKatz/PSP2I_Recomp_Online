/* psp2i_server: the PSP2i dedicated server (headless).
 *
 *   psp2i_server --lan    [--port 29001] [--lan-port 29000] [--name "..."]
 *   psp2i_server --online [--port 29001] [--bind ADDR] [--rate-limit 120]
 *
 * A fixed 30 Hz tick (33.33 ms): each tick reads the network, runs session
 * upkeep and answers LAN probes, then sleeps to the next deadline. Ctrl-C or
 * SIGTERM shuts down cleanly. See --help. */
#include "clock.h"
#include "server.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK_HZ 30
#define TICK_NS (1000000000ull / TICK_HZ)

static volatile sig_atomic_t g_quit;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

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
         "  --max-players N        1..63 (default 32)\n"
         "  --rate-limit N         online: packets per second per player (default 120, burst 2x)\n"
         "  --handshake-timeout S  seconds a new connection has to send its handshake (default 5)\n"
         "  --stats S              print a status line every S seconds (default 60, 0 = off)\n"
         "  --run-seconds S        exit after S seconds (tests, measurements)\n"
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
        else if (!strcmp(a, "--max-players")) { i = need(i, argc, a); cfg.max_players = (int)num(argv[i], a, 1, 63); }
        else if (!strcmp(a, "--rate-limit")) { i = need(i, argc, a); cfg.rate_pps = (uint32_t)num(argv[i], a, 1, 100000); }
        else if (!strcmp(a, "--handshake-timeout")) { i = need(i, argc, a); cfg.handshake_timeout_ms = (uint32_t)num(argv[i], a, 1, 3600) * 1000u; }
        else if (!strcmp(a, "--stats")) { i = need(i, argc, a); stats_s = num(argv[i], a, 0, 86400); }
        else if (!strcmp(a, "--run-seconds")) { i = need(i, argc, a); run_s = num(argv[i], a, 1, 31536000); }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "psp2i_server: unknown option '%s' (see --help)\n", a); return 2; }
    }
    if (cfg.mode == SRV_MODE_LAN && cfg.lan_port && cfg.lan_port == cfg.port) {
        fputs("psp2i_server: --port and --lan-port must differ\n", stderr);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    srv *s = srv_create(&cfg);
    if (!s || srv_start(s) != 0) { srv_destroy(s); return 1; }

    /* the fixed-timestep loop */
    const uint64_t t0 = clk_now_ns();
    uint64_t next = t0, ticks = 0, late = 0, skipped = 0, work_max = 0, work_sum = 0;
    uint64_t stat_t = t0, stat_cpu = clk_cpu_ns(), stat_ticks = 0;
    while (!g_quit) {
        const uint64_t start = clk_now_ns();
        srv_tick(s, start / 1000000u);
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

        if (stats_s && done - stat_t >= (uint64_t)stats_s * 1000000000u) {
            srv_stats st;
            srv_get_stats(s, &st);
            const uint64_t cpu = clk_cpu_ns();
            const double secs = (double)(done - stat_t) / 1e9;
            logf_("status: %d players, %d pending | %.1f Hz, tick work avg %.3f ms max %.3f ms, %llu late | "
                  "cpu %.2f%% | rss %.1f MB | in %llu, dropped %llu, lan replies %llu",
                  st.players, st.pending, (double)(ticks - stat_ticks) / secs,
                  (double)work_sum / (double)ticks / 1e6, (double)work_max / 1e6, (unsigned long long)late,
                  (double)(cpu - stat_cpu) / 1e9 / secs * 100.0, (double)clk_rss_bytes() / 1048576.0,
                  (unsigned long long)st.packets_in, (unsigned long long)st.packets_dropped,
                  (unsigned long long)st.lan_replies);
            stat_t = done; stat_cpu = cpu; stat_ticks = ticks;
        }
        clk_sleep_until_ns(next);
    }

    const double up = (double)(clk_now_ns() - t0) / 1e9;
    logf_("shutting down after %.1f s: %llu ticks (%.2f Hz), %llu late, %llu skipped, tick work avg %.3f ms max %.3f ms, "
          "cpu %.2f%%, rss %.1f MB",
          up, (unsigned long long)ticks, (double)ticks / up, (unsigned long long)late, (unsigned long long)skipped,
          ticks ? (double)work_sum / (double)ticks / 1e6 : 0.0, (double)work_max / 1e6,
          (double)clk_cpu_ns() / 1e9 / up * 100.0, (double)clk_rss_bytes() / 1048576.0);
    srv_destroy(s);
    return 0;
}
