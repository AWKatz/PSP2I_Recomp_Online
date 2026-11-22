/* Admin actions, shared by the stdin console, RCON and the web panel.
 *
 * Every action that changes something is written to audit_logs with its
 * actor ("console", "rcon:<admin>", "web:<admin>"). */
#ifndef PSP2I_SERVER_ADMIN_H
#define PSP2I_SERVER_ADMIN_H

#include <stddef.h>
#include <stdint.h>

#include "server.h"

/* Main-loop health, filled in by main.c each second. */
typedef struct {
    double   hz, tick_avg_ms, tick_max_ms, cpu_pct, rss_mb;
    uint64_t uptime_s, late_ticks;
} srv_telemetry;

typedef struct {
    srv *s;
    const srv_telemetry *tel;
    volatile int *quit;            /* /shutdown sets it (NULL: not allowed) */
} admin_ctx;

/* All return 0 on success, -1 on failure; msg (cap bytes) says what happened. */
int admin_kick(admin_ctx *a, const char *actor, uint64_t session, const char *reason, char *msg, size_t cap);
/* target: "session:N" (bans that player's IP), "ip:ADDR", "account:N". minutes 0 = permanent. */
int admin_ban(admin_ctx *a, const char *actor, const char *target, int64_t minutes, const char *reason, char *msg, size_t cap);
int admin_unban(admin_ctx *a, const char *actor, const char *target, char *msg, size_t cap);
int admin_set_admin(admin_ctx *a, const char *actor, int64_t account, int on, char *msg, size_t cap);
int admin_set_rate(admin_ctx *a, const char *actor, const char *name, double value, char *msg, size_t cap);
/* what: "drops", "rates" or "all" */
int admin_reload(admin_ctx *a, const char *actor, const char *what, char *msg, size_t cap);
int admin_announce(admin_ctx *a, const char *actor, const char *text, uint32_t rgb, char *msg, size_t cap);
int admin_set_event(admin_ctx *a, const char *actor, const char *name, int on, char *msg, size_t cap);
int admin_account_add(admin_ctx *a, const char *actor, const char *name, const char *password, int is_admin, char *msg, size_t cap);
/* Validate, then write the drop tables file (atomically) and reload it. On
 * a validation error nothing is written and msg names the place. */
int admin_save_drop_tables(admin_ctx *a, const char *actor, const char *json, char *msg, size_t cap);

/* "red", "#FF8800", "ff8800" -> 0xRRGGBB; -1 if not a colour. */
int64_t admin_parse_color(const char *s);

/* One console / RCON line ("/kick 3 spamming"); the reply in out. */
int admin_exec(admin_ctx *a, const char *actor, const char *line, char *out, size_t cap);

#endif
