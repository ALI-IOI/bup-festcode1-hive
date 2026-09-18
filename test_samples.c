#include "../src/optimizer.h"
#include "../vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
    buf[n] = 0; fclose(f); return buf;
}

static double num(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static int parse_scenario(cJSON *in, gw_scenario *sc)
{
    memset(sc, 0, sizeof *sc);
    cJSON *hours = cJSON_GetObjectItemCaseSensitive(in, "hours");
    cJSON *e;
    cJSON_ArrayForEach(e, hours) {
        int h = (int)num(e, "hour");
        if (h < 0 || h >= GW_H) return 1;
        sc->demand_kwh[h] = num(e, "demand_kwh");
        sc->solar_kwh[h]  = num(e, "solar_kwh");
        sc->tariff[h]     = num(e, "tariff_bdt_per_kwh");
    }
    cJSON *b = cJSON_GetObjectItemCaseSensitive(in, "battery");
    sc->battery.capacity_kwh               = num(b, "capacity_kwh");
    sc->battery.initial_energy_kwh         = num(b, "initial_energy_kwh");
    sc->battery.minimum_energy_kwh         = num(b, "minimum_energy_kwh");
    sc->battery.max_charge_kwh_per_hour    = num(b, "max_charge_kwh_per_hour");
    sc->battery.max_discharge_kwh_per_hour = num(b, "max_discharge_kwh_per_hour");
    return 0;
}

static int type_from_name(const char *s, gw_directive_type *t)
{
    if (!strcmp(s, "solar_reduction"))          { *t = GW_SOLAR_REDUCTION;     return 0; }
    if (!strcmp(s, "minimum_battery_reserve"))  { *t = GW_MIN_BATTERY_RESERVE; return 0; }
    if (!strcmp(s, "no_charge_window"))         { *t = GW_NO_CHARGE_WINDOW;    return 0; }
    if (!strcmp(s, "no_discharge_window"))      { *t = GW_NO_DISCHARGE_WINDOW; return 0; }
    if (!strcmp(s, "max_grid_window"))          { *t = GW_MAX_GRID_WINDOW;     return 0; }
    if (!strcmp(s, "no_op"))                    { *t = GW_NO_OP;               return 0; }
    return 1;
}

int main(void)
{
    char *txt = slurp("/mnt/user-data/uploads/BUP_CSE_FEST_2026_Preli_Public_Sample_Cases.json");
    cJSON *root = cJSON_Parse(txt);
    if (!root) { fprintf(stderr, "bad json\n"); return 1; }

    int pass = 0, total = 0;
    cJSON *cases = cJSON_GetObjectItemCaseSensitive(root, "cases"), *cs;
    cJSON_ArrayForEach(cs, cases) {
        total++;
        const char *id = cJSON_GetObjectItemCaseSensitive(cs, "id")->valuestring;
        cJSON *in  = cJSON_GetObjectItemCaseSensitive(cs, "input");
        cJSON *exp = cJSON_GetObjectItemCaseSensitive(cs, "expected_output");

        gw_scenario sc;
        if (parse_scenario(in, &sc)) { printf("%s  PARSE FAIL\n", id); continue; }

        gw_directive dirs[8]; int nd = 0;
        cJSON *di = cJSON_GetObjectItemCaseSensitive(exp, "directive_interpretation"), *ent;
        cJSON_ArrayForEach(ent, di) {
            gw_directive_type t;
            const char *tn = cJSON_GetObjectItemCaseSensitive(ent, "directive_type")->valuestring;
            if (type_from_name(tn, &t) || t == GW_NO_OP) continue;
            gw_directive *d = &dirs[nd++];
            memset(d, 0, sizeof *d);
            d->type = t;
            cJSON *sa = cJSON_GetObjectItemCaseSensitive(ent, "structured_adjustment");
            cJSON *hv, *hs = cJSON_GetObjectItemCaseSensitive(sa, "hours");
            cJSON_ArrayForEach(hv, hs) {
                int h = hv->valueint;
                if (h >= 0 && h < GW_H) d->hours[h] = 1;
            }
            d->factor             = num(sa, "factor");
            d->minimum_energy_kwh = num(sa, "minimum_energy_kwh");
            d->max_grid_kwh       = num(sa, "max_grid_kwh");
        }

        gw_result res;
        if (gw_optimize(&sc, dirs, nd, &res)) { printf("%s  INFEASIBLE\n", id); continue; }

        char err[256] = {0};
        int bad = gw_validate(&sc, dirs, nd, &res, err, sizeof err);
        double ref = num(exp, "total_cost_bdt");
        double diff = res.total_cost_bdt - ref;

        printf("%-10s ref=%9.2f  mine=%9.2f  diff=%+8.2f  %s%s%s\n",
               id, ref, res.total_cost_bdt, diff,
               bad ? "INVALID: " : "VALID",
               bad ? err : "",
               (!bad && fabs(diff) <= 0.01) ? "  [optimal]" : "");
        if (!bad && fabs(diff) <= 0.01) pass++;
    }
    printf("\n%d/%d cases valid AND cost-optimal\n", pass, total);
    cJSON_Delete(root); free(txt);
    return pass == total ? 0 : 1;
}
