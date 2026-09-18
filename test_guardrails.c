/* Feeds the guardrail layer the output a misbehaving model actually produces,
   then runs the full guardrail -> optimizer -> self-validate path. */
#include "../src/guardrails.h"
#include "../src/optimizer.h"
#include "../vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
static void ck(int cond, const char *what)
{
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

static gw_battery BAT = { 200, 120, 40, 50, 50 };

static void guard_str(const char *json, int n_notes, gw_interp *out)
{
    cJSON *p = cJSON_Parse(json);
    gw_guard(p, n_notes, &BAT, out);
    if (p) cJSON_Delete(p);
}

static int hours_eq(const gw_interp *e, const int *want, int n)
{
    int k = 0;
    for (int h = 0; h < GW_H; h++) {
        if (!e->dir.hours[h]) continue;
        if (k >= n || want[k] != h) return 0;
        k++;
    }
    return k == n;
}

int main(void)
{
    gw_interp in[GW_MAX_NOTES];

    puts("hours normalisation (unsorted, duplicated, out of range, fractional)");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"no_charge_window\","
              "\"structured_adjustment\":{\"hours\":[14,2,2,99,-3,13.5,3]},\"explanation\":\"x\"}]",
              1, in);
    { int want[] = {2,3,14};
      ck(in[0].type == GW_NO_CHARGE_WINDOW, "type preserved");
      ck(hours_eq(&in[0], want, 3), "hours deduped, sorted, filtered to 0-23, 13.5 dropped"); }

    puts("factor clamped into [0,1]");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"solar_reduction\","
              "\"structured_adjustment\":{\"hours\":[12],\"factor\":1.7},\"explanation\":\"x\"}]", 1, in);
    ck(fabs(in[0].dir.factor - 1.0) < 1e-9, "factor 1.7 -> 1.0");

    puts("reserve above capacity clamped");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"minimum_battery_reserve\","
              "\"structured_adjustment\":{\"hours\":[18],\"minimum_energy_kwh\":9999},\"explanation\":\"x\"}]", 1, in);
    ck(fabs(in[0].dir.minimum_energy_kwh - BAT.capacity_kwh) < 1e-9, "reserve clamped to capacity");

    puts("invented directive type demoted, not crashed");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"turn_off_campus\","
              "\"structured_adjustment\":{\"hours\":[1]},\"explanation\":\"x\"}]", 1, in);
    ck(in[0].type == GW_NO_OP && in[0].applies == 0, "unknown type -> no_op, applies false");

    puts("missing structured_adjustment demoted");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"max_grid_window\","
              "\"explanation\":\"x\"}]", 1, in);
    ck(in[0].type == GW_NO_OP, "missing adjustment -> no_op");

    puts("negative grid cap demoted");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"max_grid_window\","
              "\"structured_adjustment\":{\"hours\":[19],\"max_grid_kwh\":-5},\"explanation\":\"x\"}]", 1, in);
    ck(in[0].type == GW_NO_OP, "negative max_grid_kwh -> no_op");

    puts("model returns 1 entry for 3 notes: missing indices padded");
    guard_str("[{\"note_index\":1,\"applies\":true,\"directive_type\":\"no_discharge_window\","
              "\"structured_adjustment\":{\"hours\":[18]},\"explanation\":\"x\"}]", 3, in);
    ck(in[0].note_index == 0 && in[0].type == GW_NO_OP, "index 0 padded as no_op");
    ck(in[1].type == GW_NO_DISCHARGE_WINDOW && in[1].applies == 1, "index 1 kept, applies true");
    ck(in[2].note_index == 2 && in[2].type == GW_NO_OP, "index 2 padded as no_op");

    puts("duplicate note_index: first wins, no double-apply");
    guard_str("[{\"note_index\":0,\"applies\":true,\"directive_type\":\"no_charge_window\","
              "\"structured_adjustment\":{\"hours\":[5]},\"explanation\":\"a\"},"
              "{\"note_index\":0,\"applies\":true,\"directive_type\":\"no_discharge_window\","
              "\"structured_adjustment\":{\"hours\":[6]},\"explanation\":\"b\"}]", 1, in);
    ck(in[0].type == GW_NO_CHARGE_WINDOW, "duplicate index ignored");

    puts("out-of-range note_index ignored");
    guard_str("[{\"note_index\":7,\"applies\":true,\"directive_type\":\"no_charge_window\","
              "\"structured_adjustment\":{\"hours\":[5]},\"explanation\":\"x\"}]", 1, in);
    ck(in[0].type == GW_NO_OP, "note_index 7 for 1 note ignored");

    puts("non-array / null model output");
    guard_str("{\"oops\":1}", 2, in);
    ck(in[0].type == GW_NO_OP && in[1].type == GW_NO_OP, "object instead of array -> all no_op");
    gw_guard(NULL, 2, &BAT, in);
    ck(in[0].type == GW_NO_OP && in[1].type == GW_NO_OP, "NULL (provider down) -> all no_op");

    puts("applies is forced true for every non-no_op, whatever the model said");
    guard_str("[{\"note_index\":0,\"applies\":false,\"directive_type\":\"no_charge_window\","
              "\"structured_adjustment\":{\"hours\":[5]},\"explanation\":\"x\"}]", 1, in);
    ck(in[0].applies == 1, "model said applies:false on a real directive -> corrected to true");

    puts("infeasible directive still yields a valid schedule");
    {
        gw_scenario sc;
        memset(&sc, 0, sizeof sc);
        for (int h = 0; h < GW_H; h++) { sc.demand_kwh[h] = 100; sc.solar_kwh[h] = 0; sc.tariff[h] = 5; }
        sc.battery = BAT;
        gw_directive d;
        memset(&d, 0, sizeof d);
        d.type = GW_MAX_GRID_WINDOW; d.max_grid_kwh = 0; d.hours[3] = 1;  /* demand 100, grid 0, battery capped */
        gw_result res;
        int infeasible = gw_optimize(&sc, &d, 1, &res);
        ck(infeasible != 0, "impossible grid cap detected as infeasible (caller falls back)");
        ck(gw_optimize(&sc, NULL, 0, &res) == 0, "fallback with no directives is feasible");
        char err[256] = {0};
        ck(gw_validate(&sc, NULL, 0, &res, err, sizeof err) == 0, "fallback plan passes self-validation");
    }


    puts("best-effort fallback: two directives that conflict only together");
    {
        gw_scenario sc;
        memset(&sc, 0, sizeof sc);
        for (int h = 0; h < GW_H; h++) { sc.demand_kwh[h] = 100; sc.solar_kwh[h] = 0; sc.tariff[h] = 5; }
        sc.battery = BAT;                         /* cap 200, E0 120, min 40, +-50/h */
        gw_result res;
        char err[256];

        /* Each alone is satisfiable; together they are not: grid is capped to 60
           at hour 10 while discharging is simultaneously forbidden there. */
        gw_directive a, b;
        memset(&a, 0, sizeof a); a.type = GW_MAX_GRID_WINDOW;     a.max_grid_kwh = 60; a.hours[10] = 1;  /* needs 40 kWh discharge, within the 50 kWh/h cap */
        memset(&b, 0, sizeof b); b.type = GW_NO_DISCHARGE_WINDOW;                      b.hours[10] = 1;

        ck(gw_solve_best_effort(&sc, &a, 1, &res) == 1, "grid cap alone is feasible");
        ck(gw_solve_best_effort(&sc, &b, 1, &res) == 1, "no-discharge alone is feasible");

        gw_directive both[2] = { a, b };
        int applied = gw_solve_best_effort(&sc, both, 2, &res);
        ck(applied == 1, "conflicting pair -> exactly one dropped, not both");
        ck(gw_validate(&sc, NULL, 0, &res, err, sizeof err) == 0, "returned plan is still valid");

        /* Impossible on its own: 100 kWh demand, zero grid, no solar, 50 kWh/h discharge cap. */
        gw_directive imp;
        memset(&imp, 0, sizeof imp); imp.type = GW_MAX_GRID_WINDOW; imp.max_grid_kwh = 0; imp.hours[3] = 1;
        ck(gw_solve_best_effort(&sc, &imp, 1, &res) == 0, "unsatisfiable directive -> 0 applied, no error");
        ck(gw_validate(&sc, NULL, 0, &res, err, sizeof err) == 0, "unconstrained fallback plan is valid");
    }

    puts("self-contradictory base scenario still returns a plan, not a 500");
    {
        gw_scenario sc;
        memset(&sc, 0, sizeof sc);
        for (int h = 0; h < GW_H; h++) { sc.demand_kwh[h] = 100; sc.solar_kwh[h] = 0; sc.tariff[h] = 5; }
        sc.battery = (gw_battery){ 200, 30, 150, 50, 50 };   /* starts below its own floor */
        gw_result res;
        ck(gw_optimize(&sc, NULL, 0, &res) != 0, "raw LP correctly reports infeasible");
        ck(gw_solve_best_effort(&sc, NULL, 0, &res) >= 0, "best-effort relaxes the floor and returns a plan");
    }


    puts("overlapping solar_reduction takes the strictest factor, not the product");
    {
        gw_scenario sc;
        memset(&sc, 0, sizeof sc);
        for (int h = 0; h < GW_H; h++) { sc.demand_kwh[h] = 100; sc.solar_kwh[h] = 100; sc.tariff[h] = 5; }
        sc.battery = BAT;
        gw_directive a, b;
        memset(&a, 0, sizeof a); a.type = GW_SOLAR_REDUCTION; a.factor = 0.5; a.hours[12] = 1;
        memset(&b, 0, sizeof b); b.type = GW_SOLAR_REDUCTION; b.factor = 0.5; b.hours[12] = 1;
        gw_directive both[2] = { a, b };
        gw_result res;
        ck(gw_optimize(&sc, both, 2, &res) == 0, "two identical 50% reductions solve");
        /* Solar is free and demand is 100, so the plan uses all it can: the usable
           amount at hour 12 IS the effective cap. 50 means strictest-wins,
           25 would mean the two paraphrases compounded. */
        ck(fabs(res.plan[12].solar_used_kwh - 50.0) < 1e-6,
           "usable solar is 50, not 25 (paraphrases do not compound)");
    }

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "all guardrail checks passed");
    return fails != 0;
}
