/* Server-side loot: drop tables (JSON), rate multipliers (TOML) and the RNG.
 *
 * drop_tables.json:
 *   { "version": 1,
 *     "enemies": [ { "id": 1, "name": "Booma", "exp": 12, "type_points": 2,
 *                    "drop_chance": 0.30, "meseta": [5, 20],
 *                    "items": [ { "id": 4097, "name": "Saber", "weight": 60 },
 *                               { "meseta": true, "weight": 40 } ],
 *                    "rares": [ { "id": 4200, "name": "...", "chance": 0.002 } ] } ],
 *     "missions": [ { "id": 1, "name": "...", "meseta": [100, 300],
 *                     "rewards": [ { "id": 8193, "name": "Monomate", "weight": 70 } ] } ] }
 *
 * An enemy kill: experience and type points (x exp_rate, type_points_rate);
 * then one rare roll, with probability min(1, sum of rare chances x
 * rare_drop_rate), picking a rare by its share of the chances; if no rare,
 * one normal roll with probability min(1, drop_chance x drop_rate), picking
 * an entry by weight -- an item, or Meseta (uniform in [min, max] x
 * meseta_rate). At most one drop per kill.
 *
 * The JSON is parsed with SQLite's JSON functions (an in-memory connection):
 * no other parser. */
#ifndef PSP2I_SERVER_LOOT_H
#define PSP2I_SERVER_LOOT_H

#include <stddef.h>
#include <stdint.h>

#define LOOT_NAME_LEN 48

typedef struct { double exp, drop, rare, meseta, type_points; } loot_rates;
#define LOOT_RATES_DEFAULT { 1.0, 1.0, 1.0, 1.0, 1.0 }

typedef struct { uint32_t item_id; char name[LOOT_NAME_LEN]; double weight; int is_meseta; } loot_entry;
typedef struct { uint32_t item_id; char name[LOOT_NAME_LEN]; double chance; } loot_rare;

typedef struct {
    uint32_t id;
    char     name[LOOT_NAME_LEN];
    uint32_t exp, type_points;
    double   drop_chance;
    uint32_t meseta_min, meseta_max;
    loot_entry *items; int n_items; double total_weight;
    loot_rare  *rares; int n_rares; double total_rare;
} loot_enemy;

typedef struct {
    uint32_t id;
    char     name[LOOT_NAME_LEN];
    uint32_t meseta_min, meseta_max;
    loot_entry *rewards; int n_rewards; double total_weight;
} loot_mission;

typedef struct loot_table {
    int version;
    loot_enemy   *enemies;  int n_enemies;
    loot_mission *missions; int n_missions;
} loot_table;

typedef struct {
    uint32_t exp, type_points;
    int      has_drop;
    int      rare;
    uint32_t item_id;              /* 0 when the drop is Meseta */
    uint32_t meseta;
    char     name[LOOT_NAME_LEN];
} loot_result;

/* xoshiro256**, seeded with splitmix64. */
typedef struct { uint64_t s[4]; } loot_rng;
void     loot_rng_seed(loot_rng *r, uint64_t seed);
uint64_t loot_rng_next(loot_rng *r);
double   loot_rng_unit(loot_rng *r);                      /* [0, 1) */
uint32_t loot_rng_range(loot_rng *r, uint32_t lo, uint32_t hi);   /* [lo, hi] */

/* Parse; NULL with the reason (and where) in err. */
loot_table *loot_parse(const char *json, char *err, size_t err_cap);
loot_table *loot_load(const char *path, char *err, size_t err_cap);
void        loot_free(loot_table *t);
const loot_enemy   *loot_enemy_find(const loot_table *t, uint32_t id);
const loot_mission *loot_mission_find(const loot_table *t, uint32_t id);

/* 0 on success; -1 if the enemy is not in the table (nothing rolled). */
int loot_roll_enemy(const loot_table *t, uint32_t enemy_id, const loot_rates *r, loot_rng *rng, loot_result *out);
/* A mission clear: one reward by weight (x drop_rate does not apply: one
 * reward always) and Meseta x meseta_rate. */
int loot_roll_mission(const loot_table *t, uint32_t mission_id, const loot_rates *r, loot_rng *rng, loot_result *out);

/* rates.toml: exp_rate, drop_rate, rare_drop_rate, meseta_rate,
 * type_points_rate (each 0..100; missing = 1.0). 0 on success, -2 if the
 * file is missing, -1 on an error (in err). */
int  loot_rates_load(const char *path, loot_rates *out, char *err, size_t err_cap);
/* By name ("exp_rate", ...): 0 on success, -1 unknown name or out of range. */
int  loot_rates_set(loot_rates *r, const char *name, double value);

#endif
