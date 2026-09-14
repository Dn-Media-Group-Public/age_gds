# Plan: opt-in caching for repeated anonymous-mode graph construction

## Problem / Goal

`article-graph`'s query-pipeline benchmark
(`query-pipeline-benchmark.md`) found anonymous-mode `age_gds.bfs_stream`
(`age_graph`/`node_query`/`edge_query` supplied, no `graph_name`) pays a
large fixed cost — ~7 seconds at 1M-node/2M-edge scale, ~25ms at
5,000-node scale — on **every single call**, regardless of how small the
filtered result ends up being, because anonymous mode rebuilds the entire
`NetworKit::Graph` from `node_query`/`edge_query` from scratch each time.
This is by explicit design (Stage 7,
[`07-anonymous-execution.md`](implemented-plans/07-anonymous-execution.md)):
anonymous execution "does NOT populate session cache" — the `anon` graph
is stack/heap-local and freed at the end of each call, with a Stage 7 done
criterion explicitly testing that `cache_stats()` stays at 0 entries after
an anonymous call.

`article-graph` benchmarked two independent fixes for this exact problem
from its own side (found in that repo's `helpers.py`/`pipeline.py`):
routing the SQL filter through raw-SQL `UNWIND` instead of Cypher `MATCH`
inside `node_query`/`edge_query`, which cut Path A from ~2,400-5,500x
slower than the old baseline down to ~2x slower. **That fix already closes
nearly all of the gap for the case actually measured** (identical
`age_graph`/filter, single call). This plan is for the residual case that
fix does *not* help: a workload that calls anonymous mode **repeatedly with
the same (or overlapping) `age_graph`+`node_query`+`edge_query`**, where
the ~25ms-per-call graph-construction cost is paid over and over even
though nothing changed between calls. Whether that access pattern actually
occurs in `article-graph`'s production traffic has **not been established**
— its own benchmark's "Option 1" analysis leans toward dropping anonymous
mode entirely for that project rather than paying to fix this. This plan
should only be implemented if a caller with a genuine repeated-anonymous-call
pattern is identified; it is written now so the design exists if that need
materializes.

## Design decision required before implementing

**This plan proposes changing a currently-tested, intentional invariant**
(Stage 7: anonymous mode never populates the session cache). Any
implementation must either:
(a) replace that done-criterion/test with a new one reflecting opt-in
caching, or
(b) gate the new caching behind an explicit new parameter so the *existing*
default (no `graph_name` ⇒ never cached) is unchanged, and Stage 7's test
keeps passing unmodified for calls that don't opt in.

**Recommend (b)** — introduce caching only when a caller explicitly asks
for it, not as a silent change to what "anonymous" means today. Concretely:
a new optional parameter, e.g. `cache_key text DEFAULT NULL`, on the same
functions that already accept `age_graph`/`node_query`/`edge_query`. When
`cache_key` is supplied, the resolved graph is stored in (and looked up
from) the *existing* session cache under that key — reusing the exact same
`g_cache`/`ProjectedGraph`/LRU-eviction machinery Stage 4 already built for
named projections — instead of `graph_project`'s catalog-backed load path.
When `cache_key` is NULL (the default, and the only option today), behavior
is byte-identical to current anonymous mode: no caching, matching Stage 7's
existing test.

### Staleness semantics — the hard part

Named/persistent projections (`graph_name`) have an explicit
project/reindex lifecycle (`graph_project`, `graph_reindex`,
`projection_refresh_state` per `article-graph`'s own schema) that defines
when a cached graph should be considered stale. Anonymous mode has no such
lifecycle — the caller doesn't "own" a projection, it just supplies a query
pair per call. A `cache_key`-based cache therefore needs its own staleness
rule, since nothing currently tells `age_gds` when the underlying AGE data
matching `node_query`/`edge_query` has changed. Options:

1. **TTL-only** — cache entries under `cache_key` expire after N seconds
   (a new GUC, e.g. `age_gds.anon_cache_ttl_seconds`), never invalidated by
   writes. Simple, but silently serves stale results for up to the TTL
   window after a write — acceptable only if the caller already tolerates
   eventual consistency (article-graph's own dedup pipeline is
   asynchronous/debounced, so this may be fine for that caller, but this
   plan should not assume that without the caller's explicit sign-off).
2. **Manual invalidation only** — add `age_gds.anon_cache_invalidate(cache_key text)`,
   mirroring `session_cache_invalidate`; caller is responsible for calling
   it after any write it knows affects that key. No automatic staleness
   detection at all. Simplest to implement correctly, but pushes all
   correctness responsibility onto the caller.
3. **Content-hash validation** — re-run a cheap fingerprint query (e.g.
   `count(*)` + `max(id)` over the matched node/edge sets) on every call
   even when serving from cache, and invalidate on mismatch. Defeats much
   of the point (still pays a query per call, just a cheaper one than full
   reconstruction) but is the only option that's automatically correct
   without caller cooperation.

**Recommend option 2** (manual invalidation) as the simplest correct
design, since it mirrors the existing `session_cache_invalidate` API
exactly and puts staleness handling in the same place callers already
have to reason about it for named projections (`graph_drop` also requires
caller-driven lifecycle management today). This needs explicit sign-off
before implementation, though — it's a real usability tradeoff (correct but
requires caller discipline) vs. option 1 (easy but silently stale) vs.
option 3 (safe but partially defeats the optimization).

## Implementation order (once the above is decided)

1. Add `cache_key text DEFAULT NULL` parameter to `get_or_load_graph`
   (`session_cache.h`/`.cpp`) alongside the existing
   `age_graph`/`node_query`/`edge_query` parameters.
2. When `cache_key` is non-NULL: look up `g_cache` by `cache_key` first
   (same map used by named projections — verify key-namespace collision
   with real `graph_name`s is impossible, e.g. by reserving a prefix or
   using a separate map); on miss, run `age_load_graph` (the same call
   anonymous mode already makes) and insert into `g_cache` under
   `cache_key`, subject to the same LRU/`session_cache_mb` eviction as
   named projections.
3. Add `age_gds.anon_cache_invalidate(cache_key text)` (or extend
   `session_cache_invalidate` to accept either a `graph_name` or a
   `cache_key`, if the namespace is shared).
4. Thread the new `cache_key` parameter through the SQL function
   signatures that already expose `age_graph`/`node_query`/`edge_query`
   (at minimum `bfs_stream`; decide whether to add it to every
   anonymous-capable algorithm or just path-finding functions, based on
   which callers actually need it).
5. Tests: cache hit on repeated identical `cache_key` (assert
   `cache_stats().entry_count` increases by 1, not per call); cache miss on
   a new key; invalidation removes the entry; **Stage 7's existing
   "anonymous execution does NOT populate session cache" test must still
   pass unmodified when `cache_key` is omitted/NULL** (regression guard for
   design decision (b) above).
6. Benchmark against `article-graph`'s own repeated-anonymous-call scenario
   (if one is confirmed to exist) to verify the fix actually helps that
   workload before considering this done — this plan is speculative about
   whether such a workload exists; do not treat "tests pass" alone as
   sufficient completion evidence.

## Files to change

| File | Change |
|---|---|
| `src/catalog/session_cache.h` / `.cpp` | `cache_key` parameter on `get_or_load_graph`; cache lookup/insert under that key when supplied; `anon_cache_invalidate` (or extended `session_cache_invalidate`). |
| SQL function definitions exposing `age_graph`/`node_query`/`edge_query` (`sql/age_gds--1.0.sql`, likely `bfs_stream` at minimum) | Add `cache_key text DEFAULT NULL` parameter. |
| `docs/implemented-plans/07-anonymous-execution.md` | Note the new opt-in caching mode is additive, doesn't change the documented default (this may instead be a note in the new plan's own "moved to implemented" version rather than editing Stage 7's doc — the project convention here isn't settled by precedent; decide at implementation time). |
| Test suite covering Stage 7 (anonymous execution) | Add cache-hit/miss/invalidate cases; confirm default-path regression test still passes. |

## Docs to update

- `docs/reference/session-cache.md` — document `cache_key`/`anon_cache_invalidate` alongside the existing `cache_load`/`cache_stats` entries.
- `docs/architecture.md` — if it has a section distinguishing "anonymous = never cached" as an architectural property, update it to describe the new opt-in exception.
- `docs/implemented-plans/index.md` — add this plan once implemented.

## Open question flagged for the user, not decided here

Is there an actual `article-graph` (or other) caller with a genuine
repeated-identical-anonymous-call pattern that would benefit from this?
The benchmark that motivated this plan measured a single call's cost, not
a repeated-call workload — `article-graph`'s own "Option 1" analysis in
that benchmark leans toward dropping anonymous mode for its filtered-query
use case entirely (routing everything through named-projection Path B
instead), which would make this plan unnecessary for that caller. Confirm
a real use case before investing in the staleness-semantics work above.
