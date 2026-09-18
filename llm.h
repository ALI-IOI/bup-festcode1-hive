#ifndef GW_LLM_H
#define GW_LLM_H

#include "optimizer.h"

void gw_llm_init(void);
void gw_llm_cleanup(void);

/* Interprets the operator notes. Returns a malloc'd JSON array string on
   success (caller frees), or NULL on any failure — a NULL result must leave
   the service returning a valid all-no_op plan, never a 5xx. */
char *gw_llm_interpret(const char *const *notes, int n_notes, const gw_battery *bat);

/* 1 if an API key is configured. /health stays green either way; the service
   must still answer with a valid plan when the provider is unreachable. */
int gw_llm_configured(void);

#endif
