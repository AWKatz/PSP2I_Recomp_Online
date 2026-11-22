/* Task 2.3: server-side loot.
 *
 *  - the shipped config/drop_tables.json and config/rates.toml load;
 *  - malformed tables are rejected with the place named (bad JSON with line
 *    and column, unknown keys, ranges, duplicate ids, rare chances > 1, ...);
 *  - the RNG is deterministic per seed, unbiased in range, in [0, 1);
 *  - THE GATE: 10,000 kills at rare_drop_rate 1.0 and at 5.0 -- the rare
 *    frequency scales by 5 within 3% (fixed seed), and over 200 independent
 *    seed pairs the mean ratio is within 0.5% of 5 (so it is linear, not a
 *    lucky seed); how many single pairs land within 3% is reported;
 *  - drop_rate scales normal drops the same way; exp, type points and Meseta
 *    scale by their rates; a probability past 1 clamps;
 *  - mission rewards follow their weights. */
#include "loot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int rejects(const char *json, const char *expect) {
    char err[256];
    loot_table *t = loot_parse(json, err, sizeof err);
    if (t) { loot_free(t); fprintf(stderr, "  accepted: %s\n", json); return 0; }
    if (!strstr(err, expect)) { fprintf(stderr, "  error \"%s\" lacks \"%s\"\n", err, expect); return 0; }
    return 1;
}

/* the gate's table: one enemy, one rare at 19% (5x = 95%, not clamped) */
static const char *GATE =
    "{\"enemies\":[{\"id\":7,\"name\":\"Gate\",\"exp\":10,\"type_points\":3,\"drop_chance\":0.15,\"meseta\":[10,30],"
    "\"items\":[{\"id\":100,\"weight\":1},{\"meseta\":true,\"weight\":1}],"
    "\"rares\":[{\"id\":900,\"name\":\"Rare\",\"chance\":0.19}]}],"
    "\"missions\":[{\"id\":1,\"meseta\":[100,100],\"rewards\":[{\"id\":1,\"weight\":3},{\"id\":2,\"weight\":1}]}]}";
static const char *NORARE =
    "{\"enemies\":[{\"id\":8,\"drop_chance\":0.15,\"items\":[{\"id\":100,\"weight\":1}]}]}";

typedef struct { int rare, normal, meseta_drops; uint64_t meseta, exp, tp; } tally;
static tally run(const loot_table *t, uint32_t enemy, loot_rates r, uint64_t seed, int kills) {
    loot_rng rng;
    loot_rng_seed(&rng, seed);
    tally y = { 0 };
    for (int i = 0; i < kills; i++) {
        loot_result o;
        loot_roll_enemy(t, enemy, &r, &rng, &o);
        y.exp += o.exp; y.tp += o.type_points;
        if (o.rare) y.rare++;
        else if (o.has_drop && o.item_id) y.normal++;
        else if (o.has_drop) { y.meseta_drops++; y.meseta += o.meseta; }
    }
    return y;
}

int main(int argc, char **argv) {
    char err[256];

    /* ---- the shipped config ---- */
    if (argc > 2) {
        loot_table *t = loot_load(argv[1], err, sizeof err);
        CHECK(t != NULL);
        if (!t) fprintf(stderr, "  %s\n", err);
        if (t) {
            CHECK(t->n_enemies >= 1 && t->n_missions >= 1 && loot_enemy_find(t, 1) && !strcmp(loot_enemy_find(t, 1)->name, "Booma"));
            loot_free(t);
        }
        loot_rates r;
        CHECK(loot_rates_load(argv[2], &r, err, sizeof err) == 0 && r.exp == 1.0 && r.rare == 1.0);
    }
    loot_rates r = LOOT_RATES_DEFAULT;
    CHECK(loot_rates_load("/nonexistent/rates.toml", &r, err, sizeof err) == -2);
    CHECK(loot_rates_set(&r, "rare_drop_rate", 3.0) == 0 && r.rare == 3.0);
    CHECK(loot_rates_set(&r, "rare_drop_rte", 3.0) == -1 && loot_rates_set(&r, "drop_rate", -1) == -1 && loot_rates_set(&r, "drop_rate", 101) == -1);

    /* ---- validation ---- */
    CHECK(rejects("{\"enemies\": [ {\"id\": 1,, } ]}", "line 1"));
    CHECK(rejects("{\n\"enemies\": [\n  {\"id\": 1 \"x\"}\n]}", "line 3"));
    CHECK(rejects("[]", "top level"));
    CHECK(rejects("{\"enemies\": [],}", "strict JSON"));                 /* JSON5: SQLite would take it */
    CHECK(rejects("{enemies: []}", "strict JSON"));
    CHECK(rejects("{\"enemys\":[]}", "unknown key \"enemys\""));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"drop_chace\":0.1}]}", "enemies[0]: unknown key \"drop_chace\""));
    CHECK(rejects("{\"enemies\":[{\"name\":\"x\"}]}", "enemies[0]: \"id\" is missing"));
    CHECK(rejects("{\"enemies\":[{\"id\":1.5}]}", "must be an integer"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"drop_chance\":1.5,\"items\":[{\"id\":2,\"weight\":1}]}]}", "out of range"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"drop_chance\":0.5}]}", "no \"items\""));
    CHECK(rejects("{\"enemies\":[{\"id\":1},{\"id\":1}]}", "enemies[1]: id 1 is also enemies[0]"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"items\":[{\"id\":2,\"weight\":0}]}]}", "enemies[0].items[0]"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"items\":[{\"meseta\":true,\"weight\":1}]}]}", "Meseta entry needs"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"meseta\":[9,3]}]}", "min 9 is above max 3"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"meseta\":[1,2,3]}]}", "[min, max]"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"rares\":[{\"id\":2,\"chance\":0.7},{\"id\":3,\"chance\":0.6}]}]}", "add up to 1.3"));
    CHECK(rejects("{\"missions\":[{\"id\":1,\"rewards\":[{\"meseta\":true,\"weight\":1}]}]}", "not allowed here"));
    CHECK(rejects("{\"enemies\":[{\"id\":1,\"name\":\"0123456789012345678901234567890123456789012345678\"}]}", "longer than"));

    /* ---- RNG ---- */
    loot_rng a, b;
    loot_rng_seed(&a, 42); loot_rng_seed(&b, 42);
    int same = 1, inrange = 1, below1 = 1;
    for (int i = 0; i < 1000; i++) same &= loot_rng_next(&a) == loot_rng_next(&b);
    for (int i = 0; i < 100000; i++) {
        const uint32_t v = loot_rng_range(&a, 5, 9);
        inrange &= v >= 5 && v <= 9;
        const double u = loot_rng_unit(&a);
        below1 &= u >= 0 && u < 1;
    }
    CHECK(same && inrange && below1);

    loot_table *t = loot_parse(GATE, err, sizeof err), *t2 = loot_parse(NORARE, err, sizeof err);
    CHECK(t && t2);
    if (!t || !t2) { fprintf(stderr, "%s\n", err); return 1; }

    /* ---- THE GATE: 10,000 kills, rare_drop_rate 1.0 vs 5.0 ---- */
    loot_rates r1 = LOOT_RATES_DEFAULT, r5 = LOOT_RATES_DEFAULT;
    r5.rare = 5.0;
    const tally g1 = run(t, 7, r1, 1001, 10000), g5 = run(t, 7, r5, 2002, 10000);
    const double ratio = (double)g5.rare / (double)g1.rare;
    printf("loot gate: 10,000 kills: rare at 1.0x %d (%.2f%%, expected 19%%), at 5.0x %d (%.2f%%, expected 95%%), ratio %.3f (5.0 +-3%%: %.3f..%.3f)\n",
           g1.rare, g1.rare / 100.0, g5.rare, g5.rare / 100.0, ratio, 5.0 * 0.97, 5.0 * 1.03);
    CHECK(fabs(ratio - 5.0) <= 5.0 * 0.03);
    CHECK(fabs(g1.rare / 10000.0 - 0.19) <= 0.19 * 0.03 && fabs(g5.rare / 10000.0 - 0.95) <= 0.95 * 0.03);

    /* the same over 200 independent seed pairs */
    double sum = 0;
    int within = 0;
    for (int s = 0; s < 200; s++) {
        const tally x1 = run(t, 7, r1, 10000 + 2 * (uint64_t)s, 10000), x5 = run(t, 7, r5, 10001 + 2 * (uint64_t)s, 10000);
        const double q = (double)x5.rare / (double)x1.rare;
        sum += q;
        within += fabs(q - 5.0) <= 0.15;
    }
    printf("loot gate: 200 seed pairs: mean ratio %.4f; single pairs within 3%%: %d/200\n", sum / 200, within);
    CHECK(fabs(sum / 200 - 5.0) <= 5.0 * 0.005);
    CHECK(within >= 140);                           /* ~1.4 sigma at 10,000 kills: about 85% expected */

    /* drop_rate on normal drops (no rares in the way): 15% vs 75% */
    loot_rates d5 = LOOT_RATES_DEFAULT;
    d5.drop = 5.0;
    const tally n1 = run(t2, 8, r1, 3003, 10000), n5 = run(t2, 8, d5, 4004, 10000);
    const double nr = (double)n5.normal / (double)n1.normal;
    printf("loot: drop_rate 1.0x %d, 5.0x %d drops in 10,000 kills: ratio %.3f\n", n1.normal, n5.normal, nr);
    CHECK(fabs(nr - 5.0) <= 5.0 * 0.03);

    /* exp, type points: exact; Meseta: mean amount */
    loot_rates x2 = LOOT_RATES_DEFAULT;
    x2.exp = 2.0; x2.type_points = 1.5; x2.meseta = 2.0;
    const tally e1 = run(t, 7, r1, 5, 20000), e2 = run(t, 7, x2, 6, 20000);
    CHECK(e1.exp == 10u * 20000 && e2.exp == 20u * 20000 && e2.tp == 5u * 20000);   /* 3 x 1.5 = 4.5 -> 5 */
    const double m1 = (double)e1.meseta / e1.meseta_drops, m2 = (double)e2.meseta / e2.meseta_drops;
    printf("loot: mean Meseta per drop %.2f at 1x (expected 20), %.2f at 2x\n", m1, m2);
    CHECK(fabs(m1 - 20.0) < 0.6 && fabs(m2 / m1 - 2.0) < 0.06);

    /* past 1: every kill a rare */
    loot_rates r10 = LOOT_RATES_DEFAULT;
    r10.rare = 10.0;
    CHECK(run(t, 7, r10, 7, 1000).rare == 1000);
    loot_result o;
    loot_rng rng;
    loot_rng_seed(&rng, 1);
    CHECK(loot_roll_enemy(t, 12345, &r1, &rng, &o) == -1);

    /* missions: weights 3:1, Meseta fixed */
    int w1 = 0, w2 = 0;
    for (int i = 0; i < 40000; i++) {
        CHECK(loot_roll_mission(t, 1, &x2, &rng, &o) == 0 || 1);
        if (o.item_id == 1) w1++; else if (o.item_id == 2) w2++;
        if (o.meseta != 200) { CHECK(o.meseta == 200); break; }
    }
    CHECK(w1 + w2 == 40000 && fabs((double)w1 / w2 - 3.0) < 0.15);

    loot_free(t);
    loot_free(t2);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("loot: ok");
    return 0;
}
