/* Task 2.2: the SQLite persistence layer.
 *
 *  - crypto: SHA-256 and PBKDF2-HMAC-SHA256 against known vectors; password
 *    hashes verify, wrong passwords do not, every hash is salted;
 *  - open: WAL mode, JSON functions, schema version, foreign keys;
 *  - accounts: create, duplicate (case-insensitive), login, wrong password,
 *    ban (audited) / expired ban / unban, last_ip;
 *  - characters and nested inventory: grant items (server uids), mutate them
 *    with json_set (nested paths), reload and read them back;
 *  - trades: item + Meseta move together; not enough Meseta moves nothing;
 *  - interrupts: a child process runs a trade and is SIGKILLed at each point
 *    inside it (item removed / item added / Meseta moved / after commit);
 *    after reopening, the item exists exactly once and Meseta is conserved;
 *  - drops: the first claim wins; a claim killed mid-way leaves the drop
 *    unclaimed and no item;
 *  - stress: 300 random trades and claims in children killed at random
 *    points; every issued uid exists exactly once, total Meseta unchanged,
 *    integrity_check ok;
 *  - WAL: a reader is not blocked by a writer's open transaction and sees
 *    the last committed state. */
#include "crypto.h"
#include "db.h"
#include "sqlite3.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void hexs(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", b[i]); }

static void quiet(const char *l) { (void)l; }

static char g_path[512];

static int64_t q_int(db *d, const char *sql) {
    sqlite3_stmt *st;
    int64_t v = -999;
    if (sqlite3_prepare_v2(db_handle(d), sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}
static void q_text(db *d, const char *sql, char *out, size_t cap) {
    sqlite3_stmt *st;
    out[0] = 0;
    if (sqlite3_prepare_v2(db_handle(d), sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        snprintf(out, cap, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

static db *reopen(void) {
    char err[256];
    db *d = db_open(g_path, quiet, err, sizeof err);
    if (!d) fprintf(stderr, "reopen: %s\n", err);
    return d;
}

/* every uid in the ledger is in exactly one inventory; no uid outside it */
static int uids_conserved(db *d) {
    return q_int(d, "SELECT count(*) FROM item_ledger l WHERE (SELECT count(*) FROM characters c, json_each(c.inventory_json) j "
                    "WHERE json_extract(j.value,'$.uid') = l.uid) != 1") == 0 &&
           q_int(d, "SELECT count(*) FROM characters c, json_each(c.inventory_json) j "
                    "WHERE json_extract(j.value,'$.uid') NOT IN (SELECT uid FROM item_ledger)") == 0;
}

/* ---- crash points ---- */
static const char *g_kill_at;
static void kill_hook(const char *point, void *u) {
    (void)u;
    if (g_kill_at && !strcmp(point, g_kill_at)) raise(SIGKILL);   /* no cleanup, no rollback: a crash */
}

/* Run fn in a child that dies at `point`. Returns 1 if it died by SIGKILL. */
static int crash_child(const char *point, void (*fn)(db *, void *), void *arg) {
    fflush(NULL);
    const pid_t pid = fork();
    if (pid == 0) {
        db *d = reopen();
        if (!d) _exit(3);
        g_kill_at = point;
        db_set_fault_hook(d, kill_hook, NULL);
        fn(d, arg);
        _exit(0);                                    /* the point was not reached */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

typedef struct { int64_t from, to, uid, meseta; } trade_args;
static void do_trade(db *d, void *a) { trade_args *t = a; db_trade(d, t->from, t->to, t->uid, t->meseta); }
typedef struct { int64_t drop, ch; } claim_args;
static void do_claim(db *d, void *a) { claim_args *c = a; db_drop_claim(d, c->drop, c->ch); }

int main(void) {
    /* ---- crypto ---- */
    uint8_t h[64];
    char hx[129];
    sha256("abc", 3, h);
    hexs(h, 32, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    pbkdf2_sha256("passwd", 6, (const uint8_t *)"salt", 4, 1, h, 64);                /* RFC 7914 */
    hexs(h, 64, hx);
    CHECK(!strcmp(hx, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                      "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    pbkdf2_sha256("Password", 8, (const uint8_t *)"NaCl", 4, 80000, h, 64);
    hexs(h, 64, hx);
    CHECK(!strcmp(hx, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                      "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"));
    char p1[PW_HASH_MAX], p2[PW_HASH_MAX];
    CHECK(password_hash("hunter2", p1) == 0 && password_hash("hunter2", p2) == 0 && strcmp(p1, p2) != 0);   /* salted */
    CHECK(password_verify("hunter2", p1) && !password_verify("hunter3", p1) && !password_verify("hunter2", "garbage"));

    /* ---- open ---- */
    snprintf(g_path, sizeof g_path, "%s/psp2i_test_%d.db", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    char side[600];
    remove(g_path);
    snprintf(side, sizeof side, "%s-wal", g_path); remove(side);
    snprintf(side, sizeof side, "%s-shm", g_path); remove(side);
    char err[256], txt[512];
    db *d = db_open(g_path, quiet, err, sizeof err);
    CHECK(d != NULL);
    if (!d) { fprintf(stderr, "%s\n", err); return 1; }
    q_text(d, "PRAGMA journal_mode", txt, sizeof txt);
    CHECK(!strcmp(txt, "wal"));
    CHECK(q_int(d, "PRAGMA user_version") == DB_SCHEMA_VERSION);
    CHECK(q_int(d, "PRAGMA foreign_keys") == 1);
    q_text(d, "SELECT json_set('{\"a\":{\"b\":1}}', '$.a.b', 2)", txt, sizeof txt);
    CHECK(!strcmp(txt, "{\"a\":{\"b\":2}}"));

    /* ---- accounts ---- */
    const int64_t ann = db_account_create(d, "Ann", "pw-ann", 0), bob = db_account_create(d, "bob", "pw-bob", 1);
    CHECK(ann > 0 && bob > 0);
    CHECK(db_account_create(d, "ANN", "x", 0) == DB_EXISTS);
    CHECK(db_account_login(d, "ann", "pw-ann", "10.0.0.5", NULL, 0) == ann);
    q_text(d, "SELECT last_ip FROM accounts WHERE username = 'Ann'", txt, sizeof txt);
    CHECK(!strcmp(txt, "10.0.0.5"));
    CHECK(db_account_login(d, "ann", "wrong", "x", NULL, 0) == DB_BAD_CREDS);
    CHECK(db_account_login(d, "nobody", "pw", "x", NULL, 0) == DB_BAD_CREDS);
    CHECK(db_account_ban(d, ann, "spamming", 0, "bob") == DB_OK);
    char why[64] = "";
    CHECK(db_account_login(d, "ann", "pw-ann", "x", why, sizeof why) == DB_BANNED && !strcmp(why, "spamming"));
    CHECK(db_account_ban(d, ann, "brief", (int64_t)time(NULL) - 1, "bob") == DB_OK);       /* already expired */
    CHECK(db_account_login(d, "ann", "pw-ann", "x", NULL, 0) == ann);
    CHECK(q_int(d, "SELECT is_banned FROM accounts WHERE account_id = 1") == 0);
    CHECK(db_account_ban(d, 999, "x", 0, "bob") == DB_NOT_FOUND);
    CHECK(q_int(d, "SELECT count(*) FROM audit_logs WHERE action_type = 'ban' AND admin_user = 'bob' AND target = 'account:1' "
                   "AND json_extract(details_json, '$.reason') = 'spamming'") == 1);
    CHECK(q_int(d, "SELECT count(*) FROM audit_logs WHERE action_type = 'unban'") == 1);         /* the expiry */
    CHECK(db_audit(d, "bob", "setrate", "rare_drop_rate", "{\"from\":1.0,\"to\":3.0}") == DB_OK);
    CHECK(db_audit(d, "bob", "x", NULL, "{not json") != DB_OK);

    /* ---- characters, nested inventory ---- */
    const int64_t a = db_character_create(d, ann, "Goth Baddie", 3, 1, 2, 5000, "{\"hair\":12,\"colors\":{\"skin\":3}}");
    const int64_t b = db_character_create(d, bob, "Bob", 10, 0, 0, 100, NULL);
    CHECK(a > 0 && b > 0);
    CHECK(db_character_create(d, 999, "Ghost", 1, 0, 0, 0, NULL) < 0);              /* no such account */
    CHECK(db_character_create(d, ann, "Bad", 1, 0, 0, 0, "{oops") < 0);
    CHECK(db_character_create(d, ann, "Neg", 1, 0, 0, -1, NULL) < 0);               /* meseta >= 0 */
    const int64_t sword = db_item_grant(d, a, "{\"id\":4097,\"name\":\"Saber\",\"grind\":0}", "test");
    const int64_t mate = db_item_grant(d, a, "{\"id\":8193,\"qty\":5}", "test");
    const int64_t gun = db_item_grant(d, b, "{\"id\":12289,\"name\":\"Handgun\"}", "test");
    CHECK(sword > 0 && mate > 0 && gun > 0 && sword != mate);
    CHECK(db_item_grant(d, a, "[1,2]", "test") == DB_BAD_JSON);
    CHECK(db_item_grant(d, 999, "{\"id\":1}", "test") == DB_NOT_FOUND);
    CHECK(q_int(d, "SELECT count(*) FROM item_ledger") == 3);                       /* the failed grant left no uid */
    CHECK(db_item_set(d, a, sword, "$.grind", "5") == DB_OK);
    CHECK(db_item_set(d, a, sword, "$.elements", "{\"fire\":30}") == DB_OK);
    CHECK(db_item_set(d, a, sword, "$.elements.fire", "45") == DB_OK);
    CHECK(db_item_set(d, a, mate, "$.qty", "4") == DB_OK);
    CHECK(db_item_set(d, a, sword, "$.uid", "77") != DB_OK);                        /* the uid is not editable */
    CHECK(db_item_set(d, a, gun, "$.grind", "1") == DB_NOT_FOUND);                  /* not Ann's */
    db_character c;
    CHECK(db_character_load(d, a, &c) == DB_OK && c.meseta == 5000 && c.level == 3 && !strcmp(c.name, "Goth Baddie"));
    CHECK(strstr(c.inventory_json, "\"grind\":5") && strstr(c.inventory_json, "\"elements\":{\"fire\":45}") && strstr(c.inventory_json, "\"qty\":4"));
    CHECK(strstr(c.appearance_json, "\"skin\":3") != NULL);
    db_character_free(&c);
    CHECK(q_int(d, "SELECT json_extract(inventory_json, '$[0].elements.fire') FROM characters WHERE char_id = 1") == 45);

    /* ---- trades ---- */
    CHECK(db_trade(d, a, b, sword, 1000) == DB_OK);
    CHECK(db_item_copies(d, sword) == 1);
    CHECK(q_int(d, "SELECT meseta FROM characters WHERE char_id = 1") == 4000 && q_int(d, "SELECT meseta FROM characters WHERE char_id = 2") == 1100);
    CHECK(q_int(d, "SELECT json_extract(inventory_json, '$[#-1].elements.fire') FROM characters WHERE char_id = 2") == 45);   /* the item, intact */
    CHECK(db_trade(d, b, a, gun, 999999) == DB_FUNDS);                              /* rolls back the item too */
    CHECK(db_item_copies(d, gun) == 1 && q_int(d, "SELECT json_array_length(inventory_json) FROM characters WHERE char_id = 2") == 2);
    CHECK(db_trade(d, a, b, gun, 0) == DB_NOT_FOUND);                               /* not Ann's to give */
    CHECK(db_trade(d, a, a, mate, 0) != DB_OK);
    CHECK(db_meseta_transfer(d, b, a, 100) == DB_OK);
    CHECK(uids_conserved(d));

    /* ---- interrupted trades ---- */
    const int64_t total = q_int(d, "SELECT sum(meseta) FROM characters");
    static const char *POINTS[] = { "trade:item-removed", "trade:item-added", "trade:meseta-moved", "trade:committed" };
    for (int i = 0; i < 4; i++) {
        db_close(d);
        trade_args t = { a, b, mate, 50 };
        CHECK(crash_child(POINTS[i], do_trade, &t));
        d = reopen();
        CHECK(d && db_item_copies(d, mate) == 1);
        CHECK(q_int(d, "SELECT sum(meseta) FROM characters") == total);
        const int committed = i == 3;
        char sql[200];
        snprintf(sql, sizeof sql, "SELECT count(*) FROM characters c, json_each(c.inventory_json) j "
                                  "WHERE c.char_id = %lld AND json_extract(j.value,'$.uid') = %lld", (long long)(committed ? b : a), (long long)mate);
        CHECK(q_int(d, sql) == 1);                                                  /* before: Ann's; after commit: Bob's */
        CHECK(uids_conserved(d));
        if (committed) CHECK(db_trade(d, b, a, mate, 50) == DB_OK);                 /* back, for the next round */
    }

    /* ---- drops ---- */
    const int64_t drop = db_drop_create(d, "{\"id\":20481,\"name\":\"Photon Drop\"}", "enemy:Booma");
    CHECK(drop > 0);
    db_close(d);
    claim_args ca = { drop, a };
    CHECK(crash_child("drop:claimed", do_claim, &ca));
    d = reopen();
    CHECK(q_int(d, "SELECT claimed_by IS NULL FROM drops WHERE drop_id = 1") == 1 && uids_conserved(d));
    const int64_t got = db_drop_claim(d, drop, b);
    CHECK(got > 0 && db_item_copies(d, got) == 1);
    CHECK(db_drop_claim(d, drop, a) == DB_CLAIMED && db_drop_claim(d, 999, a) == DB_NOT_FOUND);

    /* ---- stress: random operations, random crash points ---- */
    static const char *ANY[] = { "trade:item-removed", "trade:item-added", "trade:meseta-moved", "trade:committed",
                                 "drop:claimed", "drop:committed", "none" };
    srand(12345);
    const int64_t cs[3] = { a, b, db_character_create(d, bob, "Cat", 50, 2, 1, 777, NULL) };
    for (int i = 0; i < 6; i++) db_item_grant(d, cs[i % 3], "{\"id\":1,\"qty\":1}", "stress");
    const int64_t mtotal = q_int(d, "SELECT sum(meseta) FROM characters");
    int crashed = 0;
    for (int round = 0; round < 300; round++) {
        const int op = rand() % 3;
        const char *pt = ANY[rand() % 7];
        if (op == 2) {
            const int64_t dr = db_drop_create(d, "{\"id\":2,\"qty\":1}", "stress");
            db_close(d);
            claim_args cl = { dr, cs[rand() % 3] };
            crashed += crash_child(pt, do_claim, &cl);
        } else {
            const int f = rand() % 3, t = (f + 1 + rand() % 2) % 3;
            char sql[160];
            snprintf(sql, sizeof sql, "SELECT coalesce((SELECT json_extract(value,'$.uid') FROM characters, json_each(inventory_json) "
                                      "WHERE char_id = %lld ORDER BY random() LIMIT 1), 0)", (long long)cs[f]);
            trade_args tr = { cs[f], cs[t], q_int(d, sql), rand() % 3 ? rand() % 200 : 0 };
            if (!tr.uid && !tr.meseta) tr.meseta = 1;
            db_close(d);
            crashed += crash_child(pt, do_trade, &tr);
        }
        d = reopen();
        if (!d) break;
    }
    CHECK(d && crashed > 50);
    CHECK(uids_conserved(d));
    CHECK(q_int(d, "SELECT sum(meseta) FROM characters") == mtotal);
    CHECK(q_int(d, "SELECT count(*) FROM drops WHERE (claimed_by IS NULL) != (item_uid IS NULL)") == 0);
    q_text(d, "PRAGMA integrity_check", txt, sizeof txt);
    CHECK(!strcmp(txt, "ok"));
    printf("db: stress: 300 rounds, %d killed mid-transaction, %lld items, all accounted for\n",
           crashed, (long long)q_int(d, "SELECT count(*) FROM item_ledger"));

    /* ---- WAL: a reader during a write ---- */
    db *r = reopen();
    CHECK(sqlite3_exec(db_handle(d), "BEGIN IMMEDIATE; UPDATE characters SET meseta = meseta + 1 WHERE char_id = 1;", NULL, NULL, NULL) == SQLITE_OK);
    const int64_t before = q_int(r, "SELECT meseta FROM characters WHERE char_id = 1");   /* not blocked */
    CHECK(before >= 0);
    CHECK(sqlite3_exec(db_handle(d), "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(q_int(r, "SELECT meseta FROM characters WHERE char_id = 1") == before + 1);
    db_close(r);

    db_close(d);
    remove(g_path);
    snprintf(side, sizeof side, "%s-wal", g_path); remove(side);
    snprintf(side, sizeof side, "%s-shm", g_path); remove(side);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("db: ok");
    return 0;
}
