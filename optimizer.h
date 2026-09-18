#ifndef GW_OPTIMIZER_H
#define GW_OPTIMIZER_H

#define GW_H 24
#define GW_MAX_DIRECTIVES 8

typedef enum {
    GW_SOLAR_REDUCTION = 0,
    GW_MIN_BATTERY_RESERVE,
    GW_NO_CHARGE_WINDOW,
    GW_NO_DISCHARGE_WINDOW,
    GW_MAX_GRID_WINDOW,
    GW_NO_OP
} gw_directive_type;

/* A validated directive. hours[] is a 0/1 mask over the 24 hours. */
typedef struct {
    gw_directive_type type;
    int    hours[GW_H];        /* 1 = hour affected */
    double factor;             /* solar_reduction: usable fraction remaining */
    double minimum_energy_kwh; /* minimum_battery_reserve */
    double max_grid_kwh;       /* max_grid_window */
} gw_directive;

typedef struct {
    double capacity_kwh;
    double initial_energy_kwh;
    double minimum_energy_kwh;
    double max_charge_kwh_per_hour;
    double max_discharge_kwh_per_hour;
} gw_battery;

typedef struct {
    double demand_kwh[GW_H];
    double solar_kwh[GW_H];
    double tariff[GW_H];
    gw_battery battery;
} gw_scenario;

typedef struct {
    double grid_kwh;
    double solar_used_kwh;
    const char *battery_action;  /* "charge" | "discharge" | "idle" */
    double battery_kwh;
    double battery_energy_after_kwh;
} gw_hour_plan;

typedef struct {
    gw_hour_plan plan[GW_H];
    double total_grid_kwh;
    double total_cost_bdt;
    double peak_grid_kwh;
} gw_result;

/* Returns 0 on success, non-zero if the LP is infeasible or the solver failed. */
int gw_optimize(const gw_scenario *sc,
                const gw_directive *dirs, int ndirs,
                gw_result *out);

/* Independent replay of a plan, exactly as the judge does it.
   Writes a human-readable reason into err (size errsz) on failure.
   Returns 0 if the plan is valid. */
int gw_validate(const gw_scenario *sc,
                const gw_directive *dirs, int ndirs,
                const gw_result *res, char *err, int errsz);

/* Schedules as many of the supplied directives as are jointly feasible, dropping
   one at a time before giving up on all of them. Returns the number applied, or
   -1 if even a relaxed schedule is impossible. Never yields an invalid plan. */
int gw_solve_best_effort(const gw_scenario *sc,
                         const gw_directive *dirs, int ndirs,
                         gw_result *out);

#endif
