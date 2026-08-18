# Plan: New graph-theoretic procedures with no current age_gds equivalent

## Problem / Goal

Add the closable "whole family" procedures from
`docs/neo4j-gds-parity-report.md` §7 that have no existing age_gds function
to extend, and don't fit the path-finding/flow grouping already covered by
`docs/plans/03-pathfinding-parity.md` (A* and min-cost-max-flow live there
instead, since they're path/flow-shaped). This plan covers graph-structure
and community-scoring procedures: topological sort, longest path (DAG),
conductance, bridges, and clique counting.

Excluded (❌, out of scope, tracked in `docs/neo4j-gds-parity-external-libs.md`):
Yen's k-shortest-paths, minimum-weight k-spanning-tree, Steiner tree
(plain + prize-collecting), SLLPA, K-1 coloring, K-means, HDBSCAN, approximate
max-k-cut, CELF/influence maximization.

## Design

1. **New: `topologicalSort_stream`** (`gds.dag.topologicalSort.stream`) —
   `(graph_name, compute_max_distance_from_source boolean DEFAULT false,
   age_graph, node_query, edge_query)` returning `(node_id bigint, sort_index
   int)` plus, when `compute_max_distance_from_source` is true, an additional
   `max_distance int` column. Backed directly by
   `NetworKit::TopologicalSort(G)` (`graph/TopologicalSort.hpp`). The
   max-distance column is a single linear DP pass over the topological
   order: `dist[v] = max(dist[u]) + 1` over predecessors — implemented as a
   plain array reduction driven by `TopologicalSort`'s output, not a new
   traversal algorithm (consistent with CLAUDE.md: this is DP over an
   already-computed ordering, no BFS/DFS/Dijkstra reimplementation).
2. **New: `longestPath_stream`** (`gds.dag.longestPath.stream`) —
   `(graph_name, relationship_weight_property text DEFAULT NULL, age_graph,
   node_query, edge_query)` returning `(node_id bigint, distance float8)`.
   Same `TopologicalSort` + DP pass as above, weighted variant:
   `dist[v] = max over predecessors u of dist[u] + weight(u,v)`. Errors out
   (not silently wraps) if the graph is not a DAG — `TopologicalSort` should
   surface this; confirm its contract at implementation time (does it throw,
   return a partial order, or need an explicit acyclicity check first via
   existing cycle-detection facilities?).
3. **New: `conductance_stats`** (`gds.conductance.stream`) —
   `(graph_name, community_property text, relationship_weight_property text
   DEFAULT NULL, age_graph, node_query, edge_query)` returning `(conductance
   float8)`. Read `community_property` into a `NetworKit::Partition`
   restricted to values `{0,1}` (per `Conductance`'s 2-partition
   requirement — reject/error on other values, don't silently coerce), call
   `NetworKit::Conductance::getQuality(partition, G)`
   (`community/Conductance.hpp`). Reuse the `read_node_property_as_partition`
   helper from `docs/plans/02-community-detection-parity.md` (implement that
   plan's community-detection work first, or duplicate the small helper if
   sequencing requires this plan to land independently — prefer reuse).
4. **New: `bridges_stream`** (`gds.bridges.stream`) —
   `(graph_name, age_graph, node_query, edge_query)` returning `(src bigint,
   dst bigint)`. No dedicated NetworKit `Bridges` class — derive from
   `NetworKit::BiconnectedComponents(G)` (`components/BiconnectedComponents.hpp`,
   already used for `articulationPoints_stream`): an edge is a bridge iff it
   is the sole edge of its biconnected component. Implemented as a one-pass
   filter over `BiconnectedComponents`'s output.
5. **New: `cliqueCounting_stats`** (`gds.cliqueCounting.stream`) —
   `(graph_name, age_graph, node_query, edge_query)` returning `(clique_size
   int, count bigint)` (counts bucketed by size) plus a `total_count bigint`
   summary row, or a simpler `(total_count bigint)` scalar if per-size
   bucketing isn't needed — confirm exact Neo4j result shape against the
   live doc page before implementing (report only says "no algorithm-specific
   config," result shape wasn't captured). Backed by
   `NetworKit::MaximalCliques(G, maximumOnly=false)` (`clique/MaximalCliques.hpp`)
   via its callback API — count as cliques are emitted, no materialization
   of the full clique list needed unless a `_stream` variant listing actual
   clique members is also wanted (decide during implementation based on
   Neo4j's actual result columns).

## Implementation order

1. `bridges_stream` (smallest, reuses `BiconnectedComponents` already in the
   codebase from `articulationPoints_stream` — no new NetworKit dependency).
2. `topologicalSort_stream` (introduces `TopologicalSort`, needed by item 3).
3. `longestPath_stream` (reuses `TopologicalSort` + the DP pattern from item 2).
4. `conductance_stats` (reuses partition-reading helper from plan 02 —
   sequence after plan 02's `partition_post.h` lands, or implement the
   helper here first if this plan proceeds independently).
5. `cliqueCounting_stats` (confirm exact Neo4j result shape via the live docs
   before finalizing the SQL signature — do this first within this item, not
   after writing code).

## Files to change

| File | Change |
|---|---|
| `src/algorithms/community.cpp` | `conductance_stats`, `cliqueCounting_stats` (community-adjacent) |
| `src/algorithms/pathfinding.cpp` or new `src/algorithms/dag.cpp` | `topologicalSort_stream`, `longestPath_stream`, `bridges_stream` — decide file placement based on whether a fifth `dag.cpp` is warranted for three small functions, or whether appending to `pathfinding.cpp`/`community.cpp` fits the existing per-file grouping better |
| `sql/age_gds--1.0.sql` | Five new function declarations |
| `docs/api.md` | New subsections for each (likely under §6 path-finding for the DAG/bridges ones, a new subsection under §5 community for conductance/clique-counting) |
| `test/sql/` | New test cases per function |

## Docs to update

- `docs/api.md` — add new function docs in the appropriate existing
  sections (do not create a new top-level section just for these five; they
  fit naturally under existing centrality/community/path-finding groupings).
- `docs/architecture.md` — add `TopologicalSort`, `BiconnectedComponents`
  (already present), `Conductance`, `MaximalCliques` to the per-file class
  listing for whichever `.cpp` files end up hosting these.
