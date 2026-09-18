# Submission Checklist — fill in before the deadline

Five deliverables. Items 1, 4 and 5 are actions only your team can complete.

## 1. Public endpoint
- [ ] Base URL: `________________________________`
- [ ] `GET  <base>/health` returns `{"status":"ok"}` **from outside your network**
- [ ] `POST <base>/optimize-energy` accepts a public sample and returns 200
- [ ] No login, dashboard, VPN or manual approval required
- [ ] Stays reachable for the whole evaluation window

## 2. GitHub repository
- [ ] Created **after** the question reveal
- [ ] Private during the event
- [ ] **Made public after the submission deadline**
- [ ] `.env` never committed (already gitignored)
- [ ] URL: `________________________________`

## 3. README
Already complete in `README.md`. Verify one thing yourself:
- [ ] A teammate who did **not** write the code does a clean `git clone` into a
      fresh directory, follows the README verbatim, and reaches
      `{"status":"ok"}` plus one successful sample — with no undocumented steps.

## 4. Docker fallback image  — NOT YET BUILT
```bash
docker build -t <REGISTRY>/gridwise:v1 .
docker run --rm -p 8080:8080 -e ANTHROPIC_API_KEY=<key> <REGISTRY>/gridwise:v1
curl -s http://localhost:8080/health     # must return {"status":"ok"}
docker push <REGISTRY>/gridwise:v1
```
- [ ] Image builds
- [ ] `/health` reachable from the running container
- [ ] Pushed and pullable by exact tag or digest
- [ ] No secrets baked in (key passed with `-e` only)
- [ ] Reference: `________________________________`
- [ ] Paste the exact `docker pull` + `docker run` commands into the README

## 5. Three-minute video (max 3:00)
Tie-break only — no base points, but it is the **first** tie-breaker.
Cover: the problem · architecture overview · LLM → guardrails → optimizer flow ·
key implementation choices · how to run and test it.
- [ ] Recorded, under 3:00, accessible to judges
- [ ] Link: `________________________________`

---

## Before you submit

- [ ] Run `make test` — expect 28 guardrail checks and 10/10 cases optimal
- [ ] Run `python3 tests/e2e.py <base-url>` against the **deployed** service
- [ ] Test with the real API key set and confirm directive types match the samples
- [ ] Kill the key temporarily and confirm the service still returns a valid 200
- [ ] Malformed JSON returns 400, not a crash
- [ ] No secrets in repo, logs, responses, or the image
