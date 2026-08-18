# Plan: Per-call relationship weight plumbing (foundation)

## Problem / Goal

Nearly every gap in `docs/neo4j-gds-parity-report.md` tagged with
`relationshipWeightProperty ✅ closable` needs the same capability: let a SQL
call name an AGE edge property to use as the graph weight for *that call*,
independent of how the graph was originally projected.

Today weight is decided once, at `age_load_graph()` time, via a single
`bool weighted` flag (`src/bridge/age_loader.h`). For anonymous execution
(caller supplies `edge_query` directly), the caller can already put any
property expression in the query's `weight` column — so `relationshipWeightProperty`
is *already* satisfiable there with zero code changes, as long as the
algorithm's SQL wrapper passes `weighted := true` through to `age_load_graph`.
For the **named/cached graph** path (`graph_name` resolves to a
`session_cache` entry deserialized from a `.bin` file), the topology was
projected once and cached — there is no per-call opportunity to change which
property backs the edge weight without re-running the full projection.

This plan builds the one shared mechanism every other plan's
`relationshipWeightProperty` items depend on, so it isn't reimplemented five
times.

**Why:** avoid duplicating "read a named edge property and apply it as
weight" logic across centrality, community, path-finding, similarity, and
embedding wrappers.
**How to apply:** every other plan in this directory that references
`relationshipWeightProperty` assumes this phase is done first.

## Design

- Add `std::optional<std::string> weight_property` as an additional parameter
  threaded through the algorithm SQL wrapper layer (not `age_load_graph`
  itself — see below).
- **Anonymous execution path:** no change needed. Document in `docs/api.md`
  that `weight_property`/`relationship_weight_property` is satisfied by
  writing the desired property into the `edge_query`'s `weight` column
  directly; wrappers should pass `weighted := (weight_property IS NOT NULL OR
  <existing weighted flag>)` to `age_load_graph`.
- **Named/cached graph path:** add a helper
  `reweight_from_property(NetworKit::Graph &g, const NodeIdMap &id_map,
  const char *age_graph_name, const char *edge_query, const char
  *weight_property)` in `src/bridge/age_loader.cpp`/`.h` that:
  1. Re-runs the *existing* `edge_query` via SPI (same query used at
     projection time, stored in the `age_gds.graphs` catalog row — confirm
     column exists, add if missing), but selects `weight_property` instead of
     whatever the original query used for its `weight` column.
  2. For each `(src, dst)` pair resolvable through the cached `NodeIdMap`,
     calls `Graph::setWeight(src_nk, dst_nk, w)` on a *copy* of the cached
     graph (never mutate the shared cached `NetworKit::Graph` in place —
     other backends/cache entries must not see this call's override).
  3. Returns the reweighted copy for the algorithm to run against.
- If `age_gds.graphs` does not already store the original `edge_query` text,
  add that column (`ALTER TABLE age_gds.graphs ADD COLUMN edge_query text`)
  and populate it in `graph_project`/`graph_index`. Check
  `src/catalog/graph_project.cpp` and `session_cache.h` first — a plain
  `Read` of these files at implementation time will confirm whether this
  already exists before adding it.
- Copying the graph for reweighting is O(V+E) — acceptable, matches the cost
  every other per-call variant already pays (e.g. `wcc_stream`'s `threshold`
  filter needs an edge-filtered copy too, see `docs/plans/community-detection.md`).

## Implementation order

1. Confirm whether `age_gds.graphs` stores `edge_query`; add the column if
   not (catalog migration + `graph_project`/`graph_index` write path).
2. Add `reweight_from_property()` to `age_loader.h`/`.cpp`.
3. Add a small shared helper in `src/util/` (e.g. `weight_override.h`/`.cpp`)
   that, given `(graph_handle, weight_property)`, returns either the cached
   graph unchanged (`weight_property` is null) or a reweighted copy — this is
   the single call site every dependent plan's wrappers will use.
4. Unit-test via one `test/sql/` case: project a graph, call an algorithm
   with a `weight_property` override that differs from the projection-time
   weight, confirm the result changes accordingly.

## Files to change

| File | Change |
|---|---|
| `src/catalog/graph_project.cpp` | Store `edge_query` on the catalog row if not already stored |
| `src/catalog/session_cache.h`/`.cpp` | Expose stored `edge_query` alongside the cached graph |
| `src/bridge/age_loader.h`/`.cpp` | Add `reweight_from_property()` |
| `src/util/weight_override.h`/`.cpp` (new) | Shared `apply_weight_override(handle, weight_property)` helper |
| `test/sql/` | New test case for weight override on a named graph |

## Docs to update

- `docs/architecture.md`: document the weight-override mechanism under a new
  "Per-call weight override" subsection.
- `docs/api.md`: document `weight_property`/`relationship_weight_property`
  as a common parameter convention (in "Common parameter conventions"),
  referencing this plan.
