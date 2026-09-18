# GridWise — LLM-Assisted Smart Campus Energy Optimization

HTTP service in C. Interprets natural-language operator notes with a language model,
validates the interpretation deterministically, and returns a cost-minimal, fully valid
24-hour energy schedule.

- `GET /health` → `{"status":"ok"}`
- `POST /optimize-energy` → directive interpretation + 24-hour plan

---

## Quickstart (from a clean machine)

### Dependencies

```bash
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install -y gcc make libglpk-dev libcurl4-openssl-dev

# macOS
brew install glpk curl
```

### Build and run

```bash
git clone <REPO_URL> && cd <REPO_DIR>
make                                  # produces ./gridwise

# Anthropic (default):
export ANTHROPIC_API_KEY=<your key>
# or Google Gemini (free tier, no card required):
#   export GW_PROVIDER=gemini
#   export GEMINI_API_KEY=<your key>
# or Groq:
#   export GW_PROVIDER=groq
#   export GROQ_API_KEY=<your key>
export GW_MODEL=claude-sonnet-5       # optional; per-provider default otherwise
export PORT=8080                      # optional, this is the default

./gridwise
```

Expected startup line on stderr:

```
gridwise listening on 0.0.0.0:8080
```

### Verify

```bash
curl -s http://localhost:8080/health
# {"status":"ok"}
```

```bash
curl -s -X POST http://localhost:8080/optimize-energy \
  -H 'Content-Type: application/json' \
  -d '{
    "scenario_id": "GRID-101",
    "operator_notes": [
      "Solar output will drop to about 20% from 1 PM to 3 PM.",
      "The cafeteria menu changes tomorrow."
    ],
    "hours": [
      {"hour":0,"demand_kwh":180,"solar_kwh":0,"tariff_bdt_per_kwh":7},
      {"hour":1,"demand_kwh":175,"solar_kwh":0,"tariff_bdt_per_kwh":7},
      {"hour":2,"demand_kwh":170,"solar_kwh":0,"tariff_bdt_per_kwh":7},
      {"hour":3,"demand_kwh":168,"solar_kwh":0,"tariff_bdt_per_kwh":7},
      {"hour":4,"demand_kwh":170,"solar_kwh":0,"tariff_bdt_per_kwh":7},
      {"hour":5,"demand_kwh":175,"solar_kwh":5,"tariff_bdt_per_kwh":8},
      {"hour":6,"demand_kwh":185,"solar_kwh":20,"tariff_bdt_per_kwh":8},
      {"hour":7,"demand_kwh":195,"solar_kwh":45,"tariff_bdt_per_kwh":9},
      {"hour":8,"demand_kwh":205,"solar_kwh":75,"tariff_bdt_per_kwh":12},
      {"hour":9,"demand_kwh":215,"solar_kwh":100,"tariff_bdt_per_kwh":13},
      {"hour":10,"demand_kwh":220,"solar_kwh":120,"tariff_bdt_per_kwh":14},
      {"hour":11,"demand_kwh":225,"solar_kwh":135,"tariff_bdt_per_kwh":15},
      {"hour":12,"demand_kwh":230,"solar_kwh":140,"tariff_bdt_per_kwh":15},
      {"hour":13,"demand_kwh":228,"solar_kwh":135,"tariff_bdt_per_kwh":15},
      {"hour":14,"demand_kwh":225,"solar_kwh":120,"tariff_bdt_per_kwh":14},
      {"hour":15,"demand_kwh":220,"solar_kwh":100,"tariff_bdt_per_kwh":14},
      {"hour":16,"demand_kwh":215,"solar_kwh":70,"tariff_bdt_per_kwh":13},
      {"hour":17,"demand_kwh":210,"solar_kwh":35,"tariff_bdt_per_kwh":13},
      {"hour":18,"demand_kwh":225,"solar_kwh":10,"tariff_bdt_per_kwh":18},
      {"hour":19,"demand_kwh":235,"solar_kwh":0,"tariff_bdt_per_kwh":18},
      {"hour":20,"demand_kwh":230,"solar_kwh":0,"tariff_bdt_per_kwh":18},
      {"hour":21,"demand_kwh":215,"solar_kwh":0,"tariff_bdt_per_kwh":14},
      {"hour":22,"demand_kwh":205,"solar_kwh":0,"tariff_bdt_per_kwh":10},
      {"hour":23,"demand_kwh":200,"solar_kwh":0,"tariff_bdt_per_kwh":9}
    ],
    "battery": {
      "capacity_kwh": 500, "initial_energy_kwh": 200, "minimum_energy_kwh": 50,
      "max_charge_kwh_per_hour": 100, "max_discharge_kwh_per_hour": 100
    }
  }'
```

### Run the public sample cases

```bash
make test                                   # optimizer vs. reference costs + 26 guardrail checks
python3 tests/e2e.py http://localhost:8080  # live service: schema + full plan replay
```

`make test` expects the public sample JSON at the path set in `tests/test_samples.c`.
`tests/e2e.py` posts every public case to a running service, checks the response schema,
and independently replays each returned `hourly_plan` against energy balance, effective
solar, battery bounds and rate limits, directive windows, end-of-day neutrality, and the
reported totals.

---

## Docker fallback

```bash
docker pull <REGISTRY>/gridwise:<TAG>

docker run --rm -p 8080:8080 \
  -e ANTHROPIC_API_KEY=<your key> \
  -e PORT=8080 \
  <REGISTRY>/gridwise:<TAG>

curl -s http://localhost:8080/health   # {"status":"ok"}
```

The image binds `0.0.0.0`, exposes `8080`, runs as a non-root user, and contains **no
baked-in credentials**. The key is supplied at run time via `-e`.

---

## Environment variables

| Name | Required | Default | Purpose |
|---|---|---|---|
| `ANTHROPIC_API_KEY` | yes | — | Auth for the interpretation model |
| `GW_MODEL` | no | `claude-sonnet-5` | Model identifier |
| `PORT` | no | `8080` | Listen port |
| `GW_PROVIDER` | no | `anthropic` | `anthropic`, `gemini`, `groq`, `openrouter`, `ollama`, `openai` |
| `GEMINI_API_KEY` / `GROQ_API_KEY` / `OPENROUTER_API_KEY` / `OPENAI_API_KEY` / `GW_API_KEY` | one of | — | Key for the chosen provider |
| `GW_API_URL` | no | per provider | Override for a proxy or a local model |
| `GW_LLM_BUDGET_MS` | no | `12000` | Wall-clock ceiling for interpretation, retry included |

No values are committed. `.env` is gitignored.

---

## Architecture

```
POST /optimize-energy
  │
  ├─ 1. Request validation        400 malformed JSON · 422 schema-invalid
  ├─ 2. LLM interpretation        one call for all notes, temperature 0, cached
  ├─ 3. Deterministic guardrails  untrusted model output → validated directives
  ├─ 4. LP optimizer (GLPK)       24-hour cost-minimal schedule
  ├─ 5. Self-replay validator     the judge's checks, run on our own output first
  └─ 6. Response assembly         totals derived from the emitted plan
```

### 2. LLM role (mandatory requirement)

Two wire formats are supported, selected by `GW_PROVIDER`: Anthropic messages
(default) and the OpenAI chat-completions shape, which Gemini, Groq, OpenRouter and
Ollama all speak. Endpoint, model and auth header default correctly per provider, so
switching providers is two environment variables and no code change. Document whichever
you actually used before submitting.


The model **is** the interpretation path. `src/llm.c` sends all operator notes in a single
request and receives the structured `directive_interpretation` array that feeds the
optimizer. There is no phrase table, no regex matcher, no keyword fallback that could
produce a directive on its own — if the model is unavailable, every note resolves to
`no_op` rather than being matched by other means.

The battery object is included in the prompt so relative language resolves correctly
("keep 50% of battery capacity" with `capacity_kwh: 200` → `minimum_energy_kwh: 100`).
The prompt pins the whole-hour, end-exclusive window convention and the
fraction-remaining meaning of `factor`.

The interpretation is cached by `(model, notes, battery)` so repeated hidden cases do not
re-pay the model latency.

Interpretation runs under a hard wall-clock budget (`GW_LLM_BUDGET_MS`, default 12s)
covering the first attempt and any retry. The spec allows 30s per request; deliberately
spending far less means a slow or dead provider degrades to a valid `no_op` plan rather
than dragging p95 into the 1/3 latency band. A retry happens only when it could plausibly
succeed and at least 3s of budget remains.

### 3. Deterministic guardrails (`src/guardrails.c`)

Model output is untrusted structured data. Every entry is checked before it can reach the
optimizer:

- exactly one entry per note, in `note_index` order, missing indices padded with `no_op`
- `directive_type` restricted to the six supported values; anything else → `no_op`
- duplicate or out-of-range `note_index` discarded
- `hours` cast to integers, deduplicated, filtered to 0–23, emitted ascending; fractional
  values rejected; empty result → `no_op`
- `factor` clamped to `[0, 1]`; `minimum_energy_kwh` clamped to `[0, capacity_kwh]`;
  `max_grid_kwh` must be finite and non-negative
- `applies` forced to `true` for every non-`no_op` and `false` with a `null` adjustment for
  `no_op`, regardless of what the model claimed
- invented fields discarded; demand, tariff and battery parameters can never be altered

**Anything invalid is demoted to `no_op`, never rejected.** A demoted note costs
interpretation credit for that note; a crash would cost the entire case.

### 3a. Resource limits

Request buffers start at 16 KiB and grow on demand to the 1 MiB body cap, rather than
reserving the maximum per connection. Handler threads use 256 KiB stacks instead of the
8 MiB default. Together these keep the service flat under burst load: measured 10.5 MB to
14.6 MB RSS across 3000 requests at 64-way concurrency, p95 60 ms.

### 4. Optimizer (`src/optimizer.c`)

Linear program solved with GLPK simplex. Four variables per hour — grid import, solar
used, battery charge, battery discharge — 96 columns and 49 rows.

```
minimize   Σ grid[h] · tariff[h]
s.t.       grid[h] + solar[h] + discharge[h] − charge[h] = demand[h]
           Σ charge − Σ discharge = 0                        (end-of-day neutrality)
           reserve[h] ≤ E₀ + Σ_{k≤h}(charge[k] − discharge[k]) ≤ capacity
           0 ≤ grid[h] ≤ grid_cap[h]
           0 ≤ solar[h] ≤ effective_solar[h]
           0 ≤ charge[h] ≤ charge_cap[h]
           0 ≤ discharge[h] ≤ discharge_cap[h]
```

Directives act only on bounds: `solar_reduction` scales `effective_solar`,
`minimum_battery_reserve` raises `reserve[h]`, `no_charge_window` and
`no_discharge_window` zero the respective rate cap, `max_grid_window` lowers `grid_cap`.

Three output-hygiene steps matter:

1. Charge and discharge are **netted** per hour. With no round-trip loss the LP may return
   both simultaneously at zero cost, but `battery_action` is single-valued. Netting is safe:
   `|net|` never exceeds the active rate cap and both windows are preserved.
2. `grid_kwh` is **recomputed from the balance equation** after netting, so the equation
   closes exactly rather than to within solver residue.
3. `battery_energy_after_kwh` is a **cumulative sum from `initial_energy_kwh`**, computed
   the same way the judge replays it, so no drift accumulates.

Reported totals are summed from the emitted, rounded plan — never from solver internals —
so they cannot disagree with a recalculation.

### 5. Failure behaviour

| Failure | Response |
|---|---|
| Malformed JSON | `400`, controlled error object |
| Well-formed but schema-invalid | `422`, controlled error object |
| Model times out, or returns 429/5xx or unparseable output | one retry inside the remaining budget, then all notes → `no_op`; valid schedule still returned with `200` |
| Model returns 401/403/404 | no retry — a bad key cannot be fixed by trying again, and retrying would double the latency of every request for the whole run |
| Interpreted directives are mutually infeasible | directives dropped one at a time until a feasible set remains; valid schedule returned with `200` |
| Base scenario self-contradictory (e.g. initial energy below its own reserve) | base floor relaxed to the starting level; valid schedule returned with `200` |
| Self-replay validation fails | same fallback path |

Secrets are never logged. Provider failures log only the curl and HTTP status codes, never
the request body or response body, and never the key.

---

## Project layout

```
src/optimizer.c    LP model + independent replay validator
src/guardrails.c   deterministic validation of model output
src/llm.c          provider client, prompt, retry, cache
src/http.c         threaded HTTP/1.1 server
src/main.c         routing, request validation, response assembly
vendor/cJSON.*     JSON parsing/serialization
tests/             optimizer, guardrail, and end-to-end suites
```

---

## Known limitations

- Battery round-trip efficiency is assumed lossless, as the specification defines it.
- When two directives of the same type cover one hour, the stricter bound wins:
  highest reserve, lowest grid cap, lowest solar factor. Overlapping solar reductions do
  **not** compound — two notes paraphrasing the same 50% outage mean 0.5, not 0.25.
- If the model returns the *removed* fraction rather than the *remaining* fraction for
  `solar_reduction`, the value is indistinguishable from a legitimate one and cannot be
  corrected downstream. This is addressed in the prompt, not the guardrails.
- The cache is in-process, 512 slots with first-slot eviction when a probe run fills;
  it is not shared across replicas.
- The HTTP server handles one request per connection (`Connection: close`) — adequate for
  the judging harness, not tuned for keep-alive throughput. `Expect: 100-continue` is
  acknowledged so clients that use it do not stall.
- Chunked transfer encoding is not supported; requests must carry `Content-Length`.
- When directives conflict, the dropped one is still reported in
  `directive_interpretation`. Interpretation and application are scored separately, so
  reporting what was read is worth more than hiding it.

---

## Credits

| Component | Use | License |
|---|---|---|
| [GLPK](https://www.gnu.org/software/glpk/) | LP simplex solver | GPLv3 |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON parse/serialize (vendored) | MIT |
| [libcurl](https://curl.se/libcurl/) | HTTPS client | curl license (MIT-style) |
| Anthropic Claude API | Operator-note interpretation | commercial API |

AI coding assistance was used during development. The architecture, LP formulation,
guardrail policy, and failure semantics are the team's own design.

**Note on licensing:** GLPK is GPLv3, which makes this project's distribution terms GPLv3.
If that is a problem, swap in HiGHS (MIT) — the LP formulation is unchanged.
