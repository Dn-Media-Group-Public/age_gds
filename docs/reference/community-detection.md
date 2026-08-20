# Community detection algorithms

All community/structure functions return one row per node.

---

### `age_gds.louvain_stream`

**SQL**

```sql
age_gds.louvain_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    gamma                         float8 DEFAULT 1.0,
    max_iterations                int    DEFAULT 10,
    recurse                       boolean DEFAULT true,
    consecutive_ids               boolean DEFAULT false,
    min_community_size            int    DEFAULT 0
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Louvain community detection. Community IDs are arbitrary non-negative
integers unless `consecutive_ids` is set.

- `relationship_weight_property`: see "Relationship weight override" above.
- `gamma`: `NetworKit::PLM`'s multi-resolution modularity parameter (no
  direct Neo4j GDS equivalent; the natural granularity knob). `1.0` =
  standard modularity, `0.0` = one community, `2m` = singleton communities.
- `max_iterations`: maps to PLM's `maxIter` (max moves per level). **Default
  changed from PLM's own default of 32 to 10** to match this plan's design;
  pass `max_iterations => 32` to restore PLM's native default.
- `recurse`: approximates Neo4j's `maxLevels` -- `true` (default, matches
  prior behavior) uses PLM's recursive multilevel coarsening; `false` runs a
  single level only. This is **not** an exact port of `maxLevels` (no
  numeric cap is exposed), just the closest binary approximation NetworKit
  exposes -- see `docs/architecture.md`.
- `consecutive_ids`: renumber community ids to a dense `0..k-1` range via
  `NetworKit::Partition::compact()`.
- `min_community_size`: nodes in a community smaller than this are **omitted
  from the result** (Neo4j semantics: excluded, not merged/reassigned).

**NetworKit class:** `NetworKit::PLM`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.louvain_stream('proj', {gamma: 1.0, maxIterations: 10}) AS row
RETURN row.nodeId, row.communityId
```

- `graphName`: named or anonymous graph.
- `config` keys: `relationshipWeightProperty`, `gamma`, `maxIterations`,
  `recurse`, `consecutiveIds`, `minCommunitySize`.
- Output: array of `{nodeId, communityId}`.

---

### `age_gds.leiden_stream`

**SQL**

```sql
age_gds.leiden_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    gamma                         float8 DEFAULT 1.0,
    iterations                    int    DEFAULT 10
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Leiden community detection (refinement of Louvain).

- `relationship_weight_property`: see "Relationship weight override" above.
- `gamma`: `NetworKit::ParallelLeiden`'s resolution parameter (direct
  constructor argument).
- `iterations`: number of full Leiden runs (`ParallelLeiden`'s own
  constructor parameter of the same name; **default changed from
  NetworKit's own default of 3 to 10**). This is only a loose analogue of
  Neo4j's `maxLevels` -- Neo4j's parameter controls coarsening *depth*
  within a single run, while NetworKit's `iterations` repeats the *entire*
  algorithm; they are not the same knob. See `docs/architecture.md`.

**NetworKit class:** `NetworKit::ParallelLeiden`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.leiden_stream('proj', {gamma: 1.0, iterations: 10}) AS row
RETURN row.nodeId, row.communityId
```

- `graphName`: named or anonymous graph.
- `config` keys: `relationshipWeightProperty`, `gamma`, `iterations`.
- Output: array of `{nodeId, communityId}`.

---

### `age_gds.labelPropagation_stream`

**SQL**

```sql
age_gds.labelPropagation_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    seed_property                 text   DEFAULT NULL,
    consecutive_ids               boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Label propagation community detection.

- `relationship_weight_property`: see "Relationship weight override" above.
- `max_iterations`: maps to `NetworKit::PLP`'s `maxIterations` constructor
  parameter. `NULL` (default) keeps NetworKit's own default (unbounded,
  subject to its internal convergence heuristic) -- unchanged from prior
  behavior.
- `consecutive_ids`: renumber community ids to a dense `0..k-1` range.
- `seed_property` **is accepted in the signature but always raises
  `ERRCODE_FEATURE_NOT_SUPPORTED`.** `NetworKit::PLP`'s seeded constructor
  (`PLP(G, baseClustering, theta)`) never initializes its private
  `maxIterations` member (confirmed by inspecting the vendored
  `third_party/networkit/include/networkit/community/PLP.hpp`/`.cpp`), so
  `run()`'s loop condition reads an uninitialized value on that path, and
  there is no public setter to fix it after construction. This is a
  NetworKit API gap discovered while implementing this plan (beyond
  `node_weight_property`, which the plan's own revision already excluded
  for a similar reason) -- age_gds refuses to either rely on the resulting
  undefined behavior or hand-roll seeded label propagation, both of which
  are against project rules. `node_weight_property` is dropped from scope
  entirely for the same underlying reason (see the plan doc).

**NetworKit class:** `NetworKit::PLP`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.labelpropagation_stream('proj', {maxIterations: 20}) AS row
RETURN row.nodeId, row.communityId
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `labelpropagation_stream`.
- `config` keys: `relationshipWeightProperty`, `maxIterations`,
  `seedProperty` (accepted but always refused, same as the SQL form),
  `consecutiveIds`.
- Output: array of `{nodeId, communityId}`.

---

### `age_gds.wcc_stream`

**SQL**

```sql
age_gds.wcc_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    threshold                     float8 DEFAULT NULL,
    consecutive_ids               boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
```

Weakly connected components. All nodes in the same component share the same
`componentId`.

- `relationship_weight_property`: see "Relationship weight override" above;
  applied *before* the `threshold` filter, so the filter sees the
  overridden weights.
- `threshold`: builds a private, filtered *copy* of the graph excluding
  every edge whose weight is below `threshold`, then runs
  `NetworKit::ConnectedComponents` on the copy (same "never mutate the
  cached graph" pattern as the weight-override copy). On an unweighted
  projection every edge has `NetworKit::defaultEdgeWeight` (`1.0`), so a
  `threshold > 1.0` drops every edge.
- `consecutive_ids`: renumber component ids to a dense `0..k-1` range.

**NetworKit class:** `NetworKit::ConnectedComponents`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.wcc_stream('proj', {threshold: 0.5}) AS row
RETURN row.nodeId, row.componentId
```

- `graphName`: named or anonymous graph.
- `config` keys: `relationshipWeightProperty`, `threshold`, `consecutiveIds`.
- Output: array of `{nodeId, componentId}`.

---

### `age_gds.scc_stream`

**SQL**

```sql
age_gds.scc_stream(
    graph_name       text    DEFAULT NULL,
    age_graph        text    DEFAULT NULL,
    node_query       text    DEFAULT NULL,
    edge_query       text    DEFAULT NULL,
    consecutive_ids  boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
```

Strongly connected components.

- `consecutive_ids`: renumber component ids to a dense `0..k-1` range.

**NetworKit class:** `NetworKit::StronglyConnectedComponents`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.scc_stream('proj', {consecutiveIds: true}) AS row
RETURN row.nodeId, row.componentId
```

- `graphName`: named or anonymous graph.
- `config` keys: `consecutiveIds`.
- Output: array of `{nodeId, componentId}`.

---

### `age_gds.kCore_stream`

**SQL**

```sql
age_gds.kCore_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "coreNumber" bigint)
```

k-core decomposition. `coreNumber` is the maximum k such that the node
belongs to the k-core.

**NetworKit class:** `NetworKit::CoreDecomposition`

**Cypher**

```cypher
UNWIND age_gds.kcore_stream('proj') AS row
RETURN row.nodeId, row.coreNumber
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `kcore_stream`.
- `config` keys: none.
- Output: array of `{nodeId, coreNumber}`.

---

### `age_gds.triangleCount_stream`

**SQL**

```sql
age_gds.triangleCount_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL,
    max_degree bigint DEFAULT 9223372036854775807
) RETURNS TABLE("nodeId" bigint, "triangleCount" bigint)
```

Per-node triangle count. Computed as `round(lcc * deg * (deg - 1) / 2)`.
Nodes with degree < 2 return 0.

- `max_degree`: nodes whose degree exceeds `max_degree` return `-1` instead
  of a computed count (pre-filter, matching Neo4j GDS semantics for
  high-degree hub nodes).

**NetworKit class:** `NetworKit::LocalClusteringCoefficient`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.trianglecount_stream('proj', {maxDegree: 1000}) AS row
RETURN row.nodeId, row.triangleCount
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `trianglecount_stream`.
- `config` keys: `maxDegree`.
- Output: array of `{nodeId, triangleCount}`.

---

### `age_gds.localClusteringCoefficient_stream`

**SQL**

```sql
age_gds.localClusteringCoefficient_stream(
    graph_name              text DEFAULT NULL,
    age_graph               text DEFAULT NULL,
    node_query              text DEFAULT NULL,
    edge_query              text DEFAULT NULL,
    triangle_count_property text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, coefficient float8)
```

Local clustering coefficient: fraction of existing triangles among all possible
triangles through each node. Range [0.0, 1.0].

- `triangle_count_property`: when given, bypasses
  `NetworKit::LocalClusteringCoefficient` entirely and computes
  `2 * triangles(u) / (deg(u) * (deg(u) - 1))` directly from a named,
  precomputed per-node triangle-count property (read via the projection's
  stored `node_query` -- see "Node-property read" above). Nodes with
  degree < 2 return 0.

**NetworKit class:** `NetworKit::LocalClusteringCoefficient` (unless
`triangle_count_property` is given)

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
UNWIND age_gds.localclusteringcoefficient_stream('proj', {triangleCountProperty: 'tc'}) AS row
RETURN row.nodeId, row.coefficient
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `localclusteringcoefficient_stream`.
- `config` keys: `triangleCountProperty`.
- Output: array of `{nodeId, coefficient}`.

---

### `age_gds.modularity_stats`

**SQL**

```sql
age_gds.modularity_stats(
    graph_name                    text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL,
    community_property            text DEFAULT NULL,
    relationship_weight_property  text DEFAULT NULL
) RETURNS TABLE(modularity float8)
```

Returns the modularity score of a partition. Without `community_property`,
runs PLM internally to obtain the partition (unchanged prior behavior; note
the plan's design doc describes this parameter as conceptually "required" for
the closable gap it targets, but it is `DEFAULT NULL` here so existing calls
with no arguments keep working). With `community_property`, evaluates the
named, precomputed per-node community-label property instead (read via the
projection's stored `node_query` -- see "Node-property read" above).
`relationship_weight_property`: see "Relationship weight override" above.
Returns one row.

**NetworKit classes:** `NetworKit::PLM` (unless `community_property` is
given), `NetworKit::Modularity`

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

**Cypher**

```cypher
RETURN age_gds.modularity_stats('proj', {communityProperty: 'community'})
```

- `graphName`: named or anonymous graph.
- `config` keys: `communityProperty`, `relationshipWeightProperty`.
- Output: map `{modularity}`.

---

### `age_gds.conductance_stats`

**SQL**

```sql
age_gds.conductance_stats(
    graph_name                    text DEFAULT NULL,
    community_property            text DEFAULT NULL,
    relationship_weight_property  text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL
) RETURNS TABLE(conductance float8)
```

Returns the conductance of a 2-partition: cut size over the volume of the
smaller-volume side. `community_property` is required and must assign every
node a value of exactly `0` or `1` (read via the projection's stored
`node_query` -- see "Node-property read" above); any other value, or a
missing value on any node, raises an error rather than being silently
coerced. `relationship_weight_property`: see "Relationship weight override"
above. Returns one row.

**Why not `read_node_property_as_partition`:** unlike other community
functions in this file, this does *not* reuse plan 02's
`read_node_property_as_partition()` helper. `NetworKit::Conductance::getQuality()`
requires the partition's cluster ids to be the literal values `0`/`1` --
internally it indexes fixed-size two-element arrays directly by cluster id,
with no bounds check, so a mismatched id would be undefined behavior, not a
caught error. `read_node_property_as_partition()` deliberately assigns
*internal* dense subset ids offset above the graph's node-id range (so
unrelated property values never collide with default singleton ids) --
exactly the wrong shape for `Conductance`'s hard 2-partition contract. This
function instead reuses `read_node_property_as_values()` (also from plan
02's `partition_post.h`) to read the raw numeric property and builds the
`NetworKit::Partition` directly from validated literal `0`/`1` ids.

**NetworKit class:** `NetworKit::Conductance`

See also: [Plan: New Graph-Theoretic Procedures](../implemented-plans/06-new-graph-procedures.md)

**Cypher**

```cypher
RETURN age_gds.conductance_stats('proj', {communityProperty: 'side'})
```

- `graphName`: named or anonymous graph.
- `config` keys: `communityProperty` (required), `relationshipWeightProperty`.
- Output: map `{conductance}`.

---

### `age_gds.cliqueCounting_stats`

**SQL**

```sql
age_gds.cliqueCounting_stats(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(clique_size int, count bigint)
```

Requires an undirected graph (cliques are not defined for directed edges;
`NetworKit::MaximalCliques` does not itself reject directed input, so this
is checked explicitly rather than silently treating edge direction as
irrelevant). Counts maximal cliques, bucketed by clique size: one row per
distinct size observed, with `count` being the number of maximal cliques
of that size.
The total maximal-clique count across all sizes is `SUM(count)` over the
result rows. Note this counts *maximal* cliques only -- NetworKit has no
primitive for enumerating every clique of every size (maximal or not), so
this does not exactly reproduce Neo4j GDS's per-node `counts` semantics
(which include non-maximal sub-cliques); it is the direct, thin-wrapper
metric NetworKit's `MaximalCliques` callback API can produce without
hand-rolled clique enumeration.

**NetworKit class:** `NetworKit::MaximalCliques` (callback form, counted and
bucketed by size as each clique is emitted -- no materialization of the
full clique list)

See also: [Plan: New Graph-Theoretic Procedures](../implemented-plans/06-new-graph-procedures.md)

**Cypher**

```cypher
UNWIND age_gds.cliquecounting_stats('proj') AS row
RETURN row.cliqueSize, row.count
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `cliquecounting_stats`.
- `config` keys: none.
- Output: array of `{cliqueSize, count}` (despite the `_stats` name — see
  the SQL description above).

---

See also: [docs/reference/README.md](README.md)
