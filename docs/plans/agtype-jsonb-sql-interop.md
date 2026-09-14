# Plan: expose `agtype` ↔ `jsonb` SQL interop functions

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

This originated from profiling one specific caller's (`article-graph`'s)
one specific query pipeline — there is no second, independent `age_gds`
consumer confirmed yet. **Decided to proceed anyway**: the design below is
generic (plain SQL functions/casts usable by any consumer reading or
writing AGE tables, nothing article-graph-specific baked in), so it's
worth shipping as reusable plumbing even with one motivating case so far.
Note the `article-graph`-side plan (`docs/plans/merge-query-round-trips.md`
in that repo) covers a Python/SQL-side-only fix (fewer round trips) that
doesn't depend on this at all, so this isn't blocking anything on that
side.

## Why `age_gds` already has (most of) the machinery for this

`src/util/cypher_agtype.cpp`/`.h` (`docs/architecture.md`'s "agtype ↔ jsonb
↔ text plumbing" section) already implements both conversion directions
today, internally, for the Cypher-callable config-parsing path:

> - **Read config**: `agtype_out(arg)` → text → `jsonb_in(text)` (all via
>   `DirectFunctionCall`, all core/AGE-stable functions) → walk the
>   resulting `Jsonb*` with `JsonbIterator`/`findJsonbValueFromContainer`...
> - **Return a result**: build a `Jsonb*` → `JsonbToCString` → text →
>   `agtype_in` → return.

This exists precisely because AGE doesn't export its internal `agtype`
iterator/builder API from `age.so` — only `agtype_in`/`agtype_out` are
stable, exported entry points (confirmed via the sibling `age_sql` project's
source, per the same architecture doc section; `cypher_agtype.cpp` already
calls both `agtype_in` at line 16 and `agtype_out` at line 21). So each
conversion direction goes through a text intermediate even inside
`age_gds` — there's no way to avoid that hop, AGE gives no other option.
The proposed win here isn't "faster text parsing," it's **moving that same
text↔jsonb step from Python (`article-graph`'s process, via a network
round trip's worth of already-fetched text plus a Python-level regex +
`json.loads`) into Postgres itself (C, via `jsonb_in`/`agtype_in`, executed
server-side)** — and, critically, once the column comes back from Postgres
as a real `jsonb` value instead of `text`, `psycopg2`'s existing built-in
JSON/JSONB adapter decodes it automatically on the client side with no
caller code needed at all (no manual `json.loads`, no regex) — this is
where the actual savings would come from, not a faster parser per se.

The reverse direction (`jsonb` → `agtype`) is motivated by symmetry and by
write-path callers: a consumer that builds a `jsonb` value client-side
(e.g. via `psycopg2`'s automatic JSON encoding) and wants to write it into
an AGE property (an `agtype` column) currently has no registered cast and
would have to go through the same manual text-round-trip dance in reverse.
**Decided to include it** even though no write-path consumer is profiled
yet — kept for generality/symmetry with the read-path direction, consistent
with not building something that only fits a single known use case.

## Design

### Expose as plain SQL functions + explicit CASTs, not Cypher overloads

Both directions, following the same shape:

```sql
CREATE FUNCTION age_gds.agtype_to_jsonb(a agtype) RETURNS jsonb
    AS 'age_gds', 'age_gds_agtype_to_jsonb' LANGUAGE C IMMUTABLE STRICT;

CREATE CAST (ag_catalog.agtype AS jsonb)
    WITH FUNCTION age_gds.agtype_to_jsonb(agtype);
    -- explicit-only cast (no AS IMPLICIT/AS ASSIGNMENT) — decided: still
    -- satisfies `properties::jsonb` call sites (explicit `::` cast syntax
    -- works with any registered cast, implicit or not); widen later only
    -- if a concrete need for implicit coercion shows up.

CREATE FUNCTION age_gds.jsonb_to_agtype(j jsonb) RETURNS agtype
    AS 'age_gds', 'age_gds_jsonb_to_agtype' LANGUAGE C IMMUTABLE STRICT;

CREATE CAST (jsonb AS ag_catalog.agtype)
    WITH FUNCTION age_gds.jsonb_to_agtype(jsonb);
    -- also explicit-only — decided, same reason as the forward cast; no
    -- known caller needs implicit coercion in either direction.
```

Registering both directions as separate explicit casts is safe: Postgres
does not treat a pair of one-way `CREATE CAST`s as implying anything about
the other direction, and neither is `AS IMPLICIT`, so there's no risk of
this pair being picked up in ambiguous/implicit contexts (e.g. function
overload resolution) that the single forward cast wasn't already exposed
to.

**No Cypher-callable overloads for either direction** — per the existing
precedent for `age_gds.version` (`docs/reference/utility.md`: "No
Cypher-callable overload... deliberately left SQL-only" with a stated
reason), `jsonb` has no representation in Cypher's type system at all
(Cypher property values are natively `agtype` already — a Cypher caller
never needs either conversion, it would only ever be useful to a plain-SQL
caller reading or writing AGE's underlying tables directly, which is
exactly `article-graph`'s access pattern). Document this explicitly in the
reference doc entry for both functions, same style as `age_gds.version`'s.

### Neo4j GDS parity: not applicable, and shouldn't be framed as if it were

This is **not** a graph algorithm and has no Neo4j GDS analog to track
parity against (per `docs/neo4j-gds-parity-report.md`'s mission — Neo4j
doesn't run on Postgres and has no `jsonb` concept). Recommend:
documenting it under `docs/reference/utility.md` (where `age_gds.version`
already lives) rather than alongside the algorithm reference docs
(`centrality.md`, `path-finding.md`, etc.), and not naming it in any
`*_stream`/`*_stats`/`*_write` style that would imply it's part of the
algorithm surface.

### Scope-fit: decided in `age_gds`

`age_gds`'s stated identity (`CLAUDE.md`): "a thin glue layer between
Apache AGE... and NetworKit... not a general graph-data-science library."
A generic `agtype`↔`jsonb` interop cast is arguably *outside* even that
narrower charter — it's not projecting a graph or running an algorithm, and
it isn't really about NetworKit at all. The alternative would have been
building this upstream in Apache AGE itself (since `agtype` is AGE's type,
not `age_gds`'s, and every AGE user — not just `age_gds`/NetworKit users —
would benefit from AGE shipping this cast).

**Decided: implement in `age_gds`.** `age_gds` already ships exactly this
conversion logic internally (`cypher_agtype.cpp`); exposing it as public
SQL functions is a small, low-risk surfacing of existing code, not new
conceptual scope — similar in spirit to `age_gds.version` already being a
non-algorithm utility function this project ships.

## Implementation order

1. Add `age_gds_agtype_to_jsonb` and
   `age_gds_jsonb_to_agtype` (C++, in a new or existing `src/util/` file —
   `cypher_agtype.cpp` is likely the right home given it already has both
   conversion directions' logic, or extract a shared helper both that file
   and the new functions call), following the exact `agtype_out` → text →
   `jsonb_in` (and, for the reverse, `jsonb_out` → text → `agtype_in`)
   patterns already documented in `docs/architecture.md`.
2. Add both SQL function declarations + both `CREATE CAST` statements to
   `sql/age_gds--1.0.sql`.
3. Add `docs/reference/utility.md` entries for both functions, modeled on
   `age_gds.version`'s entry (including the explicit "no Cypher overload,
   here's why" note).
4. Tests: SQL-level tests casting a representative value in each direction
   (including a nested map/array, to confirm the intermediate JSON
   round-trips correctly, not just flat scalars) and checking field access
   works after the forward cast (`(properties::jsonb)->>'name'` or
   similar) and that a round-trip (`agtype → jsonb → agtype`) is
   lossless for a representative value. Confirm `NULL` input behaves per
   `STRICT` (returns `NULL`, doesn't error) for both functions.
5. Report back to whoever raised this (the `article-graph` session) with
   the resulting casts so they can validate the actual end-to-end win
   (`properties::jsonb` instead of `properties::text` + Python parsing, and
   any write-path use of the reverse cast) on their own benchmark before
   committing to using it broadly.

## Files to change

| File | Change |
|---|---|
| `src/util/cypher_agtype.cpp` (or a new file) | New `age_gds_agtype_to_jsonb` and `age_gds_jsonb_to_agtype` C functions, reusing the existing `agtype_out`→`jsonb_in` and `jsonb_out`→`agtype_in` patterns. |
| `sql/age_gds--1.0.sql` | `CREATE FUNCTION age_gds.agtype_to_jsonb` / `age_gds.jsonb_to_agtype`; `CREATE CAST (agtype AS jsonb)` / `CREATE CAST (jsonb AS agtype)`. |
| `docs/reference/utility.md` | New entries for both functions, modeled on `age_gds.version`'s (explicit no-Cypher-overload note). |
| `test/sql/*` (wherever utility-function tests live) | New tests: cast a nested-map/array value in each direction, verify structure; verify a round-trip is lossless; verify `NULL` handling for both. |

## Docs to update

- `docs/reference/utility.md` — new entries for both directions (see above).
- `docs/architecture.md`'s "agtype ↔ jsonb ↔ text plumbing" section — note
  this conversion (both directions) is now also exposed as public casts,
  not just used internally for Cypher-callable config parsing.
- `docs/implemented-plans/index.md` — add this plan once implemented.

## Decisions

All previously-open questions were discussed and resolved before
implementation:

1. **Scope-fit**: implement in `age_gds`, not upstream Apache AGE (see
   "Scope-fit" above).
2. **Second consumer**: `article-graph`'s one profiled query pipeline is
   the only motivating case so far, and that's fine — proceeding on it
   alone, since the design is generic (plain reusable SQL functions/casts,
   nothing article-graph-specific), not something that only fits a single
   use case.
3. **Cast strictness**: explicit-only for both directions (see "Design"
   above); widen to `AS ASSIGNMENT` later only if a concrete need for
   implicit coercion shows up.
4. **Reverse direction (`jsonb → agtype`)**: keep it, for generality/
   symmetry with the read-path direction, even though no write-path
   consumer is profiled yet.
