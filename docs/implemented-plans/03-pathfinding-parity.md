# Plan: Path-finding & flow algorithm parity fixes + new procedures

## Problem / Goal

Close the ✅/⚠️-closable gaps in `docs/neo4j-gds-parity-report.md` §3
(Path-finding), plus add the two closable brand-new flow/path procedures from
§7 that belong in this family (A* shortest path, min-cost max flow). Depends
on `docs/plans/00-weight-plumbing.md`.

Excluded (❌, out of scope): `deltaStepping_stream`'s `delta` param (no
NetworKit delta-stepping class — stays a documented Dijkstra substitution),
`maxFlow`/`minCostMaxFlow`'s `alpha` cost-scaling param (not exposed by
`SuccessiveShortestPathMinCostFlow`), Yen's k-shortest-paths and k-spanning
tree (no NetworKit class — tracked in `docs/neo4j-gds-parity-external-libs.md`,
not this plan).

## Design

1. **`bfs_stream`**: add `max_depth integer DEFAULT -1` (one-line guard in
   the existing `Traversal::BFSfrom` handle: stop enqueueing once `dist >
   max_depth`, unless `-1`). Add `target_nodes bigint[] DEFAULT NULL` — when
   given, switch to `NetworKit::MultiTargetBFS(G, source, targetsFirst,
   targetsLast)` instead of the current traversal, terminating once all
   targets are found. **Result shape**: keep current per-node/`distance`
   rows (do not adopt Neo4j's single-row path+nodeIds shape — that's a
   different feature, see `shortestPath_dijkstra_stream` below for where
   path output actually gets added).
2. **`dfs_stream`**: add `max_depth integer DEFAULT -1` (guard on the depth
   already tracked for the `depth` column) and `target_nodes bigint[]
   DEFAULT NULL` (loop-exit condition: stop traversal once current node is in
   target set — no dedicated NetworKit class needed, this is control flow
   over the traversal already implemented).
3. **`shortestPath_dijkstra_stream`**: this is the biggest structural change
   in the plan. Split into two functions matching Neo4j's actual split:
   - `shortestPath_dijkstra_stream(graph_name, source_node, target_node,
     relationship_weight_property, ...)` — **new** single-pair function using
     `NetworKit::Dijkstra(G, source, storePaths=true,
     storeNodesSortedByDistance=false, target)`, returning
     `(total_cost float8, node_ids bigint[], costs float8[])` via
     `SSSP::getPath()`/inherited path reconstruction. Replaces the current
     function's signature — **breaking change**, per user decision to modify
     in place rather than add a parallel `_v2` function.
   - `allShortestPaths_dijkstra_stream(graph_name, source_node,
     relationship_weight_property, ...)` — **new** function, single-source-to-all,
     matching Neo4j's `gds.allShortestPaths.dijkstra.stream`, also returning
     path output (`getPaths()` for all reachable targets) instead of
     distance-only.
   - For the list-of-targets case (`target_nodes bigint[]`), use
     `NetworKit::MultiTargetDijkstra` instead of looping single-target calls.
4. **`bellmanFord_stream`**: stop substituting `Dijkstra`; swap the backing
   algorithm to `NetworKit::AlgebraicBellmanFord`. Add
   `is_negative_cycle boolean` to per-path result rows and a
   `contains_negative_cycle boolean` to a `bellmanFord_stats` companion (add
   if it doesn't exist — check `src/algorithms/pathfinding.cpp` first),
   sourced directly from `AlgebraicBellmanFord::run()`'s return value. Add
   `relationship_weight_property`.
5. **`randomWalk_stream`**: change `source_node` to `source_nodes bigint[]
   DEFAULT NULL` (NULL = all nodes, matching Neo4j's default — **breaking
   change** to the parameter, per user decision). Change `walk_length`
   default from `10` to `80` to match Neo4j. Add `walks_per_node integer
   DEFAULT 10`. Delegate to the same `NetworKit::BiasedRandomWalk`/`Node2Vec`
   walker already used by `node2vec_stream` (instead of uniform
   `Aux::Random::choice`) to pick up `in_out_factor`/`return_factor`
   (`Q`/`P`) parameters for free — add both with `DEFAULT 1.0`. Add
   `relationship_weight_property` (verify `BiasedRandomWalk` respects graph
   weights during implementation — if it doesn't, drop this one param and
   document the gap rather than faking it).
6. **`spanningTree_stream`**: add `objective text DEFAULT 'minimum'`
   (`'minimum'|'maximum'`) — `'maximum'` uses
   `NetworKit::RandomMaximumSpanningForest(G)` directly; `'minimum'` runs the
   same class on negated edge weights (standard MST trick — build an
   `edgeValues` vector of `-weight` and pass to the constructor overload that
   accepts explicit edge values). Add `relationship_weight_property`. Add
   `source_node bigint DEFAULT NULL` as a **validation-only** parameter (if
   given, error if the node doesn't exist in the graph; document that
   NetworKit's spanning-forest result is root-independent, so this does not
   change which edges are returned — Neo4j's "rooted" semantics aren't a
   real algorithmic constraint here).
7. **`allShortestPaths_stream`**: add `relationship_weight_property` only —
   no structural gap otherwise.
8. **`maxFlow_stats`**: add `capacity_property text` (replaces reliance on
   graph's generic weight — read named edge property as capacity via the
   weight-override mechanism from plan 00, reusing the same "named property →
   edge value" plumbing), `source_nodes bigint[]`/`target_nodes bigint[]`
   (multi-source/multi-sink via standard super-source/super-sink graph
   augmentation: add one virtual node connected to every requested source
   with `infinity`-large capacity edges, one virtual sink connected from
   every target the same way, run existing `EdmondsKarp`/`Dinic` unchanged on
   the augmented graph, then discard the virtual nodes from output).
   `node_capacity_property text DEFAULT NULL` (node-splitting: for each
   capacity-constrained node, split into in-node/out-node pair joined by an
   edge weighted at the node's capacity — a graph transformation applied
   before calling the max-flow class, done in the wrapper).
9. **New: `shortestPath_astar_stream`** (`gds.shortestPath.astar.stream`) —
   `(graph_name, source_node, target_node, latitude_property text,
   longitude_property text, relationship_weight_property text DEFAULT NULL,
   ...)`. Compute a haversine-distance heuristic from each node's
   lat/lon node properties to the target (simple arithmetic in the wrapper,
   read via SPI same as any node property), pass as `distanceHeu` to
   `NetworKit::AStar(G, distanceHeu, source, target, storePred=true)`. Return
   `(total_cost float8, node_ids bigint[], costs float8[])`, same shape as
   the new single-pair Dijkstra function above.
10. **New: `minCostMaxFlow_stats`** (`gds.minCostMaxFlow.stream`/`.stats`) —
    `(graph_name, capacity_property text, cost_property text, source_nodes
    bigint[], target_nodes bigint[], node_capacity_property text DEFAULT
    NULL, ...)`. Use `NetworKit::SuccessiveShortestPathMinCostFlow(G,
    capacityName, supplyName)` — reads capacity/cost/supply directly from
    named edge/node attributes (confirm exact constructor signature against
    `flow/SuccessiveShortestPath.hpp` at implementation time). Multi-source/
    multi-sink via the same super-source/super-sink reduction as
    `maxFlow_stats`, expressed as supply/demand at the virtual nodes.
    Document `alpha` as unsupported (not exposed by this implementation).

**Shared super-source/super-sink helper** in
`src/algorithms/flow_common.h`/`.cpp`, used by both `maxFlow_stats` and
`minCostMaxFlow_stats` — avoid duplicating the graph-augmentation logic.

## Implementation order

1. `bfs_stream`/`dfs_stream` `max_depth`/`target_nodes` (smallest, most
   self-contained changes, good validation of the plan's approach).
2. `allShortestPaths_stream` weight property (trivial, depends only on plan
   00).
3. `spanningTree_stream` objective + weight property + source_node
   validation.
4. `bellmanFord_stream` swap to `AlgebraicBellmanFord` (removes a documented
   substitution — high value).
5. `shortestPath_astar_stream` (new, self-contained, no dependency on other
   items in this plan).
6. `shortestPath_dijkstra_stream` split + `allShortestPaths_dijkstra_stream`
   (largest single change — path output plumbing).
7. `randomWalk_stream` (source_nodes list, walker delegation to
   BiasedRandomWalk — coordinate with `docs/plans/05-embeddings-parity.md`
   since it touches the same NetworKit walker class used by `node2vec_stream`).
8. `src/algorithms/flow_common.h`/`.cpp` shared super-source/super-sink
   helper.
9. `maxFlow_stats` (capacity_property, multi-source/sink, node_capacity_property).
10. `minCostMaxFlow_stats` (new function, built on the flow_common helper).

## Files to change

| File | Change |
|---|---|
| `src/algorithms/flow_common.h`/`.cpp` (new) | Shared super-source/super-sink graph augmentation |
| `src/algorithms/pathfinding.cpp` | All ten items above |
| `sql/age_gds--1.0.sql` | New/changed function signatures (note: `shortestPath_dijkstra_stream` and `randomWalk_stream` are breaking signature changes) |
| `docs/api.md` §6 | Update/add all function docs |
| `test/sql/` | New/updated test cases, including negative-cycle and multi-source/sink cases |

## Docs to update

- `docs/api.md` §6 (Path-finding algorithms) — flag the two breaking changes
  explicitly (`shortestPath_dijkstra_stream` result shape,
  `randomWalk_stream` `source_node` → `source_nodes`).
- `docs/architecture.md` — `pathfinding.cpp` file-header comment listing
  gains a mention of `AlgebraicBellmanFord`, `AStar`, `MultiTargetBFS`,
  `MultiTargetDijkstra`, `SuccessiveShortestPathMinCostFlow` as new NetworKit
  dependencies (`bellmanFord`'s substitution note should be removed once
  fixed).
