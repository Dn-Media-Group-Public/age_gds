# Plan: expose an `agtype` → `jsonb` SQL interop function

## Problem / Goal

`article-graph` (a downstream consumer of `age_gds`/AGE, not this repo)
profiled its query pipeline and found that fetching node/edge `properties`
(a native AGE `agtype` column, confirmed via
`information_schema.columns` — not `jsonb`) is expensive on the Python
side: since `psycopg2` has no adapter for `agtype` and this Postgres has no
`agtype→jsonb` cast registered, every caller must `SELECT properties::text`
and then manually strip an AGE-appended type suffix (`::vertex`/`::edge`/
`::path`, via regex) and `json.loads` the result — in one profiled query
this Python-side parsing was ~34% of total request time (33,540 calls to
their `_parse_props` helper on a single request). See
`query-pipeline-benchmark.md` (in the `article-graph` repo) for the full
profiling writeup — flagging it here since it named `age_gds` as a
plausible place to fix this, given `age_gds` already does a comparable
conversion internally.

**This is a speculative plan, not a confirmed requirement** — it
originated from profiling one specific caller's one specific query
pipeline, not from a demonstrated recurring need across multiple `age_gds`
consumers. Before implementing, confirm this is actually worth doing (see
"Open questions" below) — the `article-graph`-side plan
(`docs/plans/merge-query-round-trips.md` in that repo) covers a
Python/SQL-side-only fix (fewer round trips) that doesn't depend on this at
all, so this isn't blocking anything on that side.

## Why `age_gds` already has (most of) the machinery for this

`src/util/cypher_agtype.cpp`/`.h` (`docs/architecture.md`'s "agtype ↔ jsonb
↔ text plumbing" section) already implements exactly this conversion
today, internally, for the Cypher-callable config-parsing path:

> `agtype_out(arg)` → text → `jsonb_in(text)` (all via `DirectFunctionCall`,
> all core/AGE-stable functions) → walk the resulting `Jsonb*` with
> `JsonbIterator`/`findJsonbValueFromContainer`...

This exists precisely because AGE doesn't export its internal `agtype`
iterator/builder API from `age.so` — only `agtype_in`/`agtype_out` are
stable, exported entry points (confirmed via the sibling `age_sql` project's
source, per the same architecture doc section). So the conversion still
goes through a text intermediate (`agtype_out` → text → `jsonb_in`) even
inside `age_gds` — there's no way to avoid that hop, AGE gives no other
option. The proposed win here isn't "faster text parsing," it's **moving
that same text→jsonb step from Python (`article-graph`'s process, via a
network round trip's worth of already-fetched text plus a Python-level
regex + `json.loads`) into Postgres itself (C, via `jsonb_in`, executed
server-side)** — and, critically, once the column comes back from Postgres
as a real `jsonb` value instead of `text`, `psycopg2`'s existing built-in
JSON/JSONB adapter decodes it automatically on the client side with no
caller code needed at all (no manual `json.loads`, no regex) — this is
where the actual savings would come from, not a faster parser per se.

## Design

### Expose as a plain SQL function + an explicit CAST, not a Cypher overload

```sql
CREATE FUNCTION age_gds.agtype_to_jsonb(a agtype) RETURNS jsonb
    AS 'age_gds', 'age_gds_agtype_to_jsonb' LANGUAGE C IMMUTABLE STRICT;

CREATE CAST (ag_catalog.agtype AS jsonb)
    WITH FUNCTION age_gds.agtype_to_jsonb(agtype);
    -- explicit-only cast (no AS IMPLICIT/AS ASSIGNMENT) — safest default,
    -- still satisfies `properties::jsonb` call sites (explicit `::` cast
    -- syntax works with any registered cast, implicit or not); revisit if
    -- a consumer needs it to fire in an implicit-cast context (e.g. a
    -- function call or comparison without a literal `::jsonb`).
```

**No Cypher-callable `(agtype, agtype)` overload** — per the existing
precedent for `age_gds.version` (`docs/reference/utility.md`: "No
Cypher-callable overload... deliberately left SQL-only" with a stated
reason), `jsonb` has no representation in Cypher's type system at all
(Cypher property values are natively `agtype` already — a Cypher caller
never needs this conversion, it would only ever be useful to a plain-SQL
caller reading AGE's underlying tables directly, which is exactly
`article-graph`'s access pattern). Document this explicitly in the
reference doc entry, same style as `age_gds.version`'s.

### Neo4j GDS parity: not applicable, and shouldn't be framed as if it were

This is **not** a graph algorithm and has no Neo4j GDS analog to track
parity against (per `docs/neo4j-gds-parity-report.md`'s mission — Neo4j
doesn't run on Postgres and has no `jsonb` concept). Recommend:
documenting it under `docs/reference/utility.md` (where `age_gds.version`
already lives) rather than alongside the algorithm reference docs
(`centrality.md`, `path-finding.md`, etc.), and not naming it in any
`*_stream`/`*_stats`/`*_write` style that would imply it's part of the
algorithm surface.

### Scope-fit question — is this `age_gds`'s job at all?

`age_gds`'s stated identity (`CLAUDE.md`): "a thin glue layer between
Apache AGE... and NetworKit... not a general graph-data-science library."
A generic `agtype`↔`jsonb` interop cast is arguably *outside* even that
narrower charter — it's not projecting a graph or running an algorithm, and
it isn't really about NetworKit at all. Two ways to read this:

1. **In scope, as plumbing**: `age_gds` already ships exactly this
   conversion logic internally (`cypher_agtype.cpp`); exposing it as a
   public SQL function is a small, low-risk surfacing of existing code, not
   new conceptual scope — similar in spirit to `age_gds.version` already
   being a non-algorithm utility function this project ships.
2. **Out of scope — belongs upstream in Apache AGE itself**: `agtype` is
   AGE's type, not `age_gds`'s; a general-purpose `agtype→jsonb` cast is a
   generic AGE interop concern that has nothing to do with graph algorithms
   or NetworKit, and arguably every AGE user (not just `age_gds`/NetworKit
   users) would benefit from AGE itself shipping this cast. If AGE's
   upstream (or this project's vendored/forked AGE, if any) is realistically
   patchable, that's the more natural home and avoids `age_gds` taking on
   an interop-utility identity beyond its stated charter.

**Recommend surfacing this choice to whoever owns `age_gds`'s scope
decisions before implementing** — this plan is written assuming option 1
(expose it here) since it reuses existing code with minimal new surface
area, but option 2 is the more architecturally "correct" home if patching
AGE upstream is practical. If AGE upstream isn't realistically patchable in
this project's timeframe, option 1 is the pragmatic fallback.

## Implementation order

1. Get a decision on the scope-fit question above.
2. If proceeding in `age_gds`: add `age_gds_agtype_to_jsonb` (C++, in a new
   or existing `src/util/` file — `cypher_agtype.cpp` is likely the right
   home given it already has the conversion logic, or extract a shared
   helper both that file and the new function call), following the exact
   `agtype_out` → text → `jsonb_in` pattern already documented in
   `docs/architecture.md`.
3. Add the SQL function declaration + `CREATE CAST` to
   `sql/age_gds--1.0.sql`.
4. Add a `docs/reference/utility.md` entry, modeled on `age_gds.version`'s
   entry (including the explicit "no Cypher overload, here's why" note).
5. Tests: a SQL-level test casting a representative `agtype` value
   (including a nested map/array, to confirm `jsonb_in` round-trips
   correctly, not just flat scalars) to `jsonb` and checking field access
   works (`(properties::jsonb)->>'name'` or similar). Confirm `NULL` input
   behaves per `STRICT` (returns `NULL`, doesn't error).
6. Report back to whoever raised this (the `article-graph` session) with
   the resulting cast so they can validate the actual end-to-end win
   (`properties::jsonb` instead of `properties::text` + Python parsing) on
   their own benchmark before committing to using it broadly.

## Files to change

| File | Change |
|---|---|
| `src/util/cypher_agtype.cpp` (or a new file) | New `age_gds_agtype_to_jsonb` C function, reusing the existing `agtype_out`→`jsonb_in` pattern. |
| `sql/age_gds--1.0.sql` | `CREATE FUNCTION age_gds.agtype_to_jsonb`; `CREATE CAST (agtype AS jsonb)`. |
| `docs/reference/utility.md` | New entry, modeled on `age_gds.version`'s (explicit no-Cypher-overload note). |
| `test/sql/*` (wherever utility-function tests live) | New test: cast a nested-map/array `agtype` value to `jsonb`, verify structure; verify `NULL` handling. |

## Docs to update

- `docs/reference/utility.md` — new entry (see above).
- `docs/architecture.md`'s "agtype ↔ jsonb ↔ text plumbing" section — note
  this conversion is now also exposed as a public cast, not just used
  internally for Cypher-callable config parsing.
- `docs/implemented-plans/index.md` — add this plan once implemented.

## Open questions (resolve before implementing)

1. Scope-fit: `age_gds` vs. upstream Apache AGE (see "Scope-fit question"
   above) — needs a decision from whoever owns this project's scope
   boundaries.
2. Is there a second, independent `age_gds` consumer who'd actually use
   this, or is `article-graph`'s one profiled query pipeline the only
   motivating case so far? If it's the latter, consider whether
   `article-graph`'s own Phase-1/Phase-3 round-trip merging
   (`docs/plans/merge-query-round-trips.md` in that repo) plus a
   Python-side `orjson` swap (also suggested in that session) closes
   enough of the gap on its own to make this unnecessary for now.
3. Should the cast be `AS ASSIGNMENT` (fires in more implicit contexts,
   e.g. `INSERT`/`UPDATE` target-type coercion) rather than fully explicit
   -only? Explicit-only is the safer default (matches "Design" above); only
   widen if a concrete need for implicit coercion shows up.
