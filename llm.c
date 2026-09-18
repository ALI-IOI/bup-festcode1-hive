#define _GNU_SOURCE
#include "llm.h"
#include "../vendor/cJSON.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHE_SLOTS 512

static int         openai_mode(void);
static const char *provider(void);
static const char *api_key(void);

/* ------------------------------------------------------------------ prompt */

static const char SYSTEM_PROMPT[] =
"You convert campus energy operator notes into structured directives. Output JSON only.\n"
"\n"
"ALLOWED directive_type values and their required structured_adjustment:\n"
"  solar_reduction          {\"hours\":[..], \"factor\": <0..1>}\n"
"  minimum_battery_reserve  {\"hours\":[..], \"minimum_energy_kwh\": <number>}\n"
"  no_charge_window         {\"hours\":[..]}\n"
"  no_discharge_window      {\"hours\":[..]}\n"
"  max_grid_window          {\"hours\":[..], \"max_grid_kwh\": <number>}\n"
"  no_op                    null\n"
"Never invent another type. Never change demand, tariff or battery parameters.\n"
"\n"
"TIME WINDOWS are start-INCLUSIVE and end-EXCLUSIVE, whole hours, 24h clock:\n"
"  \"1 PM to 3 PM\"            -> [13,14]\n"
"  \"from noon until 2 PM\"    -> [12,13]\n"
"  \"2 AM until 5 AM\"         -> [2,3,4]\n"
"  \"6 PM until 9 PM\"         -> [18,19,20]\n"
"  \"6 PM until 10 PM\"        -> [18,19,20,21]\n"
"  \"between 11 AM and 2 PM\"  -> [11,12,13]\n"
"  \"7 PM until 10 PM\"        -> [19,20,21]\n"
"  \"at 3 PM\" (single hour)   -> [15]\n"
"  \"after 6 PM\"              -> [18,19,20,21,22,23]\n"
"  \"before 6 AM\"             -> [0,1,2,3,4,5]\n"
"Hours must be unique integers 0-23 in ascending order.\n"
"\n"
"SOLAR factor is the fraction that REMAINS usable, not the amount removed:\n"
"  \"drops to about 25% of forecast\" -> 0.25\n"
"  \"80% reduction\"                  -> 0.2\n"
"  \"about half of forecast\"         -> 0.5\n"
"  \"one-fifth of normal output\"     -> 0.2\n"
"  \"cut by 60%\"                     -> 0.4\n"
"\n"
"RESERVES may be relative. Resolve them against battery capacity_kwh, which is\n"
"given in the user message. Example: \"keep 50% of battery capacity\" with\n"
"capacity 200 -> minimum_energy_kwh = 100.\n"
"\n"
"no_op means the note does not change THIS 24-hour electrical schedule.\n"
"Deadlines, room bookings, menus, notices, next-week or next-month events are\n"
"all no_op. Do not force an energy meaning onto an unrelated note.\n"
"\n"
"Return ONLY a JSON array, one object per note, in note_index order 0..N-1:\n"
"[{\"note_index\":0,\"applies\":true,\"directive_type\":\"solar_reduction\",\n"
"  \"structured_adjustment\":{\"hours\":[13,14],\"factor\":0.2},\n"
"  \"explanation\":\"Panel maintenance reduces usable solar.\"}]\n"
"applies is true for every non-no_op directive, and false only for no_op,\n"
"where structured_adjustment must be null. No prose, no markdown fences.";

/* ------------------------------------------------------------------- cache */

typedef struct { char *key; char *val; } cache_slot;
static cache_slot   g_cache[CACHE_SLOTS];
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned long fnv1a(const char *s)
{
    unsigned long h = 1469598103934665603UL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211UL; }
    return h;
}

static char *cache_get(const char *key)
{
    char *out = NULL;
    unsigned long h = fnv1a(key);
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < 8; i++) {
        cache_slot *s = &g_cache[(h + i) % CACHE_SLOTS];
        if (!s->key) break;
        if (strcmp(s->key, key) == 0) { out = strdup(s->val); break; }
    }
    pthread_mutex_unlock(&g_cache_mu);
    return out;
}

static void cache_put(const char *key, const char *val)
{
    unsigned long h = fnv1a(key);
    pthread_mutex_lock(&g_cache_mu);
    for (int i = 0; i < 8; i++) {
        cache_slot *s = &g_cache[(h + i) % CACHE_SLOTS];
        if (!s->key) { s->key = strdup(key); s->val = strdup(val); goto out; }
        if (strcmp(s->key, key) == 0) { free(s->val); s->val = strdup(val); goto out; }
    }
    /* Every probe slot taken by a different key: evict the first rather than
       silently stopping caching for the rest of the run. */
    {
        cache_slot *s = &g_cache[h % CACHE_SLOTS];
        free(s->key); free(s->val);
        s->key = strdup(key); s->val = strdup(val);
    }
out:
    pthread_mutex_unlock(&g_cache_mu);
}

/* -------------------------------------------------------------- http (curl) */

typedef struct { char *data; size_t len; } buf_t;

static size_t on_data(void *p, size_t sz, size_t nm, void *ud)
{
    size_t n = sz * nm;
    buf_t *b = ud;
    char *nd = realloc(b->data, b->len + n + 1);
    if (!nd) return 0;
    b->data = nd;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

void gw_llm_init(void)    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void gw_llm_cleanup(void) { curl_global_cleanup(); }

int gw_llm_configured(void)
{
    /* Ollama runs locally and needs no key. */
    if (!strcmp(provider(), "ollama")) return 1;
    return api_key() != NULL;
}

/* Two wire formats are supported. "anthropic" is the default. "openai" is the
   OpenAI chat-completions shape, which Gemini, Groq, OpenRouter, Together and
   Ollama all speak, so one mode covers every practical fallback provider. */
static int openai_mode(void)
{
    const char *p = getenv("GW_PROVIDER");
    return p && (!strcmp(p, "openai") || !strcmp(p, "gemini") ||
                 !strcmp(p, "groq")   || !strcmp(p, "openrouter") ||
                 !strcmp(p, "ollama"));
}

static const char *provider(void)
{
    const char *p = getenv("GW_PROVIDER");
    return (p && *p) ? p : "anthropic";
}

static const char *api_url(void)
{
    const char *u = getenv("GW_API_URL");
    if (u && *u) return u;
    const char *p = provider();
    if (!strcmp(p, "gemini"))
        return "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions";
    if (!strcmp(p, "groq"))
        return "https://api.groq.com/openai/v1/chat/completions";
    if (!strcmp(p, "openrouter"))
        return "https://openrouter.ai/api/v1/chat/completions";
    if (!strcmp(p, "ollama"))
        return "http://localhost:11434/v1/chat/completions";
    if (openai_mode())
        return "https://api.openai.com/v1/chat/completions";
    return "https://api.anthropic.com/v1/messages";
}

static const char *model_name(void)
{
    const char *m = getenv("GW_MODEL");
    if (m && *m) return m;
    const char *p = provider();
    if (!strcmp(p, "gemini"))     return "gemini-3-flash";
    if (!strcmp(p, "groq"))       return "llama-3.3-70b-versatile";
    if (openai_mode())            return "gpt-4o-mini";
    return "claude-sonnet-5";
}

/* Accept whichever key variable the chosen provider conventionally uses, so a
   team can export GEMINI_API_KEY or GROQ_API_KEY without renaming anything. */
static const char *api_key(void)
{
    static const char *names[] = { "GW_API_KEY", "ANTHROPIC_API_KEY",
                                   "GEMINI_API_KEY", "GOOGLE_API_KEY",
                                   "GROQ_API_KEY", "OPENROUTER_API_KEY",
                                   "OPENAI_API_KEY" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (v && *v) return v;
    }
    return NULL;
}

/* Strip markdown fences and prose, returning the first substring that actually
   parses as a JSON array. Trying each candidate '[' matters: a model that writes
   "Note [1] is a distractor. [{...}]" would otherwise yield a bogus slice, and a
   truncated response must fail here so the caller retries rather than silently
   degrading every note to no_op. */
static char *extract_array(const char *text)
{
    const char *end = strrchr(text, ']');
    if (!end) return NULL;

    for (const char *a = strchr(text, '['); a && a < end; a = strchr(a + 1, '[')) {
        size_t n = (size_t)(end - a) + 1;
        char *cand = malloc(n + 1);
        if (!cand) return NULL;
        memcpy(cand, a, n);
        cand[n] = 0;

        cJSON *probe = cJSON_Parse(cand);
        if (probe && cJSON_IsArray(probe)) { cJSON_Delete(probe); return cand; }
        if (probe) cJSON_Delete(probe);
        free(cand);
    }
    return NULL;
}

/* Total wall-clock budget for interpretation. The spec allows 30s per request;
   we deliberately spend far less so a slow or dead provider degrades to a valid
   no_op plan instead of dragging p95 into the 1/3 latency band. */
static long budget_ms(void)
{
    const char *e = getenv("GW_LLM_BUDGET_MS");
    long v = (e && *e) ? strtol(e, NULL, 10) : 12000;
    if (v < 1000)  v = 1000;
    if (v > 25000) v = 25000;
    return v;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* timeout_ms caps this attempt. *retryable says whether a second attempt could
   plausibly succeed: a timeout, 429 or 5xx might, but 401/403/404 never will —
   retrying those just doubles the latency of every request for the whole run. */
static char *call_once(const char *user_msg, long timeout_ms, int *retryable)
{
    *retryable = 0;
    const int oai = openai_mode();
    const char *key = api_key();
    if (!key && strcmp(provider(), "ollama")) return NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model_name());
    cJSON_AddNumberToObject(root, "temperature", 0);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    if (oai) {
        /* OpenAI shape: the system prompt is the first message, and the token
           cap is max_tokens on some gateways, max_completion_tokens on others;
           sending max_tokens is accepted by Gemini, Groq and OpenRouter. */
        cJSON_AddNumberToObject(root, "max_tokens", 1500);
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", SYSTEM_PROMPT);
        cJSON_AddItemToArray(msgs, sys);
    } else {
        cJSON_AddNumberToObject(root, "max_tokens", 1500);
        cJSON_AddStringToObject(root, "system", SYSTEM_PROMPT);
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user_msg);
    cJSON_AddItemToArray(msgs, m);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return NULL;

    CURL *c = curl_easy_init();
    if (!c) { free(payload); return NULL; }

    struct curl_slist *hdr = NULL;
    char auth[1024];
    if (oai) {
        if (key) {
            snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
            hdr = curl_slist_append(hdr, auth);
        }
    } else {
        snprintf(auth, sizeof auth, "x-api-key: %s", key);
        hdr = curl_slist_append(hdr, auth);
        hdr = curl_slist_append(hdr, "anthropic-version: 2023-06-01");
    }
    hdr = curl_slist_append(hdr, "content-type: application/json");

    buf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, api_url());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 4000 ? timeout_ms : 4000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    free(payload);

    if (rc != CURLE_OK || http < 200 || http >= 300) {
        if (rc != CURLE_OK)                                *retryable = 1;  /* timeout, DNS, reset */
        else if (http == 408 || http == 429 || http >= 500) *retryable = 1;  /* transient */
        else                                                *retryable = 0;  /* 401/403/404: hopeless */
        /* Never log the key or the body: the body can echo request content. */
        fprintf(stderr, "llm: request failed (curl=%d http=%ld retryable=%d)\n",
                (int)rc, http, *retryable);
        free(b.data);
        return NULL;
    }

    cJSON *resp = cJSON_Parse(b.data);
    free(b.data);
    if (!resp) { *retryable = 1; return NULL; }

    char *out = NULL;
    if (oai) {
        /* OpenAI shape: choices[].message.content */
        cJSON *ch, *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
        cJSON_ArrayForEach(ch, choices) {
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch, "message");
            cJSON *ct  = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
            if (cJSON_IsString(ct)) {
                out = extract_array(ct->valuestring);
                if (out) break;
            }
        }
    } else {
        /* Anthropic shape: find the first content block of type "text" —
           do not index content[0], it may be a tool-use block. */
        cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content"), *blk;
        cJSON_ArrayForEach(blk, content) {
            cJSON *ty = cJSON_GetObjectItemCaseSensitive(blk, "type");
            cJSON *tx = cJSON_GetObjectItemCaseSensitive(blk, "text");
            if (cJSON_IsString(ty) && !strcmp(ty->valuestring, "text") && cJSON_IsString(tx)) {
                out = extract_array(tx->valuestring);
                if (out) break;
            }
        }
    }
    cJSON_Delete(resp);
    if (!out) *retryable = 1;   /* prose, fences or a truncated array: try once more */
    return out;
}

char *gw_llm_interpret(const char *const *notes, int n_notes, const gw_battery *bat)
{
    /* Build the user message and the cache key together. */
    cJSON *u = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(u, "operator_notes");
    for (int i = 0; i < n_notes; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(notes[i] ? notes[i] : ""));
    cJSON *b = cJSON_AddObjectToObject(u, "battery");
    cJSON_AddNumberToObject(b, "capacity_kwh",       bat->capacity_kwh);
    cJSON_AddNumberToObject(b, "initial_energy_kwh", bat->initial_energy_kwh);
    cJSON_AddNumberToObject(b, "minimum_energy_kwh", bat->minimum_energy_kwh);
    char *ctx = cJSON_PrintUnformatted(u);
    cJSON_Delete(u);
    if (!ctx) return NULL;

    /* Sized to the actual content, never truncated. A truncated key makes two
       different note sets collide and return the wrong cached interpretation;
       a truncated prompt hands the model malformed JSON. */
    static const char LEAD[] = "Interpret each operator note. Return one entry per note, in order.\n";
    const char *model = model_name();
    size_t msg_n = sizeof LEAD + strlen(ctx);
    size_t key_n = strlen(provider()) + strlen(model) + strlen(ctx) + 3;
    char *user_msg = malloc(msg_n);
    char *key      = malloc(key_n);
    if (!user_msg || !key) { free(user_msg); free(key); free(ctx); return NULL; }
    snprintf(user_msg, msg_n, "%s%s", LEAD, ctx);
    snprintf(key, key_n, "%s|%s|%s", provider(), model, ctx);
    free(ctx);

    char *hit = cache_get(key);
    if (hit) { free(user_msg); free(key); return hit; }

    const long deadline = now_ms() + budget_ms();
    int retryable = 0;

    long left = deadline - now_ms();
    if (left <= 0) { free(user_msg); free(key); return NULL; }
    char *res = call_once(user_msg, left > 8000 ? 8000 : left, &retryable);

    /* Retry only if it could help AND there is real time left. Without this a
       hanging provider costs 2x the per-attempt timeout on every single request. */
    if (!res && retryable) {
        left = deadline - now_ms();
        if (left >= 3000) {
            struct timespec backoff = { .tv_sec = 0, .tv_nsec = 250L * 1000000L };
            nanosleep(&backoff, NULL);
            left = deadline - now_ms();
            if (left > 0) res = call_once(user_msg, left, &retryable);
        }
    }
    if (res) cache_put(key, res);
    free(user_msg);
    free(key);
    return res;
}
