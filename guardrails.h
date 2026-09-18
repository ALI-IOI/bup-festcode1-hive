#ifndef GW_GUARDRAILS_H
#define GW_GUARDRAILS_H

#include "optimizer.h"
#include "../vendor/cJSON.h"

#define GW_MAX_NOTES GW_MAX_DIRECTIVES
#define GW_EXPL_MAX  400

typedef struct {
    int               note_index;
    int               applies;      /* 0 only for no_op */
    gw_directive_type type;
    gw_directive      dir;          /* meaningful when type != GW_NO_OP */
    char              explanation[GW_EXPL_MAX];
} gw_interp;

const char *gw_type_name(gw_directive_type t);

/* Takes the raw array the model produced (may be NULL) and produces exactly
   n_notes validated entries in note_index order. Anything malformed, out of
   range, or unrecognised is demoted to no_op rather than rejected: a demoted
   note costs interpretation credit, a crash costs the whole case. Always
   succeeds. */
void gw_guard(cJSON *raw_array, int n_notes, const gw_battery *bat,
              gw_interp *out);

/* Collect the non-no_op directives for the optimizer. Returns the count. */
int gw_collect(const gw_interp *in, int n, gw_directive *out);

#endif
