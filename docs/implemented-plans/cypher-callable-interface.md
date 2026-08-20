# Cypher-callable interface (Neo4j GDS parity)

## Problem / Goal

Every `age_gds` function today is callable only as plain SQL
(`SELECT * FROM age_gds.pageRank_stream(...)`), never from inside an AGE
`cypher()` block. Neo4j GDS procedures are called from Cypher itself
(`CALL gds.pageRank.stream(graphName, config) YIELD nodeId, score`). This
plan adds a second, Cypher-callable entry point for every existing
`age_gds` function (and the graph-catalog functions), matching Neo4j's
naming, call shape, and config-key/output-column names as closely as
AGE's Cypher grammar allows, **without changing any existing SQL
function's signature or behavior**.

This grew out of investigating `bfs_stream`'s documented result-shape gap
vs Neo4j's `gds.bfs.stream` (`docs/neo4j-gds-parity-report.md` line 225).
The fix isn't a shape change on the existing function — it's a second,
Cypher-native entry point with the shape natural to that calling
convention. `bfs_stream`'s gap is one instance of a structural gap
(*no age_gds function is Cypher-callable at all*) that applies uniformly
to all of them.

## Hard constraint discovered during design

AGE's Cypher grammar has **no `CALL ... YIELD` clause** (that's Neo4j's
own procedure framework, not part of core openCypher). A Cypher-callable
function can only appear as a **scalar expression** — it takes AGE's
`agtype` arguments and returns one `agtype` value. There is no mechanism
for a single function call to fan out into multiple named output columns.

The closest equivalent to `CALL ... YIELD`:

```cypher
-- Neo4j:
CALL gds.pageRank.stream('proj', {dampingFactor: 0.85})
YIELD nodeId, score
RETURN nodeId, score

-- age_gds, this plan:
UNWIND age_gds.pageRank_stream('proj', {dampingFactor: 0.85}) AS row
RETURN row.nodeId, row.score
```

Row shape and field names match exactly; the only difference is
`row.nodeId` vs bare `nodeId`, forced by AGE's grammar (no `YIELD`).

## Design

### Naming

Neo4j's dotted names (`gds.pageRank.stream`) already got their SQL
translation when the existing functions were built: join the segments
after `gds.` with `_` (`pageRank.stream` → `pageRank_stream`). The new
Cypher-callable entry points reuse that **exact same name** — no second
naming scheme. `age_gds.pageRank_stream` gets a second overload
(`agtype, agtype`) alongside its existing SQL overload
(`text, float8, int, ...`); Postgres resolves which one to call by
argument types/arity, same as any overloaded SQL function. So:
`bfs_stream`, `shortestPath_dijkstra_stream`, `graph_project`, etc. all
gain an `(agtype, agtype)` overload under their current names — nothing
new to invent, nothing to keep consistent across two conventions.

(Caveat to check per-function during implementation: overload resolution
needs the two signatures to be unambiguous. Since almost every existing
function takes far more than 2 SQL params, arity alone disambiguates; the
rare function with exactly 2 SQL params of types that could unify with
`(agtype, agtype)` would need a closer look, but none currently do.)

### Call shape

Every new overload takes exactly two positional arguments, matching
Neo4j's `(graphNameOrConfig, configuration)` pattern:

```
age_gds.<existing_name>(graphName agtype, config agtype DEFAULT NULL) RETURNS agtype
```

- **Named-graph mode**: `graphName` is a string naming a graph already
  registered via `graph_project`/`graph_index`.
- **Anonymous mode**: `graphName` is `NULL`; `config` carries `ageGraph`,
  `nodeQuery`, `edgeQuery` (camelCase renames of the existing
  `age_graph`/`node_query`/`edge_query` SQL params) to project inline.
- All other per-algorithm parameters live inside `config`, keyed by
  Neo4j's **exact** camelCase config names for that procedure (already
  catalogued per-function in `docs/api.md` and
  `docs/neo4j-gds-parity-report.md` — those documents are the source of
  truth for key names during implementation, not re-derived here).
- **Unknown key in `config` → error**, naming the bad key. Matches
  Neo4j's strict config validation.

### Output shape

- `*_stream` wrappers return `agtype` holding a **JSON array**; each
  element is a map using the exact same camelCase field names as the
  existing SQL function's `RETURNS TABLE(...)` columns (which already
  mostly match Neo4j's YIELD columns, e.g. `"nodeId"`, `score`). Consumed
  via `UNWIND ... AS row`.
- `*_stats`/`*_write` wrappers return `agtype` holding a single JSON map
  (same field names as the existing single-row `TABLE(...)` / write-stats
  columns).

### agtype plumbing — no new hand-rolled parsing

Confirmed via the sibling `age_sql` project (`age_sql/src/regexp.c:11-16`):
most of AGE's internal agtype API (iterators, structured field access,
`_agtype_build_*`) is **not exported from `age.so`**, or breaks outside a
real SQL call context. Only `agtype_in`/`agtype_out` are safe to call
directly (they must stay stable — Postgres's own fmgr dispatch depends on
them). `age_sql` already uses exactly this pair via `DirectFunctionCall`,
treating agtype content as JSON text.

We go one step further and avoid writing a JSON parser at all:
`src/util/config_parser.h` already uses Postgres's **core** `utils/jsonb.h`
API (`Jsonb`, `JsonbIterator`, `pushJsonbValue`, etc. — not AGE, not a new
dependency) to parse PageRank's existing `config jsonb` SQL parameter.
Reuse that same core-jsonb machinery for every wrapper:

- **Read config**: `agtype_out(config)` → text → `jsonb_in(text)` (all via
  `DirectFunctionCall`, all core/AGE-stable functions) → walk the
  resulting `Jsonb*` with Postgres's `JsonbIterator` API to pull typed
  fields, erroring on unrecognized keys.
- **Build result**: construct a `JsonbValue`/`JsonbParseState` tree with
  `pushJsonbValue` (array of maps, or a single map) → `JsonbToCString` →
  text → `agtype_in(text)` → return.

New shared module: `src/util/cypher_agtype.{h,cpp}` — the `agtype_in`/
`agtype_out`/jsonb glue, plus a config-key-validation helper (given a
known key set, error on anything else). First implementation step should
audit `config_parser.h` for what's directly reusable vs needs
generalizing, rather than assuming.

### Build

- Link `age_gds.so` against `age.so`; add `#include "utils/agtype.h"`.
- `sql/age_gds--1.0.sql` needs the same `LOAD '$libdir/age';` line
  `age_sql--1.0.sql` uses before its `CREATE FUNCTION` statements, so
  symbol validation against `age.so` succeeds at `CREATE FUNCTION` time.
- `CMakeLists.txt`: add AGE's library to `target_link_libraries`, same
  section as the existing NetworKit/OpenMP links.

### Implementation pattern per function

Extract each function's existing "core" — everything after argument
resolution, before the SRF emit loop — into a shared internal function
returning an in-memory result. Two thin wrappers call it:

- the existing SQL SRF entry point (unchanged behavior/signature, now
  delegating to the extracted core instead of duplicating it)
- the new agtype entry point: parse `config` → call core → pack result →
  `agtype`

No SPI round-trips between the two wrappers; both call the same C++
function directly.

### `bfs_stream` — the case that started this

`age_gds.bfs_stream(graphName agtype, config agtype)` returns a JSON array
with **one** element: `{sourceNode, nodeIds}`.

- **No `path` field.** Neo4j's `path` is a linear chain over `nodeIds`
  using fabricated, non-graph `NEXT` relationships — established during
  design discussion that it carries zero information beyond `nodeIds`
  itself (no real edge, no type, no properties). Building it would also
  require AGE's private, unexported vertex/edge/path builder internals.
  This is a **deliberate, documented discrepancy**: `docs/api.md`'s
  `bfs_stream` entry must state that `path` is omitted and why.
- `config.maxDepth` — as today's `max_depth`.
- `config.targetNodes` — matches Neo4j's real semantics (confirmed
  against `docs/neo4j-gds-parity-report.md:233-235`): early termination
  once all targets are visited, `nodeIds` is the (possibly reduced)
  visited-node list — **not** the existing SQL `bfs_stream`'s
  per-target-distance output. That per-target-distance behavior is a
  distinct feature with no Neo4j equivalent; it stays SQL-only, unchanged,
  under `age_gds.bfs_stream`'s existing `target_nodes` param.

### `graph_project` — label/property projection

Neo4j's `gds.graph.project(graphName, nodeProjection, relationshipProjection,
configuration)` projects by node-label/relationship-type and property
filters — a different mechanism from our Cypher-query-based projection
(`node_query`/`edge_query` MATCH text). To match Neo4j's semantics for
real (not just its call shape), `nodeProjection`/`relationshipProjection`
config objects are **translated into generated `node_query`/`edge_query`
text internally**, then executed through the existing query-based
projection engine — e.g. `nodeProjection: {Person: {}}` compiles to
`MATCH (n:Person) RETURN id(n) AS id`. `nodeProjection`/
`relationshipProjection: '*'` (wildcard, all labels) compiles to today's
default all-nodes/all-edges query. Property-list filtering (Neo4j's
`properties: [...]` inside a projection entry) is out of scope for the
first pass — flag as a follow-up if Neo4j's `properties` key is supplied
(error naming it unsupported, rather than silently ignoring).

Other catalog functions (`graph_drop`, `graph_list`, `graph_exists`) are
straightforward 1:1 wrappers — single scalar/void/list results, no
projection-translation complexity.

## Implementation order

1. **Shared infrastructure**: `age.so` link + `utils/agtype.h` include in
   `CMakeLists.txt`; `LOAD '$libdir/age';` in the SQL script;
   `src/util/cypher_agtype.{h,cpp}` (agtype↔jsonb↔text glue, config-key
   validator, result-array/map builder). Prove it end-to-end with one
   trivial function first (`age_gds.version` has no args/config — instead
   prove the round-trip with the smallest real algorithm, `bridges_stream`
   or `hits_stream`, both parameterless besides graph selection).
2. **Path-finding family** (where this started): `bfs_stream`,
   `shortestPath_dijkstra_stream`, `allShortestPaths_dijkstra_stream`,
   `deltaStepping_stream`, `bellmanFord_stream`/`bellmanFord_stats`, `dfs_stream`,
   `randomWalk_stream`, `spanningTree_stream`, `allShortestPaths_stream`,
   `maxFlow_stats`, `shortestPath_astar_stream`, `minCostMaxFlow_stats`,
   `topologicalSort_stream`, `longestPath_stream`, `bridges_stream`.
3. **Centrality family**: `pageRank_stream`/`pageRank_stats`/`pageRank_write`,
   `betweenness_stream`/`betweenness_write`, `closeness_stream`,
   `harmonicCentrality_stream`, `degree_stream`, `eigenvector_stream`,
   `hits_stream`, `articleRank_stream`, `articulationPoints_stream`.
4. **Community family**: `louvain_stream`, `leiden_stream`,
   `labelPropagation_stream`, `wcc_stream`, `scc_stream`, `kCore_stream`,
   `triangleCount_stream`, `localClusteringCoefficient_stream`,
   `modularity_stats`, `conductance_stats`, `cliqueCounting_stats`.
5. **Similarity / link prediction / embeddings**: `nodeSimilarity_stream`,
   `knn_stream`, `linkprediction_*_stream` (4: adamicAdar,
   commonNeighbors, preferentialAttachment, resourceAllocation), the
   single-pair `linkprediction_*` scalar functions (4 — already scalar,
   so these just need agtype-argument overloads, no array/map packing),
   `node2vec_stream`.
6. **Catalog**: `graph_project` (incl. projection translation),
   `graph_drop`, `graph_list`, `graph_exists`, `graph_reindex`, `cache_stats`,
   `cache_load`, `graph_is_stale`.

Each phase lands with its own test coverage (new `test/sql/*.sql` cases
calling the wrappers from inside `cypher()`) before moving to the next.

## Files to change

| File | Change |
|---|---|
| `CMakeLists.txt` | Link `age.so`; AGE include path already assumed per project deps table |
| `sql/age_gds--1.0.sql` | Add `LOAD '$libdir/age';`; add one `CREATE FUNCTION` per new wrapper |
| `src/util/cypher_agtype.h/.cpp` | New: agtype↔jsonb↔text glue, config validation, result packing |
| `src/util/config_parser.h/.cpp` | Audit for reuse/generalization by the new config-parsing helper |
| `src/algorithms/*.cpp` (all) | Extract core logic per function into a shared internal call, add agtype entry points |
| `src/algorithms/pathfinding.cpp` | `bfs_stream`'s `targetNodes` early-termination semantics (distinct from SQL `bfs_stream`'s `target_nodes`) |
| `src/catalog/*.cpp` | `graph_project`'s projection-translation logic; other catalog wrappers |
| `test/sql/*.sql` | New Cypher-call test cases per phase |

## Docs to update

- `docs/api.md` — every function gets its Cypher-callable signature
  documented alongside the existing SQL signature; `bfs_stream` explicitly
  documents the omitted `path` field and why.
- `docs/architecture.md` — new section on the Cypher-callable calling
  convention, the `agtype_in`/`agtype_out`-only plumbing rule (and why
  nothing deeper is used), and the `UNWIND`-instead-of-`YIELD` pattern.
- `docs/neo4j-gds-parity-report.md` — close the `bfs_stream` result-shape
  item (now solved by the Cypher entry point, not a shape change on the
  SQL one); note the new Cypher-callable interface as closing the
  broader "no CALL/YIELD support" gap.
- `docs/user-guide.md` — add Cypher-call examples (`UNWIND ... AS row`)
  alongside the existing SQL examples.
