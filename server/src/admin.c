/* Admin actions and the console / RCON command language (see admin.h). */
#include "admin.h"

#include "db.h"
#include "loot.h"
#include "sqlite3.h"

#include <psprecomp/net_proto.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int say(char *msg, size_t cap, int rc, const char *fmt, ...) {
    if (msg && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, cap, fmt, ap);
        va_end(ap);
    }
    return rc;
}

/* audit with details built by SQLite (proper JSON escaping) */
static void audit(admin_ctx *a, const char *actor, const char *action, const char *target, const char *k1, const char *v1,
                  const char *k2, const char *v2) {
    db *d = srv_db(a->s);
    if (!d) return;
    char *details = NULL;
    if (k1) {
        const char *p[4] = { k1, v1, k2 ? k2 : "_", v2 };
        details = db_json(d, k2 ? "SELECT json_object(?1, ?2, ?3, ?4)" : "SELECT json_object(?1, ?2)", k2 ? 4 : 2, p);
    }
    db_audit(d, actor, action, target, details);
    free(details);
}

int64_t admin_parse_color(const char *s) {
    static const struct { const char *n; uint32_t v; } NAMED[] = {
        { "white", 0xFFFFFF }, { "yellow", 0xFFD84A }, { "red", 0xFF5A5A }, { "green", 0x5AD16E },
        { "blue", 0x5AA0FF }, { "cyan", 0x4DE1E6 }, { "magenta", 0xE07BFF }, { "orange", 0xFFA040 } };
    if (!s || !*s) return -1;
    for (size_t i = 0; i < sizeof NAMED / sizeof *NAMED; i++) if (!strcmp(s, NAMED[i].n)) return NAMED[i].v;
    if (*s == '#') s++;
    if (strlen(s) != 6) return -1;
    for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return -1;
    return (int64_t)strtol(s, NULL, 16);
}

int admin_kick(admin_ctx *a, const char *actor, uint64_t session, const char *reason, char *msg, size_t cap) {
    srv_player p[64];
    const int n = srv_players(a->s, p, 64);
    const char *name = NULL;
    for (int i = 0; i < n; i++) if (p[i].session == session) name = p[i].name;
    if (!name) return say(msg, cap, -1, "no player in session %llu", (unsigned long long)session);
    char who[40], target[32];
    snprintf(who, sizeof who, "%s", name);
    snprintf(target, sizeof target, "session:%llu", (unsigned long long)session);
    srv_kick(a->s, session, PSP_MMO_DISC_KICKED);
    audit(a, actor, "kick", target, "name", who, reason && *reason ? "reason" : NULL, reason);
    return say(msg, cap, 0, "kicked '%s' (session %llu)", who, (unsigned long long)session);
}

int admin_ban(admin_ctx *a, const char *actor, const char *target, int64_t minutes, const char *reason, char *msg, size_t cap) {
    db *d = srv_db(a->s);
    if (!d) return say(msg, cap, -1, "bans need the database");
    if (minutes < 0) return say(msg, cap, -1, "minutes must be 0 (permanent) or more");
    const int64_t expires = minutes > 0 ? (int64_t)time(NULL) + minutes * 60 : 0;
    char span[48];
    if (minutes) snprintf(span, sizeof span, "for %lld min", (long long)minutes); else snprintf(span, sizeof span, "permanently");
    if (!strncmp(target, "session:", 8)) {
        char ip[64];
        if (srv_ban_session(a->s, strtoull(target + 8, NULL, 10), reason, minutes, actor, ip) != 0)
            return say(msg, cap, -1, "no player in %s", target);
        return say(msg, cap, 0, "banned %s (ip %s) %s", target, ip, span);
    }
    if (!strncmp(target, "ip:", 3) && target[3]) {
        if (db_ip_ban(d, target + 3, reason, expires, actor) != DB_OK) return say(msg, cap, -1, "could not ban %s", target);
        const int n = srv_kick_ip(a->s, target + 3, PSP_MMO_DISC_BANNED);
        return say(msg, cap, 0, "banned %s %s (%d disconnected)", target, span, n);
    }
    if (!strncmp(target, "account:", 8)) {
        const int rc = db_account_ban(d, strtoll(target + 8, NULL, 10), reason, expires, actor);
        if (rc == DB_NOT_FOUND) return say(msg, cap, -1, "no %s", target);
        return rc == DB_OK ? say(msg, cap, 0, "banned %s %s", target, span) : say(msg, cap, -1, "could not ban %s", target);
    }
    return say(msg, cap, -1, "ban what? session:N, ip:ADDR or account:N");
}

int admin_unban(admin_ctx *a, const char *actor, const char *target, char *msg, size_t cap) {
    db *d = srv_db(a->s);
    if (!d) return say(msg, cap, -1, "bans need the database");
    int rc;
    if (!strncmp(target, "ip:", 3)) rc = db_ip_unban(d, target + 3, actor);
    else if (!strncmp(target, "account:", 8)) rc = db_account_unban(d, strtoll(target + 8, NULL, 10), actor);
    else return say(msg, cap, -1, "unban what? ip:ADDR or account:N");
    if (rc == DB_NOT_FOUND) return say(msg, cap, -1, "%s is not banned", target);
    return rc == DB_OK ? say(msg, cap, 0, "unbanned %s", target) : say(msg, cap, -1, "could not unban %s", target);
}

int admin_set_admin(admin_ctx *a, const char *actor, int64_t account, int on, char *msg, size_t cap) {
    db *d = srv_db(a->s);
    if (!d) return say(msg, cap, -1, "accounts need the database");
    const int rc = db_account_set_admin(d, account, on, actor);
    if (rc == DB_NOT_FOUND) return say(msg, cap, -1, "no account %lld", (long long)account);
    return rc == DB_OK ? say(msg, cap, 0, "account %lld %s", (long long)account, on ? "is now an admin" : "is no longer an admin")
                       : say(msg, cap, -1, "could not change account %lld", (long long)account);
}

int admin_set_rate(admin_ctx *a, const char *actor, const char *name, double value, char *msg, size_t cap) {
    loot_rates r = srv_rates(a->s);
    double before;
    if (!strcmp(name, "exp_rate")) before = r.exp;
    else if (!strcmp(name, "drop_rate")) before = r.drop;
    else if (!strcmp(name, "rare_drop_rate")) before = r.rare;
    else if (!strcmp(name, "meseta_rate")) before = r.meseta;
    else if (!strcmp(name, "type_points_rate")) before = r.type_points;
    else return say(msg, cap, -1, "unknown rate '%s' (exp_rate, drop_rate, rare_drop_rate, meseta_rate, type_points_rate)", name);
    if (srv_set_rate(a->s, name, value) != 0) return say(msg, cap, -1, "%s must be from 0 to 100", name);
    char f[32], t[32];
    snprintf(f, sizeof f, "%g", before);
    snprintf(t, sizeof t, "%g", value);
    audit(a, actor, "setrate", name, "from", f, "to", t);
    return say(msg, cap, 0, "%s: %g -> %g", name, before, value);
}

int admin_reload(admin_ctx *a, const char *actor, const char *what, char *msg, size_t cap) {
    if (strcmp(what, "drops") && strcmp(what, "rates") && strcmp(what, "all"))
        return say(msg, cap, -1, "reload what? drops, rates or all");
    srv_stats before, after;
    srv_get_stats(a->s, &before);
    if (!strcmp(what, "drops")) srv_reload_drops(a->s);   /* a broken file keeps the old */
    else if (!strcmp(what, "rates")) srv_reload_rates(a->s);
    else srv_reload(a->s);
    srv_get_stats(a->s, &after);
    audit(a, actor, "reload", what, NULL, NULL, NULL, NULL);
    if (after.reload_errors > before.reload_errors) return say(msg, cap, -1, "reloaded with errors: the server log says which (the previous version stays)");
    return say(msg, cap, 0, "reloaded: %d enemies; rates exp %.2f drop %.2f rare %.2f meseta %.2f type points %.2f",
               srv_enemy_count(a->s), srv_rates(a->s).exp, srv_rates(a->s).drop, srv_rates(a->s).rare,
               srv_rates(a->s).meseta, srv_rates(a->s).type_points);
}

int admin_announce(admin_ctx *a, const char *actor, const char *text, uint32_t rgb, char *msg, size_t cap) {
    if (!text || !*text) return say(msg, cap, -1, "nothing to announce");
    if (strlen(text) >= PSP_MMO_NOTICE_TEXT_LEN) return say(msg, cap, -1, "at most %d bytes", PSP_MMO_NOTICE_TEXT_LEN - 1);
    const int n = srv_announce(a->s, text, rgb);
    char col[16];
    snprintf(col, sizeof col, "#%06X", (unsigned)rgb);
    audit(a, actor, "announce", NULL, "text", text, "color", col);
    return say(msg, cap, 0, "announced to %d player%s", n < 0 ? 0 : n, n == 1 ? "" : "s");
}

int admin_set_event(admin_ctx *a, const char *actor, const char *name, int on, char *msg, size_t cap) {
    if (srv_set_event(a->s, name, on) != 0) {
        char list[128] = "";
        for (int i = 0; srv_event_name(i); i++) { strcat(list, i ? ", " : ""); strcat(list, srv_event_name(i)); }
        return say(msg, cap, -1, "unknown event '%s' (%s)", name, list);
    }
    audit(a, actor, on ? "event_on" : "event_off", name, NULL, NULL, NULL, NULL);
    return say(msg, cap, 0, "event %s %s", name, on ? "on" : "off");
}

int admin_account_add(admin_ctx *a, const char *actor, const char *name, const char *password, int is_admin, char *msg, size_t cap) {
    db *d = srv_db(a->s);
    if (!d) return say(msg, cap, -1, "accounts need the database");
    if (!name || !*name || strlen(name) > 32) return say(msg, cap, -1, "a name of 1 to 32 characters");
    if (!password || strlen(password) < 8) return say(msg, cap, -1, "a password of at least 8 characters");
    const int64_t id = db_account_create(d, name, password, is_admin);
    if (id == DB_EXISTS) return say(msg, cap, -1, "'%s' already exists", name);
    if (id <= 0) return say(msg, cap, -1, "could not create '%s'", name);
    char target[32];
    snprintf(target, sizeof target, "account:%lld", (long long)id);
    audit(a, actor, "account_add", target, "username", name, "admin", is_admin ? "yes" : "no");
    return say(msg, cap, 0, "account %lld '%s' created%s", (long long)id, name, is_admin ? " (admin)" : "");
}

int admin_save_drop_tables(admin_ctx *a, const char *actor, const char *json, char *msg, size_t cap) {
    const char *path = srv_drop_tables_path(a->s);
    if (!path) return say(msg, cap, -1, "the server has no drop tables file");
    char err[256];
    loot_table *t = loot_parse(json, err, sizeof err);
    if (!t) return say(msg, cap, -1, "%s", err);
    const int ne = t->n_enemies, nm = t->n_missions;
    loot_free(t);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return say(msg, cap, -1, "cannot write %s", tmp);
    const size_t n = strlen(json);
    const int ok = fwrite(json, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { remove(tmp); return say(msg, cap, -1, "cannot write %s", tmp); }
#ifdef _WIN32
    remove(path);                                     /* rename does not replace on Windows */
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return say(msg, cap, -1, "cannot replace %s", path); }
    srv_reload_drops(a->s);                           /* live rate changes stay */
    char counts[64];
    snprintf(counts, sizeof counts, "%d enemies, %d missions", ne, nm);
    audit(a, actor, "drop_tables", path, "result", counts, NULL, NULL);
    return say(msg, cap, 0, "saved and reloaded: %s", counts);
}

/* ---- the command language ---- */

static const char HELP[] =
    "/status                          server health and players\n"
    "/players                         who is on (session, name, ip, ping)\n"
    "/kick <session> [reason]\n"
    "/ban <session|ip:ADDR|account:N> [minutes, 0 = permanent] [reason]\n"
    "/unban <ip:ADDR|account:N>\n"
    "/account add <name> <password> [admin]\n"
    "/admin <account id> on|off       promote / demote\n"
    "/announce [colour] <text>        colours: white yellow red green blue cyan magenta orange #RRGGBB\n"
    "/setrate <name> <value>          exp_rate drop_rate rare_drop_rate meseta_rate type_points_rate\n"
    "/rates\n"
    "/reload drops|rates|all\n"
    "/event <name> on|off             /events lists them\n"
    "/audit [text]                    the last 15 audit entries (matching text)\n"
    "/shutdown\n"
    "/help";

static char *next_word(char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
    if (!**p) return NULL;
    char *w = *p;
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    if (**p) *(*p)++ = 0;
    return w;
}
/* The next word, copied (p is not moved); 0 if there is none. */
static int peek_word(const char *p, char *w, size_t cap) {
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && n + 1 < cap) w[n++] = *p++;
    w[n] = 0;
    return n > 0;
}
static char *rest(char **p) { while (**p == ' ' || **p == '\t') (*p)++; return *p; }

static int status_text(admin_ctx *a, char *out, size_t cap) {
    srv_stats st;
    srv_get_stats(a->s, &st);
    const srv_telemetry *t = a->tel;
    const loot_rates r = srv_rates(a->s);
    size_t n = (size_t)snprintf(out, cap, "'%s' (%s): %d/%d players, %d connecting | up %llus, %.1f Hz, tick %.3f ms avg %.3f max, cpu %.2f%%, rss %.1f MB\n"
                                "rates: exp %.2f drop %.2f rare %.2f meseta %.2f type points %.2f | kills %llu, drops %llu (rare %llu)",
                                srv_name(a->s), srv_mode_online(a->s) ? "online" : "LAN", st.players, srv_max_players(a->s), st.pending,
                                t ? (unsigned long long)t->uptime_s : 0ull, t ? t->hz : 0, t ? t->tick_avg_ms : 0, t ? t->tick_max_ms : 0,
                                t ? t->cpu_pct : 0, t ? t->rss_mb : 0, r.exp, r.drop, r.rare, r.meseta, r.type_points,
                                (unsigned long long)st.kills, (unsigned long long)st.drops, (unsigned long long)st.rare_drops);
    return n < cap ? 0 : 0;
}

int admin_exec(admin_ctx *a, const char *actor, const char *line, char *out, size_t cap) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", line ? line : "");
    char *p = buf;
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) buf[--len] = 0;
    char *cmd = next_word(&p);
    if (!cmd) return say(out, cap, 0, "");
    if (*cmd == '/') cmd++;

    if (!strcmp(cmd, "help")) return say(out, cap, 0, "%s", HELP);
    if (!strcmp(cmd, "status")) return status_text(a, out, cap);
    if (!strcmp(cmd, "players")) {
        srv_player pl[64];
        const int n = srv_players(a->s, pl, 64);
        size_t o = (size_t)snprintf(out, cap, "%d player%s", n, n == 1 ? "" : "s");
        for (int i = 0; i < n && o < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "\n  %3llu  %-20s %-21s %-15s %4d ms  %s", (unsigned long long)pl[i].session, pl[i].name,
                                  pl[i].ip, pl[i].lan_ip[0] ? pl[i].lan_ip : "-", pl[i].ping_ms, pl[i].mac);
        return 0;
    }
    if (!strcmp(cmd, "kick")) {
        char *s = next_word(&p);
        if (!s) return say(out, cap, -1, "usage: /kick <session> [reason]");
        return admin_kick(a, actor, strtoull(s, NULL, 10), rest(&p), out, cap);
    }
    if (!strcmp(cmd, "ban")) {
        char *t = next_word(&p);
        if (!t) return say(out, cap, -1, "usage: /ban <session|ip:ADDR|account:N> [minutes] [reason]");
        char target[96];
        if (isdigit((unsigned char)*t)) snprintf(target, sizeof target, "session:%s", t); else snprintf(target, sizeof target, "%s", t);
        char word[32];
        int64_t minutes = 0;
        if (peek_word(p, word, sizeof word) && word[0] && strspn(word, "0123456789") == strlen(word)) {
            minutes = strtoll(word, NULL, 10);
            next_word(&p);
        }
        return admin_ban(a, actor, target, minutes, rest(&p), out, cap);
    }
    if (!strcmp(cmd, "unban")) {
        char *t = next_word(&p);
        return t ? admin_unban(a, actor, t, out, cap) : say(out, cap, -1, "usage: /unban <ip:ADDR|account:N>");
    }
    if (!strcmp(cmd, "account")) {
        char *sub = next_word(&p), *name = next_word(&p), *pw = next_word(&p), *adm = next_word(&p);
        if (!sub || strcmp(sub, "add") || !name || !pw) return say(out, cap, -1, "usage: /account add <name> <password> [admin]");
        return admin_account_add(a, actor, name, pw, adm && !strcmp(adm, "admin"), out, cap);
    }
    if (!strcmp(cmd, "admin")) {
        char *id = next_word(&p), *on = next_word(&p);
        if (!id || !on || (strcmp(on, "on") && strcmp(on, "off"))) return say(out, cap, -1, "usage: /admin <account id> on|off");
        return admin_set_admin(a, actor, strtoll(id, NULL, 10), !strcmp(on, "on"), out, cap);
    }
    if (!strcmp(cmd, "announce")) {
        char word[32];
        int64_t col = peek_word(p, word, sizeof word) ? admin_parse_color(word) : -1;
        if (col >= 0) next_word(&p); else col = 0xFFD84A;   /* default: yellow */
        return admin_announce(a, actor, rest(&p), (uint32_t)col, out, cap);
    }
    if (!strcmp(cmd, "setrate")) {
        char *n = next_word(&p), *v = next_word(&p), *end = NULL;
        const double x = v ? strtod(v, &end) : 0;
        if (!n || !v || !end || *end) return say(out, cap, -1, "usage: /setrate <name> <value>");
        return admin_set_rate(a, actor, n, x, out, cap);
    }
    if (!strcmp(cmd, "rates")) {
        const loot_rates r = srv_rates(a->s);
        return say(out, cap, 0, "exp_rate %.2f\ndrop_rate %.2f\nrare_drop_rate %.2f\nmeseta_rate %.2f\ntype_points_rate %.2f",
                   r.exp, r.drop, r.rare, r.meseta, r.type_points);
    }
    if (!strcmp(cmd, "reload")) {
        char *w = next_word(&p);
        return admin_reload(a, actor, w ? w : "all", out, cap);
    }
    if (!strcmp(cmd, "event")) {
        char *n = next_word(&p), *on = next_word(&p);
        if (!n || !on || (strcmp(on, "on") && strcmp(on, "off"))) return say(out, cap, -1, "usage: /event <name> on|off");
        return admin_set_event(a, actor, n, !strcmp(on, "on"), out, cap);
    }
    if (!strcmp(cmd, "events")) {
        size_t o = 0;
        out[0] = 0;
        for (int i = 0; srv_event_name(i) && o < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "%s%s %s", i ? "\n" : "", srv_event_name(i), (srv_events(a->s) >> i) & 1 ? "on" : "off");
        return 0;
    }
    if (!strcmp(cmd, "audit")) {
        db *d = srv_db(a->s);
        if (!d) return say(out, cap, -1, "the audit log needs the database");
        const char *q = rest(&p);
        const char *params[1] = { q };
        char *txt = db_json(d, "SELECT group_concat(line, char(10)) FROM (SELECT datetime(timestamp, 'unixepoch') || '  ' || admin_user || '  ' || "
                               "action_type || coalesce('  ' || target, '') || coalesce('  ' || details_json, '') AS line FROM audit_logs "
                               "WHERE ?1 = '' OR (admin_user || ' ' || action_type || ' ' || coalesce(target, '') || ' ' || coalesce(details_json, '')) "
                               "LIKE '%' || ?1 || '%' ORDER BY log_id DESC LIMIT 15)", 1, params);
        say(out, cap, 0, "%s", txt && strcmp(txt, "null") ? txt : "(no entries)");
        free(txt);
        return 0;
    }
    if (!strcmp(cmd, "shutdown")) {
        if (!a->quit) return say(out, cap, -1, "not from here");
        audit(a, actor, "shutdown", NULL, NULL, NULL, NULL, NULL);
        *a->quit = 1;
        return say(out, cap, 0, "shutting down");
    }
    return say(out, cap, -1, "unknown command '/%s' (try /help)", cmd);
}
