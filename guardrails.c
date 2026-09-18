#include "guardrails.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

const char *gw_type_name(gw_directive_type t)
{
    switch (t) {
    case GW_SOLAR_REDUCTION:     return "solar_reduction";
    case GW_MIN_BATTERY_RESERVE: return "minimum_battery_reserve";
    case GW_NO_CHARGE_WINDOW:    return "no_charge_window";
    case GW_NO_DISCHARGE_WINDOW: return "no_discharge_window";
    case GW_MAX_GRID_WINDOW:     return "max_grid_window";
    default:                     return "no_op";
    }
}

static int type_from_name(const char *s, gw_directive_type *t)
{
    if (!s) return 1;
    if (!strcmp(s, "solar_reduction"))         { *t = GW_SOLAR_REDUCTION;     return 0; }
    if (!strcmp(s, "minimum_battery_reserve")) { *t = GW_MIN_BATTERY_RESERVE; return 0; }
    if (!strcmp(s, "no_charge_window"))        { *t = GW_NO_CHARGE_WINDOW;    return 0; }
    if (!strcmp(s, "no_discharge_window"))     { *t = GW_NO_DISCHARGE_WINDOW; return 0; }
    if (!strcmp(s, "max_grid_window"))         { *t = GW_MAX_GRID_WINDOW;     return 0; }
    if (!strcmp(s, "no_op"))                   { *t = GW_NO_OP;               return 0; }
    return 1;
}

static void make_no_op(gw_interp *e, int idx, const char *why)
{
    memset(e, 0, sizeof *e);
    e->note_index = idx;
    e->applies    = 0;
    e->type       = GW_NO_OP;
    snprintf(e->explanation, GW_EXPL_MAX, "%s", why);
}

static int finite_num(cJSON *o, const char *k, double *v)
{
    cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(x)) return 1;
    if (!isfinite(x->valuedouble)) return 1;
    *v = x->valuedouble;
    return 0;
}

void gw_guard(cJSON *raw, int n_notes, const gw_battery *bat, gw_interp *out)
{
    if (n_notes > GW_MAX_NOTES) n_notes = GW_MAX_NOTES;

    /* Default every slot to no_op. A model that returns fewer entries than
       there are notes, or skips an index, still yields a complete, in-order
       array. Missing or duplicate mappings are a schema failure. */
    for (int i = 0; i < n_notes; i++)
        make_no_op(&out[i], i, "Note does not affect the 24-hour energy schedule.");

    if (!cJSON_IsArray(raw)) return;

    int seen[GW_MAX_NOTES] = {0};
    cJSON *ent;
    cJSON_ArrayForEach(ent, raw) {
        if (!cJSON_IsObject(ent)) continue;

        cJSON *ni = cJSON_GetObjectItemCaseSensitive(ent, "note_index");
        if (!cJSON_IsNumber(ni)) continue;
        int idx = (int)ni->valuedouble;
        if (idx < 0 || idx >= n_notes) continue;   /* out of range */
        if (seen[idx]) continue;                   /* duplicate: first wins */
        seen[idx] = 1;

        cJSON *ex = cJSON_GetObjectItemCaseSensitive(ent, "explanation");
        char expl[GW_EXPL_MAX];
        snprintf(expl, GW_EXPL_MAX, "%s",
                 cJSON_IsString(ex) && ex->valuestring ? ex->valuestring : "Interpreted.");

        gw_directive_type t;
        cJSON *dt = cJSON_GetObjectItemCaseSensitive(ent, "directive_type");
        if (type_from_name(cJSON_IsString(dt) ? dt->valuestring : NULL, &t)) {
            make_no_op(&out[idx], idx, "Unsupported directive type; treated as no_op.");
            continue;
        }
        if (t == GW_NO_OP) { make_no_op(&out[idx], idx, expl); continue; }

        cJSON *sa = cJSON_GetObjectItemCaseSensitive(ent, "structured_adjustment");
        if (!cJSON_IsObject(sa)) {
            make_no_op(&out[idx], idx, "Missing structured_adjustment; treated as no_op.");
            continue;
        }

        gw_directive d;
        memset(&d, 0, sizeof d);
        d.type = t;

        /* hours: dedupe, integer, 0..23. The mask makes duplicates and
           out-of-order input harmless; emission is always ascending. */
        int any = 0;
        cJSON *hv, *hs = cJSON_GetObjectItemCaseSensitive(sa, "hours");
        if (!cJSON_IsArray(hs)) {
            make_no_op(&out[idx], idx, "Missing hours; treated as no_op.");
            continue;
        }
        cJSON_ArrayForEach(hv, hs) {
            if (!cJSON_IsNumber(hv)) continue;
            double hd = hv->valuedouble;
            if (!isfinite(hd)) continue;
            int h = (int)hd;
            if ((double)h != hd) continue;          /* reject 13.5 */
            if (h < 0 || h >= GW_H) continue;
            if (!d.hours[h]) { d.hours[h] = 1; any = 1; }
        }
        if (!any) {
            make_no_op(&out[idx], idx, "No valid hours in range; treated as no_op.");
            continue;
        }

        double v;
        switch (t) {
        case GW_SOLAR_REDUCTION:
            if (finite_num(sa, "factor", &v)) {
                make_no_op(&out[idx], idx, "Invalid solar factor; treated as no_op.");
                continue;
            }
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;                   /* factor is the fraction REMAINING */
            d.factor = v;
            break;
        case GW_MIN_BATTERY_RESERVE:
            if (finite_num(sa, "minimum_energy_kwh", &v) || v < 0.0) {
                make_no_op(&out[idx], idx, "Invalid reserve; treated as no_op.");
                continue;
            }
            if (v > bat->capacity_kwh) v = bat->capacity_kwh;
            d.minimum_energy_kwh = v;
            break;
        case GW_MAX_GRID_WINDOW:
            if (finite_num(sa, "max_grid_kwh", &v) || v < 0.0) {
                make_no_op(&out[idx], idx, "Invalid grid cap; treated as no_op.");
                continue;
            }
            d.max_grid_kwh = v;
            break;
        default:
            break;                                   /* window types need hours only */
        }

        out[idx].note_index = idx;
        out[idx].applies    = 1;                     /* non-no_op is always true */
        out[idx].type       = t;
        out[idx].dir        = d;
        snprintf(out[idx].explanation, GW_EXPL_MAX, "%s", expl);
    }
}

int gw_collect(const gw_interp *in, int n, gw_directive *out)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        if (in[i].type != GW_NO_OP) out[k++] = in[i].dir;
    return k;
}
