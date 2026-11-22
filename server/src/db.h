/* psp2i_server persistence: embedded SQLite (WAL, JSON).
 *
 * Hybrid schema: strict relational columns for what the server and admins
 * query (accounts, bans, levels, Meseta), JSON text for the game's nested
 * data (appearance, inventory, partner cards).
 *
 * Inventory is a JSON array of item objects. Every item carries a "uid"
 * issued by the server from item_ledger (one row per item ever created), so
 * an item existing twice is detectable, and every move of an item or of
 * Meseta happens inside one BEGIN IMMEDIATE transaction: a crash at any point
 * leaves the database before or after the whole move, never in between. */
#ifndef PSP2I_SERVER_DB_H
#define PSP2I_SERVER_DB_H

#include <stddef.h>
#include <stdint.h>

typedef struct db db;
typedef struct sqlite3 sqlite3;

enum {
    DB_OK        = 0,
    DB_ERR       = -1,    /* SQLite error (logged) */
    DB_NOT_FOUND = -2,
    DB_EXISTS    = -3,    /* unique name taken */
    DB_BANNED    = -4,
    DB_FUNDS     = -5,    /* not enough Meseta */
    DB_CLAIMED   = -6,    /* a drop already claimed */
    DB_BAD_JSON  = -7,
    DB_BAD_CREDS = -8,
};

#define DB_SCHEMA_VERSION 1

/* Open (or create) the database: WAL, foreign keys, busy timeout, schema.
 * NULL on failure, with the reason in err. */
db  *db_open(const char *path, void (*log)(const char *line), char *err, size_t err_cap);
void db_close(db *d);
sqlite3 *db_handle(db *d);                 /* for queries the API does not cover */

/* ---- accounts ---- */
/* > 0: the new account id; DB_EXISTS, DB_ERR. */
int64_t db_account_create(db *d, const char *username, const char *password, int is_admin);
/* > 0: the account id (last_ip recorded); DB_BAD_CREDS, DB_BANNED (*reason
 * filled when given, cap bytes), DB_ERR. An expired ban is lifted. */
int64_t db_account_login(db *d, const char *username, const char *password, const char *ip,
                         char *ban_reason, size_t cap);
/* expires_at: unix seconds, 0 = permanent. Audited as `admin`. */
int db_account_ban(db *d, int64_t account_id, const char *reason, int64_t expires_at, const char *admin);
int db_account_unban(db *d, int64_t account_id, const char *admin);

/* ---- characters ---- */
typedef struct {
    int64_t char_id, account_id;
    char    name[33];
    int     level, race, cls;
    int64_t meseta;
    char   *appearance_json, *inventory_json, *partner_cards_json;   /* malloc'd by db_character_load */
} db_character;

/* > 0: the new character id. JSON fields may be NULL (= {} / [] / []). */
int64_t db_character_create(db *d, int64_t account_id, const char *name, int level, int race, int cls,
                            int64_t meseta, const char *appearance_json);
int  db_character_load(db *d, int64_t char_id, db_character *out);
void db_character_free(db_character *c);

/* ---- items (inventory_json) ---- */
/* Issue a new item (uid from item_ledger) into a character's inventory.
 * item_json: an object, e.g. {"id":1234,"qty":1}; "uid" is set by the server.
 * > 0: the uid. */
int64_t db_item_grant(db *d, int64_t char_id, const char *item_json, const char *origin);
/* json_set on one item, found by uid: path relative to the item ("$.grind",
 * "$.elements.fire"), value as JSON ("5", "\"Fire\"", "{...}"). */
int db_item_set(db *d, int64_t char_id, int64_t uid, const char *path, const char *value_json);
/* How many inventories hold uid right now (1 = fine, 0 = gone, > 1 = duplicated). */
int db_item_copies(db *d, int64_t uid);

/* ---- economy: each one transaction ---- */
/* Move an item (uid, 0 = none) and/or Meseta (>= 0) from one character to
 * another. DB_NOT_FOUND (no such character or item), DB_FUNDS, DB_ERR. */
int db_trade(db *d, int64_t from_char, int64_t to_char, int64_t item_uid, int64_t meseta);
int db_meseta_transfer(db *d, int64_t from_char, int64_t to_char, int64_t amount);
/* A drop waiting on the field. > 0: drop id. */
int64_t db_drop_create(db *d, const char *item_json, const char *origin);
/* The first claim wins: > 0 is the item's uid in the claimer's inventory;
 * DB_CLAIMED for everyone after. */
int64_t db_drop_claim(db *d, int64_t drop_id, int64_t char_id);

/* ---- audit ---- */
int db_audit(db *d, const char *admin_user, const char *action, const char *target, const char *details_json);

/* Tests: called at named points inside the economy transactions
 * ("trade:item-removed", "trade:item-added", "trade:meseta-moved",
 * "trade:committed", "drop:claimed", "drop:committed"). */
void db_set_fault_hook(db *d, void (*fn)(const char *point, void *user), void *user);

#endif
