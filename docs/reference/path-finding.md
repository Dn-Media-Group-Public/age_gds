# Path-finding algorithms

---

### `age_gds.bfs_stream`

**SQL**

```sql
age_gds.bfs_stream(
    graph_name   text     DEFAULT NULL,
    source_node  bigint   DEFAULT NULL,
    age_graph    text     DEFAULT NULL,
    node_query   text     DEFAULT NULL,
    edge_query   text     DEFAULT NULL,
    max_depth    integer  DEFAULT -1,
    target_nodes bigint[] DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Breadth-first search from `source_node`. Returns hop distance (not edge
weight). `distance` is NULL for nodes unreachable from the source (or beyond
`max_depth`, when set). `source_node` must be a valid AGE graphid present in
the graph.

`max_depth` (`-1` = unlimited) caps how far the traversal is *reported* --
nodes farther than `max_depth` hops get a NULL `distance`, same as
unreachable nodes.

`target_nodes`, when given, restricts the result to just those nodes (one
row per entry) and switches to `NetworKit::MultiTargetBFS`, which stops
once every target has been found rather than visiting the whole graph.

**NetworKit classes:** `NetworKit::Traversal::BFSfrom` (no `target_nodes`),
`NetworKit::MultiTargetBFS` (`target_nodes` given)

**Cypher**

```cypher
RETURN age_gds.bfs_stream('proj', {sourceNode: 123, maxDepth: 3, targetNodes: [456, 789]})
```

- `graphName`: named or anonymous graph.
- `config` keys: `sourceNode`, `maxDepth`, `targetNodes`.
- Output: a JSON array with **one** element: `{sourceNode, nodeIds}`,
  matching Neo4j's `gds.bfs.stream` shape.

**No `path` field**: Neo4j's `path` is a linear chain over `nodeIds` using
fabricated, non-graph `NEXT` relationships (no real edge, type, or
properties) — it carries zero information beyond `nodeIds` itself, and
building it would require AGE's private, unexported path-builder internals.
`config.targetNodes` here means "stop once every target is visited" (Neo4j's
real semantics) — `nodeIds` is then the (possibly reduced) visited-node
list. This is a **different feature** from the SQL function's
`target_nodes` (per-target-distance rows via `MultiTargetBFS`, unchanged,
SQL-only). See `docs/architecture.md`'s "Cypher-callable interface" section
for the general calling convention.

---

### `age_gds.shortestPath_dijkstra_stream`

**SQL**

```sql
age_gds.shortestPath_dijkstra_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    target_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE(total_cost float8, node_ids bigint[], costs float8[])
```

**BREAKING CHANGE:** this used to be a single-source-to-all function
returning `(nodeId, distance)` rows. It now matches Neo4j GDS's actual split
-- `gds.shortestPath.dijkstra.stream` is single source/target pair with path
output; the single-source-to-all equivalent is
`age_gds.allShortestPaths_dijkstra_stream` below.

Single-pair shortest path from `source_node` to `target_node`. `node_ids`
includes both `source_node` and `target_node`; `costs[i]` is the cumulative
edge-weight sum from `source_node` to `node_ids[i]` (so `costs[0] = 0` and
`costs[last] = total_cost`). If `target_node` is unreachable, returns a
single row with `total_cost`/`node_ids`/`costs` all NULL (not zero rows).

**NetworKit class:** `NetworKit::Dijkstra` (`storePaths = true`, `target`
constructor argument set for early termination)

**Cypher**

```cypher
RETURN age_gds.shortestpath_dijkstra_stream('proj', {sourceNode: 123, targetNode: 456})
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `shortestpath_dijkstra_stream`.
- `config` keys: `sourceNode`, `targetNode`, `relationshipWeightProperty`.
- Output: array with 1 element `{totalCost, nodeIds, costs}`.

---

### `age_gds.allShortestPaths_dijkstra_stream`

**SQL**

```sql
age_gds.allShortestPaths_dijkstra_stream(
    graph_name                    text     DEFAULT NULL,
    source_node                   bigint   DEFAULT NULL,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    relationship_weight_property  text     DEFAULT NULL,
    target_nodes                  bigint[] DEFAULT NULL
) RETURNS TABLE("targetNodeId" bigint, total_cost float8, node_ids bigint[], costs float8[])
```

New function matching `gds.allShortestPaths.dijkstra.stream`:
single-source, path output for every reachable node (or, if `target_nodes`
is given, just those nodes). One row per target; unreachable targets get
`total_cost`/`node_ids`/`costs` all NULL. See `node_ids`/`costs` semantics
above.

When `target_nodes` is given, this runs one `NetworKit::Dijkstra` per
requested target rather than `NetworKit::MultiTargetDijkstra` -- that class
does not store predecessors/paths (`STSP`'s multi-target constructor
hardcodes `storePred = false`), which would silently drop the path output
that is this function's reason for existing. See
`docs/architecture.md`'s "Path-finding and flow algorithms" section.

**NetworKit class:** `NetworKit::Dijkstra`

**Cypher**

```cypher
UNWIND age_gds.allshortestpaths_dijkstra_stream('proj', {sourceNode: 123}) AS row
RETURN row.targetNodeId, row.totalCost, row.nodeIds, row.costs
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `allshortestpaths_dijkstra_stream`.
- `config` keys: `sourceNode`, `relationshipWeightProperty`, `targetNodes`.
- Output: array of `{targetNodeId, totalCost, nodeIds, costs}`.

---

### `age_gds.deltaStepping_stream`

**SQL**

```sql
age_gds.deltaStepping_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Single-source shortest paths, intended as delta-stepping SSSP.

**Substitution:** NetworKit has no `DeltaStepping` class. `NetworKit::Dijkstra`
is used as an equivalent for graphs with non-negative edge weights. Excluded
from `docs/implemented-plans/03-pathfinding-parity.md` (no `delta` parameter
to add; tracked in `docs/neo4j-gds-parity-external-libs.md`).

**NetworKit class:** `NetworKit::Dijkstra`

**Cypher**

```cypher
UNWIND age_gds.deltastepping_stream('proj', {sourceNode: 123}) AS row
RETURN row.nodeId, row.distance
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `deltastepping_stream`.
- `config` keys: `sourceNode`.
- Output: array of `{nodeId, distance}`.

---

### `age_gds.bellmanFord_stream`

**SQL**

```sql
age_gds.bellmanFord_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8, "isNegativeCycle" boolean)
```

Single-source shortest paths via real Bellman-Ford (supports negative edge
weights). `"isNegativeCycle"` is the same graph-wide boolean repeated on
every row (`NetworKit::AlgebraicBellmanFord` reports negative-cycle
*presence*, not which nodes participate in one) -- when true, `distance`
values are not meaningful shortest-path distances. Use
`age_gds.bellmanFord_stats` if only the negative-cycle flag is needed.

No longer substitutes `Dijkstra` (the previous implementation could not
handle negative weights at all).

**NetworKit class:** `NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix>`

**Cypher**

```cypher
UNWIND age_gds.bellmanford_stream('proj', {sourceNode: 123}) AS row
RETURN row.nodeId, row.distance, row.isNegativeCycle
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `bellmanford_stream`.
- `config` keys: `sourceNode`, `relationshipWeightProperty`.
- Output: array of `{nodeId, distance, isNegativeCycle}`.

---

### `age_gds.bellmanFord_stats`

**SQL**

```sql
age_gds.bellmanFord_stats(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("containsNegativeCycle" boolean, "computeMillis" bigint)
```

New companion stats function: one row with just the negative-cycle flag and
computation time, for callers that don't need per-node distances.

**NetworKit class:** `NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix>`

**Cypher**

```cypher
RETURN age_gds.bellmanford_stats('proj', {sourceNode: 123})
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `bellmanford_stats`.
- `config` keys: same as `bellmanFord_stream`.
- Output: map `{containsNegativeCycle, computeMillis}`.

---

### `age_gds.dfs_stream`

**SQL**

```sql
age_gds.dfs_stream(
    graph_name   text     DEFAULT NULL,
    source_node  bigint   DEFAULT NULL,
    age_graph    text     DEFAULT NULL,
    node_query   text     DEFAULT NULL,
    edge_query   text     DEFAULT NULL,
    max_depth    integer  DEFAULT -1,
    target_nodes bigint[] DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, depth int)
```

Depth-first search from `source_node`. Returns each reachable node with its
DFS tree depth (source node is depth 0). Unreachable nodes, nodes deeper
than `max_depth` (`-1` = unlimited), and (if `target_nodes` is given) nodes
not in that list are omitted.

`max_depth`/`target_nodes` are applied as filters over the fully-computed
depth map rather than an early-exit inside the traversal --
`NetworKit::Traversal::DFSEdgesFrom` has no early-termination hook, and a
hand-rolled DFS loop to add one would violate project rules against
hand-written graph algorithms. The filtered result is identical to what an
early-exit traversal would produce; only the (here, irrelevant for
correctness) work-avoidance benefit of stopping early is not realized.

**NetworKit:** `NetworKit::Traversal::DFSEdgesFrom` (from `<networkit/graph/DFS.hpp>`)

**Cypher**

```cypher
UNWIND age_gds.dfs_stream('proj', {sourceNode: 123, maxDepth: 3}) AS row
RETURN row.nodeId, row.depth
```

- `graphName`: named or anonymous graph.
- `config` keys: `sourceNode`, `maxDepth`, `targetNodes`.
- Output: array of `{nodeId, depth}`.

---

### `age_gds.randomWalk_stream`

**SQL**

```sql
age_gds.randomWalk_stream(
    graph_name                    text     DEFAULT NULL,
    source_nodes                  bigint[] DEFAULT NULL,
    walk_length                   int      DEFAULT 80,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    walks_per_node                int      DEFAULT 10,
    in_out_factor                 float8   DEFAULT 1.0,
    return_factor                 float8   DEFAULT 1.0,
    relationship_weight_property  text     DEFAULT NULL
) RETURNS TABLE(walk_id bigint, "nodeId" bigint)
```

**BREAKING CHANGE:** `source_node bigint` became `source_nodes bigint[]`
(`NULL` = every node, matching Neo4j's default); `walk_length`'s default
changed from `10` to `80` (matching Neo4j).

Returns `walks_per_node` random walks starting from each entry of
`source_nodes` (or every node, if NULL), each up to `walk_length` nodes
long (shorter if a dead end is hit). Rows are grouped by `walk_id` (stable
within one result set, not meaningful across calls) in visit order, so
`walk_id`'s first row is always that walk's starting node.

`in_out_factor` (Neo4j's `Q`) and `return_factor` (Neo4j's `P`) bias the
walk toward exploring outward vs. returning to the previous node --
`in_out_factor = return_factor = 1.0` degenerates to an unbiased random
walk. `relationship_weight_property` is respected (the walker's transition
probabilities are weighted by edge weight).

**Substitution:** NetworKit has no `RandomWalk` class as such, but delegates
to `NetworKit::Embedding::BiasedRandomWalk` -- the same P/Q-biased walker
`NetworKit::Node2Vec` (`age_gds.node2vec_stream`) uses internally, chosen
over the hand-rolled uniform `Aux::Random::choice` this function used
before. See `docs/architecture.md`'s "Path-finding and flow algorithms"
section for why this required an extra, non-default NetworKit include
directory.

**Cypher**

```cypher
UNWIND age_gds.randomwalk_stream('proj', {sourceNodes: [123], walkLength: 80}) AS row
RETURN row.walkId, row.nodeId
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `randomwalk_stream`.
- `config` keys: `sourceNodes`, `walkLength`, `walksPerNode`, `inOutFactor`,
  `returnFactor`, `relationshipWeightProperty`.
- Output: array of `{walkId, nodeId}`.

---

### `age_gds.spanningTree_stream`

**SQL**

```sql
age_gds.spanningTree_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    objective                     text   DEFAULT 'minimum',
    relationship_weight_property  text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint, cost float8)
```

Returns the edges of a spanning forest of the graph. For a connected graph
returns `node_count - 1` edges.

`objective` (`'minimum'` or `'maximum'`) selects a minimum- or
maximum-weight spanning forest. **Note this changes the default behavior**:
even at `objective => 'minimum'` (the default), this now runs
`NetworKit::RandomMaximumSpanningForest` (on negated weights) rather than
the previous, weight-*ignoring* `NetworKit::SpanningForest` -- prior
callers relying on an arbitrary (not minimum-weight) spanning forest will
see a behavior change.

`source_node`, if given, is **validation-only**: raises an error if the
node doesn't exist in the graph, but cannot change which edges come back.
`NetworKit::RandomMaximumSpanningForest` (Kruskal's algorithm) has no
"rooted" concept -- Neo4j's `sourceNode`/rooted semantics are not a real
algorithmic constraint here.

**NetworKit class:** `NetworKit::RandomMaximumSpanningForest`

**Cypher**

```cypher
UNWIND age_gds.spanningtree_stream('proj', {objective: 'minimum'}) AS row
RETURN row.src, row.dst, row.cost
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `spanningtree_stream`.
- `config` keys: `objective`, `relationshipWeightProperty`, `sourceNode`.
- Output: array of `{src, dst, cost}`.

---

### `age_gds.allShortestPaths_stream`

**SQL**

```sql
age_gds.allShortestPaths_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, distance float8)
```

All-pairs shortest paths. Returns `node_count²` rows (including self-pairs
with distance 0). `distance` is NULL for unreachable pairs. Complexity is
O(n²); use with care on large graphs.

**NetworKit class:** `NetworKit::APSP`

**Cypher**

```cypher
UNWIND age_gds.allshortestpaths_stream('proj') AS row
RETURN row.sourceNodeId, row.targetNodeId, row.distance
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `allshortestpaths_stream`.
- `config` keys: `relationshipWeightProperty`.
- Output: array of `{sourceNodeId, targetNodeId, distance}`.

---

### `age_gds.shortestPath_astar_stream`

**SQL**

```sql
age_gds.shortestPath_astar_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    target_node                   bigint DEFAULT NULL,
    latitude_property             text   DEFAULT NULL,
    longitude_property            text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE(total_cost float8, node_ids bigint[], costs float8[])
```

New function matching `gds.shortestPath.astar.stream`. `latitude_property`
and `longitude_property` (both required) are read as node properties via
the projection's `node_query`, and a haversine (great-circle) distance in
kilometers from each node to `target_node` is used as A*'s admissible-lower-
bound heuristic. Result shape matches `shortestPath_dijkstra_stream`
(`node_ids` includes both endpoints; see that function's docs for
`costs` semantics).

**Caveat:** the heuristic assumes edge weights are in the same units as the
great-circle distance (kilometers) -- the same modeling assumption Neo4j's
own `astar` implementation makes. `age_gds` cannot verify this; a mismatch
won't error, it will just produce a heuristic that isn't a true lower bound
(A*'s optimality guarantee no longer holds, though it will still terminate
with *some* path).

**NetworKit class:** `NetworKit::AStar`

**Cypher**

```cypher
RETURN age_gds.shortestpath_astar_stream('proj', {sourceNode: 123, targetNode: 456, latitudeProperty: 'lat', longitudeProperty: 'lon'})
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `shortestpath_astar_stream`.
- `config` keys: `sourceNode`, `targetNode`, `latitudeProperty`,
  `longitudeProperty`, `relationshipWeightProperty`.
- Output: array with 1 element `{totalCost, nodeIds, costs}`.

---

### `age_gds.maxFlow_stats`

**SQL**

```sql
age_gds.maxFlow_stats(
    graph_name              text     DEFAULT NULL,
    source_node             bigint   DEFAULT NULL,
    target_node             bigint   DEFAULT NULL,
    age_graph               text     DEFAULT NULL,
    node_query              text     DEFAULT NULL,
    edge_query              text     DEFAULT NULL,
    capacity_property       text     DEFAULT NULL,
    source_nodes            bigint[] DEFAULT NULL,
    target_nodes            bigint[] DEFAULT NULL,
    node_capacity_property  text     DEFAULT NULL
) RETURNS TABLE(flow float8, "computeMillis" bigint)
```

Maximum flow from `source_node` to `target_node` (or, if `source_nodes`/
`target_nodes` are given, from any of `source_nodes` to any of
`target_nodes`, computed via a super-source/super-sink graph augmentation --
see `docs/architecture.md`). Returns one row with the flow value and
computation time in milliseconds. Not backward compatible in behavior only
if the new parameters are used; the plain `source_node`/`target_node` call
shape is unchanged.

`capacity_property` reuses the `relationship_weight_property` mechanism
(see "Common parameter conventions" above) -- `EdmondsKarp` already treats
graph weight as capacity, so overriding the weight *is* overriding the
capacity.

`node_capacity_property`, if given, caps each node's total throughput by
splitting it into an in-node/out-node pair joined by an edge weighted at
the node's capacity (a graph transformation applied before calling
`EdmondsKarp`, not an algorithm change). Using any of
`source_nodes`/`target_nodes`/`node_capacity_property` requires a directed
graph.

**NetworKit class:** `NetworKit::EdmondsKarp`

**Note:** This function calls `graph.indexEdges()` on the cached graph
(single source/sink path only), which modifies internal edge-ID metadata.
This is safe for subsequent algorithm calls but means the graph is no
longer in a pristine state after this function runs.

**Cypher**

```cypher
RETURN age_gds.maxflow_stats('proj', {sourceNode: 123, targetNode: 456})
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `maxflow_stats`.
- `config` keys: `sourceNode`, `targetNode`, `capacityProperty`,
  `sourceNodes`, `targetNodes`, `nodeCapacityProperty`.
- Output: map `{flow, computeMillis}`.

---

### `age_gds.minCostMaxFlow_stats`

**SQL**

```sql
age_gds.minCostMaxFlow_stats(
    graph_name              text     DEFAULT NULL,
    capacity_property       text     DEFAULT NULL,
    cost_property           text     DEFAULT NULL,
    source_nodes            bigint[] DEFAULT NULL,
    target_nodes            bigint[] DEFAULT NULL,
    age_graph               text     DEFAULT NULL,
    node_query              text     DEFAULT NULL,
    edge_query              text     DEFAULT NULL,
    node_capacity_property  text     DEFAULT NULL
) RETURNS TABLE(total_cost float8, "computeMillis" bigint)
```

New function matching the `gds.minCostMaxFlow` family. Routes the maximum
possible flow (computed via an `EdmondsKarp` prepass on the same topology)
from `source_nodes` to `target_nodes` at minimum total cost.
`capacity_property` is required; `cost_property` defaults to the graph's
existing projected weight if not given. Requires a directed graph, and the
graph's catalog row must have `age_graph`/`edge_query` recorded (re-run
`graph_project`/`graph_index` otherwise). `node_capacity_property` applies
the same node-splitting transform as `maxFlow_stats`, adapted to keep cost
(graph weight) and capacity (a named edge attribute) separate -- see
`docs/architecture.md`.

In practice this means `graph_name` (a named/cached projection created with
`directed => true`), not anonymous execution: `get_or_load_graph()`'s
anonymous path defaults `directed`/`weighted` to `false` regardless of the
`age_graph`/`edge_query` given, so an anonymous call will always fail the
"requires a directed graph" check above.

**Unsupported:** `alpha` (cost-scaling) is not exposed by
`NetworKit::SuccessiveShortestPathMinCostFlow` and is not a parameter of
this function.

**NetworKit classes:** `NetworKit::EdmondsKarp` (max-flow value prepass),
`NetworKit::SuccessiveShortestPathMinCostFlow`

See also: [Plan: Path-finding & Flow Algorithm Parity Fixes](../implemented-plans/03-pathfinding-parity.md)

**Cypher**

```cypher
RETURN age_gds.mincostmaxflow_stats('proj', {capacityProperty: 'cap', sourceNodes: [1], targetNodes: [9]})
```

- `graphName`: named graph only, per the caveat above.
- `config` keys: `capacityProperty`, `costProperty`, `sourceNodes`,
  `targetNodes`, `nodeCapacityProperty`. **Case-sensitive call**: use
  `mincostmaxflow_stats`.
- Output: map `{totalCost, computeMillis}`.

---

### `age_gds.topologicalSort_stream`

**SQL**

```sql
age_gds.topologicalSort_stream(
    graph_name                        text    DEFAULT NULL,
    compute_max_distance_from_source  boolean DEFAULT false,
    age_graph                         text    DEFAULT NULL,
    node_query                        text    DEFAULT NULL,
    edge_query                        text    DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "sortIndex" int, "maxDistance" int)
```

Returns one valid topological order of the graph, one row per node.
`sortIndex` is dense, `0` to `n-1`, matching the returned order. Requires a
directed acyclic graph -- raises an error (not a partial/wrong order) if
the graph is undirected, or contains a cycle.

`compute_max_distance_from_source`: when true, also populates `maxDistance`
-- the length, in edges, of the longest path from any source node (a node
with no incoming edges) to each node. Computed as a single linear DP pass
over the already-computed topological order (`dist[v] = max(dist[u]) + 1`
over predecessors `u`), not a new traversal algorithm. `maxDistance` is
`NULL` when this flag is false.

**NetworKit class:** `NetworKit::TopologicalSort`

See also: [Plan: New Graph-Theoretic Procedures](../implemented-plans/06-new-graph-procedures.md)

**Cypher**

```cypher
UNWIND age_gds.topologicalsort_stream('proj', {computeMaxDistanceFromSource: true}) AS row
RETURN row.nodeId, row.sortIndex, row.maxDistance
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `topologicalsort_stream`.
- `config` keys: `computeMaxDistanceFromSource`.
- Output: array of `{nodeId, sortIndex, maxDistance}`.

---

### `age_gds.longestPath_stream`

**SQL**

```sql
age_gds.longestPath_stream(
    graph_name                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Weighted longest-path-in-a-DAG: for each node, the length of the longest
weighted path from any source node (no incoming edges) to it. Same
`TopologicalSort` + linear DP pass as `topologicalSort_stream`'s
`maxDistance` column, but weighted: `dist[v] = max` over predecessors `u`
of `dist[u] + weight(u,v)`. `relationship_weight_property`: see
"Relationship weight override" above. Requires a directed acyclic graph --
raises an error if the graph is undirected or contains a cycle.

**NetworKit class:** `NetworKit::TopologicalSort`

See also: [Plan: New Graph-Theoretic Procedures](../implemented-plans/06-new-graph-procedures.md)

**Cypher**

```cypher
UNWIND age_gds.longestpath_stream('proj') AS row
RETURN row.nodeId, row.distance
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `longestpath_stream`.
- `config` keys: `relationshipWeightProperty`.
- Output: array of `{nodeId, distance}`.

---

### `age_gds.bridges_stream`

**SQL**

```sql
age_gds.bridges_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint)
```

Returns every bridge edge (an edge whose removal disconnects the graph),
one row per bridge, `src`/`dst` in arbitrary order (the underlying edge is
undirected).

**Substitution:** NetworKit has no dedicated `Bridges` class. Derived from
`NetworKit::BiconnectedComponents` (the same class `articulationPoints_stream`
uses): an edge is a bridge if and only if it is the sole edge of its
biconnected component, i.e. that component's node set has exactly 2
members. One-pass filter over `BiconnectedComponents`'s output.

**Caveat (parallel edges):** "biconnected component has exactly 2 nodes"
and "biconnected component has exactly 1 edge" only coincide for simple
graphs. The AGE-to-NetworKit loader does not deduplicate parallel
relationships between the same node pair, so a projection with two (or
more) parallel edges between the same `u`/`v` would report that pair as a
bridge even though removing just one of the parallel edges leaves the
graph connected. Exact for the common case of no parallel relationships
between the same node pair.

**NetworKit class:** `NetworKit::BiconnectedComponents`

See also: [Plan: New Graph-Theoretic Procedures](../implemented-plans/06-new-graph-procedures.md)

**Cypher**

```cypher
UNWIND age_gds.bridges_stream('proj') AS row
RETURN row.src, row.dst
```

- `graphName`: named or anonymous graph.
- `config` keys: none.
- Output: array of `{src, dst}`.

---

See also: [docs/reference/README.md](README.md)
