/* psp2i_server persistence (see db.h). */
#include "db.h"

#include "crypto.h"
#include "sqlite3.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct db {
    sqlite3 *h;
    void (*log)(const char *line);
    void (*fault)(const char *point, void *user);
    void *fault_user;
};

static const char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS accounts ("
    "  account_id     INTEGER PRIMARY KEY,"
    "  username       TEXT NOT NULL UNIQUE COLLATE NOCASE,"
    "  password_hash  TEXT NOT NULL,"
    "  is_admin       INTEGER NOT NULL DEFAULT 0 CHECK (is_admin IN (0, 1)),"
    "  is_banned      INTEGER NOT NULL DEFAULT 0 CHECK (is_banned IN (0, 1)),"
    "  ban_reason     TEXT,"
    "  ban_expires_at INTEGER,"                                   /* unix seconds; NULL/0 = permanent */
    "  last_ip        TEXT,"
    "  created_at     INTEGER NOT NULL DEFAULT (unixepoch())"
    ") STRICT;"
    "CREATE TABLE IF NOT EXISTS characters ("
    "  char_id            INTEGER PRIMARY KEY,"
    "  account_id         INTEGER NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,"
    "  name               TEXT NOT NULL,"
    "  level              INTEGER NOT NULL DEFAULT 1 CHECK (level BETWEEN 1 AND 200),"
    "  race               INTEGER NOT NULL DEFAULT 0,"
    "  class              INTEGER NOT NULL DEFAULT 0,"
    "  meseta             INTEGER NOT NULL DEFAULT 0 CHECK (meseta >= 0),"
    "  appearance_json    TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(appearance_json)),"
    "  inventory_json     TEXT NOT NULL DEFAULT '[]' CHECK (json_type(inventory_json) = 'array'),"
    "  partner_cards_json TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(partner_cards_json))"
    ") STRICT;"
    "CREATE INDEX IF NOT EXISTS characters_by_account ON characters(account_id);"
    "CREATE TABLE IF NOT EXISTS audit_logs ("
    "  log_id       INTEGER PRIMARY KEY,"
    "  timestamp    INTEGER NOT NULL,"
    "  admin_user   TEXT NOT NULL,"
    "  action_type  TEXT NOT NULL,"
    "  target       TEXT,"
    "  details_json TEXT CHECK (details_json IS NULL OR json_valid(details_json))"
    ") STRICT;"
    "CREATE INDEX IF NOT EXISTS audit_by_time ON audit_logs(timestamp);"
    /* one row per item ever issued: its uid */
    "CREATE TABLE IF NOT EXISTS item_ledger ("
    "  uid        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  origin     TEXT"
    ") STRICT;"
    "CREATE TABLE IF NOT EXISTS drops ("
    "  drop_id    INTEGER PRIMARY KEY,"
    "  item_json  TEXT NOT NULL CHECK (json_type(item_json) = 'object'),"
    "  origin     TEXT,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  claimed_by INTEGER REFERENCES characters(char_id),"
    "  claimed_at INTEGER,"
    "  item_uid   INTEGER REFERENCES item_ledger(uid)"
    ") STRICT;"
    /* v2 */
    "CREATE TABLE IF NOT EXISTS ip_bans ("
    "  ip         TEXT PRIMARY KEY,"
    "  reason     TEXT,"
    "  expires_at INTEGER,"                                   /* unix seconds; NULL = permanent */
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  admin_user TEXT"
    ") STRICT;";

static void dlog(db *d, const char *fmt, ...) {
    if (!d->log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    d->log(line);
}

static char *dup_col(sqlite3_stmt *st, int i);

static void fault(db *d, const char *point) { if (d->fault) d->fault(point, d->fault_user); }

static int exec(db *d, const char *sql) {
    char *msg = NULL;
    if (sqlite3_exec(d->h, sql, NULL, NULL, &msg) == SQLITE_OK) return DB_OK;
    dlog(d, "db: %s", msg ? msg : sqlite3_errmsg(d->h));
    sqlite3_free(msg);
    return DB_ERR;
}

static sqlite3_stmt *prep(db *d, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->h, sql, -1, &st, NULL) != SQLITE_OK) {
        dlog(d, "db: prepare: %s", sqlite3_errmsg(d->h));
        return NULL;
    }
    return st;
}

/* Run a statement that returns no rows. DB_OK or DB_ERR. */
static int step_done(db *d, sqlite3_stmt *st) {
    const int rc = st ? sqlite3_step(st) : SQLITE_ERROR;
    if (st && rc != SQLITE_DONE) dlog(d, "db: %s", sqlite3_errmsg(d->h));
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? DB_OK : DB_ERR;
}

static int begin(db *d) { return exec(d, "BEGIN IMMEDIATE"); }
static int commit(db *d) { return exec(d, "COMMIT"); }
static void rollback(db *d) { if (!sqlite3_get_autocommit(d->h)) exec(d, "ROLLBACK"); }

/* ---- open ---- */

db *db_open(const char *path, void (*log)(const char *line), char *err, size_t err_cap) {
    db *d = calloc(1, sizeof *d);
    if (!d) { snprintf(err, err_cap, "out of memory"); return NULL; }
    d->log = log;
    if (sqlite3_open_v2(path, &d->h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        snprintf(err, err_cap, "%s: %s", path, d->h ? sqlite3_errmsg(d->h) : "cannot open");
        db_close(d);
        return NULL;
    }
    sqlite3_busy_timeout(d->h, 5000);
    /* WAL: the game loop writes while the admin dashboard reads. synchronous
     * FULL: a committed trade survives a power cut, not only a crash. */
    sqlite3_stmt *st = prep(d, "PRAGMA journal_mode=WAL");
    const char *mode = st && sqlite3_step(st) == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    const int wal = mode && !strcmp(mode, "wal");
    sqlite3_finalize(st);
    if (!wal) { snprintf(err, err_cap, "%s: cannot switch to WAL mode", path); db_close(d); return NULL; }
    if (exec(d, "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;") != DB_OK) {
        snprintf(err, err_cap, "%s: %s", path, sqlite3_errmsg(d->h));
        db_close(d);
        return NULL;
    }
    st = prep(d, "PRAGMA user_version");
    const int ver = st && sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    if (ver > DB_SCHEMA_VERSION) {
        snprintf(err, err_cap, "%s: schema version %d is newer than this server's (%d)", path, ver, DB_SCHEMA_VERSION);
        db_close(d);
        return NULL;
    }
    if (begin(d) != DB_OK || exec(d, SCHEMA) != DB_OK || exec(d, "PRAGMA user_version=2") != DB_OK || commit(d) != DB_OK) {
        snprintf(err, err_cap, "%s: schema: %s", path, sqlite3_errmsg(d->h));
        rollback(d);
        db_close(d);
        return NULL;
    }
    return d;
}

void db_close(db *d) {
    if (!d) return;
    if (d->h) sqlite3_close_v2(d->h);
    free(d);
}

sqlite3 *db_handle(db *d) { return d->h; }

void db_set_fault_hook(db *d, void (*fn)(const char *, void *), void *user) { d->fault = fn; d->fault_user = user; }

/* ---- audit ---- */

static int audit_locked(db *d, const char *admin, const char *action, const char *target, const char *details) {
    sqlite3_stmt *st = prep(d, "INSERT INTO audit_logs (timestamp, admin_user, action_type, target, details_json) "
                               "VALUES (unixepoch(), ?1, ?2, ?3, ?4)");
    if (!st) return DB_ERR;
    sqlite3_bind_text(st, 1, admin ? admin : "server", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, action, -1, SQLITE_TRANSIENT);
    if (target) sqlite3_bind_text(st, 3, target, -1, SQLITE_TRANSIENT);
    if (details) sqlite3_bind_text(st, 4, details, -1, SQLITE_TRANSIENT);
    return step_done(d, st);
}

int db_audit(db *d, const char *admin, const char *action, const char *target, const char *details) {
    return audit_locked(d, admin, action, target, details);
}

/* ---- accounts ---- */

int64_t db_account_create(db *d, const char *username, const char *password, int is_admin) {
    char hash[PW_HASH_MAX];
    if (!username || !*username || !password || password_hash(password, hash) != 0) return DB_ERR;
    sqlite3_stmt *st = prep(d, "INSERT INTO accounts (username, password_hash, is_admin) VALUES (?1, ?2, ?3)");
    if (!st) return DB_ERR;
    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, is_admin ? 1 : 0);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) return DB_EXISTS;
    if (rc != SQLITE_DONE) { dlog(d, "db: %s", sqlite3_errmsg(d->h)); return DB_ERR; }
    return sqlite3_last_insert_rowid(d->h);
}

int64_t db_account_login(db *d, const char *username, const char *password, const char *ip, char *reason, size_t cap) {
    sqlite3_stmt *st = prep(d, "SELECT account_id, password_hash, is_banned, ban_reason, coalesce(ban_expires_at, 0) "
                               "FROM accounts WHERE username = ?1");
    if (!st) return DB_ERR;
    sqlite3_bind_text(st, 1, username ? username : "", -1, SQLITE_TRANSIENT);
    int64_t id = 0, expires = 0;
    int banned = 0;
    char stored[PW_HASH_MAX] = "", why[256] = "";
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        snprintf(stored, sizeof stored, "%s", (const char *)sqlite3_column_text(st, 1));
        banned = sqlite3_column_int(st, 2);
        if (sqlite3_column_text(st, 3)) snprintf(why, sizeof why, "%s", (const char *)sqlite3_column_text(st, 3));
        expires = sqlite3_column_int64(st, 4);
    }
    sqlite3_finalize(st);
    /* the same work whether or not the name exists: no timing hint */
    static const char DUMMY[] = "pbkdf2-sha256$100000$00000000000000000000000000000000$"
                                "0000000000000000000000000000000000000000000000000000000000000000";
    const int ok = password_verify(password ? password : "", id ? stored : DUMMY);
    if (!id || !ok) return DB_BAD_CREDS;
    if (banned) {
        if (expires > 0 && expires <= (int64_t)time(NULL)) {
            db_account_unban(d, id, "server (ban expired)");
        } else {
            if (reason && cap) snprintf(reason, cap, "%s", why);
            return DB_BANNED;
        }
    }
    st = prep(d, "UPDATE accounts SET last_ip = ?1 WHERE account_id = ?2");
    if (st) {
        if (ip) sqlite3_bind_text(st, 1, ip, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        step_done(d, st);
    }
    return id;
}

static int set_ban(db *d, int64_t id, int on, const char *reason, int64_t expires, const char *admin) {
    if (begin(d) != DB_OK) return DB_ERR;
    sqlite3_stmt *st = prep(d, "UPDATE accounts SET is_banned = ?1, ban_reason = ?2, ban_expires_at = ?3 WHERE account_id = ?4");
    if (!st) { rollback(d); return DB_ERR; }
    sqlite3_bind_int(st, 1, on);
    if (on && reason) sqlite3_bind_text(st, 2, reason, -1, SQLITE_TRANSIENT);
    if (on && expires > 0) sqlite3_bind_int64(st, 3, expires);
    sqlite3_bind_int64(st, 4, id);
    if (step_done(d, st) != DB_OK) { rollback(d); return DB_ERR; }
    if (sqlite3_changes(d->h) != 1) { rollback(d); return DB_NOT_FOUND; }
    char target[32], details[512];
    snprintf(target, sizeof target, "account:%lld", (long long)id);
    sqlite3_stmt *js = prep(d, "SELECT json_object('reason', ?1, 'expires_at', ?2)");
    details[0] = 0;
    if (js) {
        if (reason) sqlite3_bind_text(js, 1, reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(js, 2, expires);
        if (sqlite3_step(js) == SQLITE_ROW) snprintf(details, sizeof details, "%s", (const char *)sqlite3_column_text(js, 0));
        sqlite3_finalize(js);
    }
    if (audit_locked(d, admin, on ? "ban" : "unban", target, on && details[0] ? details : NULL) != DB_OK) { rollback(d); return DB_ERR; }
    return commit(d);
}

int db_account_ban(db *d, int64_t id, const char *reason, int64_t expires, const char *admin) { return set_ban(d, id, 1, reason, expires, admin); }
int db_account_unban(db *d, int64_t id, const char *admin) { return set_ban(d, id, 0, NULL, 0, admin); }

int db_account_set_admin(db *d, int64_t id, int on, const char *admin) {
    if (begin(d) != DB_OK) return DB_ERR;
    sqlite3_stmt *st = prep(d, "UPDATE accounts SET is_admin = ?1 WHERE account_id = ?2");
    if (st) { sqlite3_bind_int(st, 1, on ? 1 : 0); sqlite3_bind_int64(st, 2, id); }
    if (step_done(d, st) != DB_OK) { rollback(d); return DB_ERR; }
    if (sqlite3_changes(d->h) != 1) { rollback(d); return DB_NOT_FOUND; }
    char target[32];
    snprintf(target, sizeof target, "account:%lld", (long long)id);
    if (audit_locked(d, admin, on ? "promote" : "demote", target, NULL) != DB_OK) { rollback(d); return DB_ERR; }
    return commit(d);
}

/* ---- IP bans ---- */

int db_ip_ban(db *d, const char *ip, const char *reason, int64_t expires, const char *admin) {
    if (!ip || !*ip) return DB_ERR;
    if (begin(d) != DB_OK) return DB_ERR;
    sqlite3_stmt *st = prep(d, "INSERT INTO ip_bans (ip, reason, expires_at, admin_user) VALUES (?1, ?2, ?3, ?4) "
                               "ON CONFLICT(ip) DO UPDATE SET reason = ?2, expires_at = ?3, admin_user = ?4, created_at = unixepoch()");
    if (st) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_TRANSIENT);
        if (reason) sqlite3_bind_text(st, 2, reason, -1, SQLITE_TRANSIENT);
        if (expires > 0) sqlite3_bind_int64(st, 3, expires);
        sqlite3_bind_text(st, 4, admin ? admin : "server", -1, SQLITE_TRANSIENT);
    }
    if (step_done(d, st) != DB_OK) { rollback(d); return DB_ERR; }
    char target[80], details[400] = "";
    snprintf(target, sizeof target, "ip:%s", ip);
    sqlite3_stmt *js = prep(d, "SELECT json_object('reason', ?1, 'expires_at', ?2)");
    if (js) {
        if (reason) sqlite3_bind_text(js, 1, reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(js, 2, expires);
        if (sqlite3_step(js) == SQLITE_ROW) snprintf(details, sizeof details, "%s", (const char *)sqlite3_column_text(js, 0));
        sqlite3_finalize(js);
    }
    if (audit_locked(d, admin, "ban", target, details[0] ? details : NULL) != DB_OK) { rollback(d); return DB_ERR; }
    return commit(d);
}

int db_ip_unban(db *d, const char *ip, const char *admin) {
    if (begin(d) != DB_OK) return DB_ERR;
    sqlite3_stmt *st = prep(d, "DELETE FROM ip_bans WHERE ip = ?1");
    if (st) sqlite3_bind_text(st, 1, ip ? ip : "", -1, SQLITE_TRANSIENT);
    if (step_done(d, st) != DB_OK) { rollback(d); return DB_ERR; }
    if (sqlite3_changes(d->h) != 1) { rollback(d); return DB_NOT_FOUND; }
    char target[80];
    snprintf(target, sizeof target, "ip:%s", ip);
    if (audit_locked(d, admin, "unban", target, NULL) != DB_OK) { rollback(d); return DB_ERR; }
    return commit(d);
}

int db_ip_banned(db *d, const char *ip, char *reason, size_t cap) {
    sqlite3_stmt *st = prep(d, "SELECT reason, coalesce(expires_at, 0) FROM ip_bans WHERE ip = ?1");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, ip ? ip : "", -1, SQLITE_TRANSIENT);
    int banned = 0;
    int64_t expires = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        banned = 1;
        if (reason && cap) snprintf(reason, cap, "%s", sqlite3_column_text(st, 0) ? (const char *)sqlite3_column_text(st, 0) : "");
        expires = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    if (banned && expires > 0 && expires <= (int64_t)time(NULL)) {
        db_ip_unban(d, ip, "server (ban expired)");
        return 0;
    }
    return banned;
}

char *db_json(db *d, const char *sql, int n, const char *const *params) {
    sqlite3_stmt *st = prep(d, sql);
    if (!st) return NULL;
    for (int i = 0; i < n; i++) if (params[i]) sqlite3_bind_text(st, i + 1, params[i], -1, SQLITE_TRANSIENT);
    char *out = NULL;
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) out = dup_col(st, 0);
    else if (rc == SQLITE_DONE) out = dup_col(st, 0);         /* no row: "" */
    else dlog(d, "db: %s", sqlite3_errmsg(d->h));
    sqlite3_finalize(st);
    if (out && !*out) { free(out); out = malloc(5); if (out) memcpy(out, "null", 5); }
    return out;
}

/* ---- characters ---- */

int64_t db_character_create(db *d, int64_t account, const char *name, int level, int race, int cls,
                            int64_t meseta, const char *appearance) {
    sqlite3_stmt *st = prep(d, "INSERT INTO characters (account_id, name, level, race, class, meseta, appearance_json) "
                               "VALUES (?1, ?2, ?3, ?4, ?5, ?6, coalesce(?7, '{}'))");
    if (!st) return DB_ERR;
    sqlite3_bind_int64(st, 1, account);
    sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, level);
    sqlite3_bind_int(st, 4, race);
    sqlite3_bind_int(st, 5, cls);
    sqlite3_bind_int64(st, 6, meseta);
    if (appearance) sqlite3_bind_text(st, 7, appearance, -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { dlog(d, "db: %s", sqlite3_errmsg(d->h)); return rc == SQLITE_CONSTRAINT ? DB_BAD_JSON : DB_ERR; }
    return sqlite3_last_insert_rowid(d->h);
}

static char *dup_col(sqlite3_stmt *st, int i) {
    const char *s = (const char *)sqlite3_column_text(st, i);
    const size_t n = s ? strlen(s) : 0;
    char *r = malloc(n + 1);
    if (r) { if (n) memcpy(r, s, n); r[n] = 0; }
    return r;
}

int db_character_load(db *d, int64_t id, db_character *c) {
    memset(c, 0, sizeof *c);
    sqlite3_stmt *st = prep(d, "SELECT char_id, account_id, name, level, race, class, meseta, appearance_json, "
                               "inventory_json, partner_cards_json FROM characters WHERE char_id = ?1");
    if (!st) return DB_ERR;
    sqlite3_bind_int64(st, 1, id);
    int rc = DB_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        c->char_id = sqlite3_column_int64(st, 0);
        c->account_id = sqlite3_column_int64(st, 1);
        snprintf(c->name, sizeof c->name, "%s", (const char *)sqlite3_column_text(st, 2));
        c->level = sqlite3_column_int(st, 3);
        c->race = sqlite3_column_int(st, 4);
        c->cls = sqlite3_column_int(st, 5);
        c->meseta = sqlite3_column_int64(st, 6);
        c->appearance_json = dup_col(st, 7);
        c->inventory_json = dup_col(st, 8);
        c->partner_cards_json = dup_col(st, 9);
        rc = DB_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

void db_character_free(db_character *c) {
    free(c->appearance_json);
    free(c->inventory_json);
    free(c->partner_cards_json);
    memset(c, 0, sizeof *c);
}

/* ---- items ---- */

static int char_exists(db *d, int64_t id) {
    sqlite3_stmt *st = prep(d, "SELECT 1 FROM characters WHERE char_id = ?1");
    if (!st) return 0;
    sqlite3_bind_int64(st, 1, id);
    const int ok = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}

/* The array index of item uid in a character's inventory (-1 if absent); its
 * JSON in *json (malloc'd) when asked. */
static int64_t item_index(db *d, int64_t char_id, int64_t uid, char **json) {
    sqlite3_stmt *st = prep(d, "SELECT j.key, j.value FROM characters c, json_each(c.inventory_json) j "
                               "WHERE c.char_id = ?1 AND json_extract(j.value, '$.uid') = ?2");
    if (!st) return -1;
    sqlite3_bind_int64(st, 1, char_id);
    sqlite3_bind_int64(st, 2, uid);
    int64_t idx = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        idx = sqlite3_column_int64(st, 0);
        if (json) *json = dup_col(st, 1);
    }
    sqlite3_finalize(st);
    return idx;
}

static int json_object_ok(db *d, const char *s) {
    sqlite3_stmt *st = prep(d, "SELECT json_valid(?1) AND json_type(?1) = 'object'");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, s ? s : "", -1, SQLITE_TRANSIENT);
    const int ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return ok;
}

/* Inside a transaction: a new uid, and the item (with it) appended. */
static int64_t grant_locked(db *d, int64_t char_id, const char *item_json, const char *origin) {
    sqlite3_stmt *st = prep(d, "INSERT INTO item_ledger (origin) VALUES (?1)");
    if (!st) return DB_ERR;
    if (origin) sqlite3_bind_text(st, 1, origin, -1, SQLITE_TRANSIENT);
    if (step_done(d, st) != DB_OK) return DB_ERR;
    const int64_t uid = sqlite3_last_insert_rowid(d->h);
    st = prep(d, "UPDATE characters SET inventory_json = json_insert(inventory_json, '$[#]', json_set(json(?1), '$.uid', ?2)) "
                 "WHERE char_id = ?3");
    if (!st) return DB_ERR;
    sqlite3_bind_text(st, 1, item_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, uid);
    sqlite3_bind_int64(st, 3, char_id);
    if (step_done(d, st) != DB_OK) return DB_ERR;
    return sqlite3_changes(d->h) == 1 ? uid : DB_NOT_FOUND;
}

int64_t db_item_grant(db *d, int64_t char_id, const char *item_json, const char *origin) {
    if (!json_object_ok(d, item_json)) return DB_BAD_JSON;
    if (begin(d) != DB_OK) return DB_ERR;
    const int64_t uid = grant_locked(d, char_id, item_json, origin);
    if (uid <= 0 || commit(d) != DB_OK) { rollback(d); return uid <= 0 ? uid : DB_ERR; }
    return uid;
}

int db_item_set(db *d, int64_t char_id, int64_t uid, const char *path, const char *value_json) {
    if (!path || path[0] != '$' || !strcmp(path, "$") || !strncmp(path, "$.uid", 5)) return DB_ERR;   /* the uid is the server's */
    if (begin(d) != DB_OK) return DB_ERR;
    const int64_t idx = item_index(d, char_id, uid, NULL);
    if (idx < 0) { rollback(d); return DB_NOT_FOUND; }
    sqlite3_stmt *st = prep(d, "UPDATE characters SET inventory_json = json_set(inventory_json, "
                               "'$[' || ?1 || ']' || substr(?2, 2), json(?3)) WHERE char_id = ?4");
    if (!st) { rollback(d); return DB_ERR; }
    sqlite3_bind_int64(st, 1, idx);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, value_json ? value_json : "null", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, char_id);
    if (step_done(d, st) != DB_OK) { rollback(d); return DB_BAD_JSON; }
    return commit(d);
}

int db_item_copies(db *d, int64_t uid) {
    sqlite3_stmt *st = prep(d, "SELECT count(*) FROM characters c, json_each(c.inventory_json) j "
                               "WHERE json_extract(j.value, '$.uid') = ?1");
    if (!st) return DB_ERR;
    sqlite3_bind_int64(st, 1, uid);
    const int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : DB_ERR;
    sqlite3_finalize(st);
    return n;
}

/* ---- economy ---- */

int db_trade(db *d, int64_t from, int64_t to, int64_t uid, int64_t meseta) {
    if (from == to || meseta < 0 || (!uid && !meseta)) return DB_ERR;
    if (begin(d) != DB_OK) return DB_ERR;
    int rc = DB_OK;
    if (!char_exists(d, from) || !char_exists(d, to)) { rc = DB_NOT_FOUND; goto fail; }
    if (uid) {
        char *item = NULL;
        const int64_t idx = item_index(d, from, uid, &item);
        if (idx < 0) { rc = DB_NOT_FOUND; goto fail; }
        sqlite3_stmt *st = prep(d, "UPDATE characters SET inventory_json = json_remove(inventory_json, '$[' || ?1 || ']') "
                                   "WHERE char_id = ?2");
        if (st) { sqlite3_bind_int64(st, 1, idx); sqlite3_bind_int64(st, 2, from); }
        if (step_done(d, st) != DB_OK) { free(item); rc = DB_ERR; goto fail; }
        fault(d, "trade:item-removed");
        st = prep(d, "UPDATE characters SET inventory_json = json_insert(inventory_json, '$[#]', json(?1)) WHERE char_id = ?2");
        if (st) { sqlite3_bind_text(st, 1, item, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(st, 2, to); }
        free(item);
        if (step_done(d, st) != DB_OK) { rc = DB_ERR; goto fail; }
        fault(d, "trade:item-added");
    }
    if (meseta) {
        sqlite3_stmt *st = prep(d, "UPDATE characters SET meseta = meseta - ?1 WHERE char_id = ?2 AND meseta >= ?1");
        if (st) { sqlite3_bind_int64(st, 1, meseta); sqlite3_bind_int64(st, 2, from); }
        if (step_done(d, st) != DB_OK) { rc = DB_ERR; goto fail; }
        if (sqlite3_changes(d->h) != 1) { rc = DB_FUNDS; goto fail; }
        st = prep(d, "UPDATE characters SET meseta = meseta + ?1 WHERE char_id = ?2");
        if (st) { sqlite3_bind_int64(st, 1, meseta); sqlite3_bind_int64(st, 2, to); }
        if (step_done(d, st) != DB_OK) { rc = DB_ERR; goto fail; }
        fault(d, "trade:meseta-moved");
    }
    if (commit(d) != DB_OK) { rc = DB_ERR; goto fail; }
    fault(d, "trade:committed");
    return DB_OK;
fail:
    rollback(d);
    return rc;
}

int db_meseta_transfer(db *d, int64_t from, int64_t to, int64_t amount) {
    return amount > 0 ? db_trade(d, from, to, 0, amount) : DB_ERR;
}

int64_t db_drop_create(db *d, const char *item_json, const char *origin) {
    if (!json_object_ok(d, item_json)) return DB_BAD_JSON;
    sqlite3_stmt *st = prep(d, "INSERT INTO drops (item_json, origin) VALUES (json(?1), ?2)");
    if (!st) return DB_ERR;
    sqlite3_bind_text(st, 1, item_json, -1, SQLITE_TRANSIENT);
    if (origin) sqlite3_bind_text(st, 2, origin, -1, SQLITE_TRANSIENT);
    if (step_done(d, st) != DB_OK) return DB_ERR;
    return sqlite3_last_insert_rowid(d->h);
}

int64_t db_drop_claim(db *d, int64_t drop_id, int64_t char_id) {
    if (begin(d) != DB_OK) return DB_ERR;
    int64_t rc;
    sqlite3_stmt *st = prep(d, "UPDATE drops SET claimed_by = ?1, claimed_at = unixepoch() "
                               "WHERE drop_id = ?2 AND claimed_by IS NULL RETURNING item_json, origin");
    if (!st) { rollback(d); return DB_ERR; }
    sqlite3_bind_int64(st, 1, char_id);
    sqlite3_bind_int64(st, 2, drop_id);
    char *item = NULL, *origin = NULL;
    const int step = sqlite3_step(st);
    if (step == SQLITE_ROW) { item = dup_col(st, 0); origin = dup_col(st, 1); }
    else if (step != SQLITE_DONE) dlog(d, "db: %s", sqlite3_errmsg(d->h));
    sqlite3_finalize(st);
    if (!item) {
        rollback(d);
        if (step != SQLITE_DONE && step != SQLITE_ROW) return DB_ERR;
        st = prep(d, "SELECT 1 FROM drops WHERE drop_id = ?1");
        if (!st) return DB_ERR;
        sqlite3_bind_int64(st, 1, drop_id);
        rc = sqlite3_step(st) == SQLITE_ROW ? DB_CLAIMED : DB_NOT_FOUND;
        sqlite3_finalize(st);
        free(origin);
        return rc;
    }
    fault(d, "drop:claimed");
    rc = grant_locked(d, char_id, item, origin);
    free(item);
    free(origin);
    if (rc > 0) {
        st = prep(d, "UPDATE drops SET item_uid = ?1 WHERE drop_id = ?2");
        if (st) { sqlite3_bind_int64(st, 1, rc); sqlite3_bind_int64(st, 2, drop_id); }
        if (step_done(d, st) != DB_OK) rc = DB_ERR;
    }
    if (rc <= 0 || commit(d) != DB_OK) { rollback(d); return rc <= 0 ? rc : DB_ERR; }
    fault(d, "drop:committed");
    return rc;
}
