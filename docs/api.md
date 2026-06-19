# age_gds SQL API Reference

All functions live in the `age_gds` schema. Install with `CREATE EXTENSION age_gds`.

## Common parameter conventions

### Graph source (all algorithm functions)

Every algorithm function accepts **exactly one** of the two graph-source modes:

| Mode | Parameters required |
|------|---------------------|
| Persistent projection | `graph_name text` |
| Anonymous (query-scoped) | `age_graph text`, `node_query text`, `edge_query text` |

Supplying both `graph_name` and `age_graph` raises
`ERRCODE_INVALID_PARAMETER_VALUE: "supply graph_name OR age_graph, not both"`.

Supplying neither raises
`ERRCODE_INVALID_PARAMETER_VALUE: "must supply graph_name or age_graph"`.

In the anonymous mode, `node_query` and `edge_query` are both required;
omitting either raises an error.

### nodeId values

All `nodeId` / `node1` / `node2` / `sourceNodeId` / `targetNodeId` columns
return the AGE internal graphid (`bigint`). These are the same values returned
by `id(n)` in Cypher.

### Unreachable nodes

Distance functions (BFS, Dijkstra, etc.) return `NULL` for the `distance`
column when a node is unreachable from the source. The `nodeId` column is
always non-NULL.

---

## 1. Projection management

### `age_gds.graph_index`

```sql
age_gds.graph_index(
    name      text,
    age_graph text,
    directed  boolean DEFAULT false,
    weighted  boolean DEFAULT false
) RETURNS void
```

Creates a **live index** projection. Execution:

1. Reads all vertex rows from every vertex-label table in `age_graph`, building
   a `NetworKit::Graph` keyed by AGE graphid.
2. Reads all edge rows from every edge-label table, adding edges.
3. Serializes the initial snapshot to two binary files:
   `<age_gds.data_dir>/<name>.nodes` and `<age_gds.data_dir>/<name>.edges`.
4. Installs AFTER INSERT and AFTER DELETE row-level triggers on every vertex and
   edge label table that exists at creation time. Each trigger atomically appends
   to the `.nodes`/`.edges` file and increments a version counter.
5. Installs `age_gds_label_watch` AFTER INSERT trigger on `ag_catalog.ag_label`
   (shared; one trigger covers all live indices).
6. Records a row in `age_gds.graphs` with `live = true`.

**AGE 1.7.0 trigger bypass limitation:** AGE uses `simple_table_tuple_insert`
for all Cypher DML, bypassing PostgreSQL row-level triggers. This means:
- Vertices and edges inserted via Cypher (`CREATE`) are **not** captured by the
  live index.
- New label tables created via Cypher are **not** detected by the
  `age_gds_label_watch` trigger.

Only mutations issued as direct SQL `INSERT`/`DELETE` on AGE label tables fire
the live-index triggers.

To incorporate Cypher mutations into the live index, drop and recreate it:

```sql
SELECT age_gds.graph_drop('my_index');
SELECT age_gds.graph_index(name => 'my_index', age_graph => 'my_graph');
```

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — `age_graph` does not exist.
- `ERRCODE_INVALID_PARAMETER_VALUE` — `name` already exists with a different
  mode (live vs. snapshot).
- Duplicate `name` raises a unique-constraint violation from `age_gds.graphs`.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md)

---

### `age_gds.graph_reindex`

```sql
age_gds.graph_reindex(name text) RETURNS void
```

Behaviour depends on the projection mode:

- **Live index**: compacts the `.nodes` and `.edges` binary files by rewriting
  them without tombstoned (deleted) entries. Invalidates the session cache so
  the next algorithm call re-reads the compacted files. Does **not** reload
  data from AGE; only removes stale entries from the existing live-index files.

- **Snapshot**: drops and rebuilds the projection from the stored Cypher queries
  (equivalent to `graph_drop` + `graph_project`).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.
- `ERRCODE_INTERNAL_ERROR` — file I/O failure.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md)

---

### `age_gds.graph_project`

```sql
age_gds.graph_project(
    name        text,
    age_graph   text,
    node_query  text,
    edge_query  text,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false
) RETURNS void
```

Projects an AGE Cypher graph into a persistent NetworKit graph. Execution:

1. Runs `node_query` against `age_graph` via `ag_catalog.cypher()`. The query
   must return one column named `id` of type `agtype` containing the AGE
   graphid of each node.
2. Runs `edge_query` against `age_graph`. The query must return three columns:
   `src agtype`, `dst agtype`, `weight agtype`. The `weight` column is used
   only when `weighted = true`; otherwise edge weight defaults to `1.0`.
3. Serializes the resulting `NetworKit::Graph` to
   `<age_gds.data_dir>/<name>.bin`.
4. Inserts a row into `age_gds.graphs` with the projection metadata.

The operation is transactional: if the transaction is aborted, the `.bin` file
is deleted by a registered `XactCallback`.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — `age_graph` does not exist in `ag_catalog.ag_graph`.
- `ERRCODE_INTERNAL_ERROR` — serialization or SPI failure.
- Duplicate `name` raises a unique-constraint violation from `age_gds.graphs`.

---

### `age_gds.graph_drop`

```sql
age_gds.graph_drop(name text) RETURNS void
```

Drops the projection:
- For **snapshot** projections: deletes the catalog row, unlinks the `.bin`
  file, and evicts the entry from the calling backend's session cache.
- For **live index** projections: additionally drops all data triggers installed
  on AGE label tables for this index (entries in `age_gds.index_triggers` are
  cascade-deleted), and unlinks the `.nodes` and `.edges` files.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.

---

### `age_gds.graph_list`

```sql
age_gds.graph_list() RETURNS TABLE(
    name          text,
    age_graph     text,
    node_count    bigint,
    edge_count    bigint,
    directed      boolean,
    weighted      boolean,
    live          boolean,
    file_path     text,
    created_at    timestamptz,
    "memoryUsage" bigint
)
```

Returns all rows from `age_gds.graphs`, ordered by `created_at`. The
`live` column is `true` for live-index projections and `false` for snapshots.
The `memoryUsage` column is a derived estimate: `node_count * 8 + edge_count * 20`
bytes (approximate in-memory cost of the deserialized graph).

---

### `age_gds.graph_exists`

```sql
age_gds.graph_exists(name text) RETURNS boolean
```

Returns `true` if a projection named `name` exists in the catalog, `false`
otherwise. STRICT (NULL input returns NULL).

---

### `age_gds.graph_is_stale`

```sql
age_gds.graph_is_stale(name text) RETURNS boolean
```

Returns `true` if the underlying AGE graph has been modified since the last
`ANALYZE` run. Internally queries `pg_stat_user_tables` for the AGE graph
schema's `_ag_label_vertex` table and checks `n_mod_since_analyze > 0`.

Always returns `false` for **live index** projections (they are never stale
by definition — mutations are captured immediately via triggers).

Returns `false` if no statistics row exists (cannot determine staleness).
STRICT (NULL input returns NULL).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.

---

## 2. Session cache

### `age_gds.cache_stats`

```sql
age_gds.cache_stats() RETURNS TABLE(
    entry_count int,
    total_bytes bigint,
    limit_bytes bigint
)
```

Returns one row describing the calling backend's session cache:
- `entry_count` — number of graphs currently loaded.
- `total_bytes` — sum of `approx_bytes` across all cached graphs.
- `limit_bytes` — the current `age_gds.session_cache_mb` GUC value in bytes.

---

### `age_gds.cache_load`

```sql
age_gds.cache_load(graph_name text) RETURNS void
```

Pre-warms the session cache by loading `graph_name` from disk if not already
cached. Equivalent to calling any algorithm with `graph_name` and discarding
the result; useful for pre-loading before latency-sensitive queries.
STRICT (NULL input returns NULL).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection does not exist.
- `ERRCODE_INTERNAL_ERROR` — `.bin` file cannot be read.

---

## 3. Utility

### `age_gds.version`

```sql
age_gds.version() RETURNS text
```

Returns the extension version string (currently `"0.1.0"`). Useful to verify
the C++ entry point is reachable after `CREATE EXTENSION`.

---

## 4. Centrality algorithms

All centrality functions return one row per node in the graph.

---

### `age_gds.pageRank_stream`

```sql
age_gds.pageRank_stream(
    graph_name     text   DEFAULT NULL,
    damping_factor float8 DEFAULT NULL,
    max_iterations int    DEFAULT NULL,
    tolerance      float8 DEFAULT NULL,
    config         jsonb  DEFAULT NULL,
    age_graph      text   DEFAULT NULL,
    node_query     text   DEFAULT NULL,
    edge_query     text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Computes PageRank scores. Delegates to `NetworKit::PageRank`.

**Parameters:**
- `damping_factor` — teleportation probability, default `0.85`.
- `max_iterations` — maximum power-iteration steps, default `20`.
- `tolerance` — convergence threshold, default `1e-7`.
- `config` — JSONB object; accepted keys: `"dampingFactor"`, `"maxIterations"`,
  `"tolerance"`. Named parameters override JSONB values when both are present.

Scores sum to approximately 1.0. All scores are non-negative.

**NetworKit class:** `NetworKit::PageRank`

---

### `age_gds.pageRank_stats`

```sql
age_gds.pageRank_stats(
    graph_name     text   DEFAULT NULL,
    damping_factor float8 DEFAULT NULL,
    max_iterations int    DEFAULT NULL,
    tolerance      float8 DEFAULT NULL,
    config         jsonb  DEFAULT NULL,
    age_graph      text   DEFAULT NULL,
    node_query     text   DEFAULT NULL,
    edge_query     text   DEFAULT NULL
) RETURNS TABLE(
    "ranIterations" int,
    "didConverge"   bool,
    "computeMillis" bigint,
    "minScore"      float8,
    "maxScore"      float8,
    "meanScore"     float8
)
```

Runs PageRank and returns a single summary row instead of per-node scores.
`didConverge` is `true` when `ranIterations < max_iterations`.

**NetworKit class:** `NetworKit::PageRank`

---

### `age_gds.betweenness_stream`

```sql
age_gds.betweenness_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Betweenness centrality: fraction of shortest paths passing through each node.

**NetworKit class:** `NetworKit::Betweenness` (normalized=false)

---

### `age_gds.closeness_stream`

```sql
age_gds.closeness_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Closeness centrality. Uses the `GENERALIZED` variant, which handles
disconnected graphs without dividing by zero.

**NetworKit class:** `NetworKit::Closeness` (variant=GENERALIZED)

---

### `age_gds.harmonicCentrality_stream`

```sql
age_gds.harmonicCentrality_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Harmonic centrality: sum of reciprocal distances. Works correctly on
disconnected graphs (unreachable pairs contribute 0).

**NetworKit class:** `NetworKit::HarmonicCloseness`

---

### `age_gds.degree_stream`

```sql
age_gds.degree_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Degree centrality (normalized node degree).

**NetworKit class:** `NetworKit::DegreeCentrality` (normalized=false)

---

### `age_gds.eigenvector_stream`

```sql
age_gds.eigenvector_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Eigenvector centrality: node importance proportional to the importance of its
neighbors.

**NetworKit class:** `NetworKit::EigenvectorCentrality`

---

### `age_gds.hits_stream`

```sql
age_gds.hits_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, auth float8, hub float8)
```

HITS (Hyperlink-Induced Topic Search) authority and hub scores.

**Substitution:** NetworKit has no HITS class. Both `auth` and `hub` return
the same `EigenvectorCentrality` score. For undirected graphs this is
equivalent; for directed graphs a proper HITS would use the transposed graph
for authority scores.

**NetworKit class:** `NetworKit::EigenvectorCentrality`

---

### `age_gds.articleRank_stream`

```sql
age_gds.articleRank_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

ArticleRank centrality. Run with fixed parameters: damping=0.85,
tolerance=1e-6, maxIterations=20.

**Substitution:** NetworKit has no ArticleRank class. `NetworKit::PageRank`
is used with default parameters.

**NetworKit class:** `NetworKit::PageRank`

---

### `age_gds.articulationPoints_stream`

```sql
age_gds.articulationPoints_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint)
```

Returns the set of articulation points (cut vertices) — nodes whose removal
increases the number of connected components.

**Substitution:** NetworKit has no dedicated ArticulationPoints class. Computed
via `BiconnectedComponents`: a node is an articulation point if and only if
`getComponentsOfNode(u).size() > 1`.

**NetworKit class:** `NetworKit::BiconnectedComponents`

---

## 5. Community detection algorithms

All community/structure functions return one row per node.

---

### `age_gds.louvain_stream`

```sql
age_gds.louvain_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Louvain community detection. Community IDs are arbitrary non-negative integers.

**NetworKit class:** `NetworKit::PLM`

---

### `age_gds.leiden_stream`

```sql
age_gds.leiden_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Leiden community detection (refinement of Louvain).

**NetworKit class:** `NetworKit::ParallelLeiden`

---

### `age_gds.labelPropagation_stream`

```sql
age_gds.labelPropagation_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
```

Label propagation community detection.

**NetworKit class:** `NetworKit::PLP`

---

### `age_gds.wcc_stream`

```sql
age_gds.wcc_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
```

Weakly connected components. All nodes in the same component share the same
`componentId`.

**NetworKit class:** `NetworKit::ConnectedComponents`

---

### `age_gds.scc_stream`

```sql
age_gds.scc_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
```

Strongly connected components.

**NetworKit class:** `NetworKit::StronglyConnectedComponents`

---

### `age_gds.kCore_stream`

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

---

### `age_gds.triangleCount_stream`

```sql
age_gds.triangleCount_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "triangleCount" bigint)
```

Per-node triangle count. Computed as `round(lcc * deg * (deg - 1) / 2)`.
Nodes with degree < 2 return 0.

**NetworKit class:** `NetworKit::LocalClusteringCoefficient`

---

### `age_gds.localClusteringCoefficient_stream`

```sql
age_gds.localClusteringCoefficient_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, coefficient float8)
```

Local clustering coefficient: fraction of existing triangles among all possible
triangles through each node. Range [0.0, 1.0].

**NetworKit class:** `NetworKit::LocalClusteringCoefficient`

---

### `age_gds.modularity_stats`

```sql
age_gds.modularity_stats(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(modularity float8)
```

Returns the modularity score of the Louvain partition. Runs PLM internally to
obtain the partition, then evaluates it with `NetworKit::Modularity::getQuality`.
Returns one row.

**NetworKit classes:** `NetworKit::PLM`, `NetworKit::Modularity`

---

## 6. Path-finding algorithms

---

### `age_gds.bfs_stream`

```sql
age_gds.bfs_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Breadth-first search from `source_node`. Returns hop distance (not edge
weight). `distance` is NULL for nodes unreachable from the source.
`source_node` must be a valid AGE graphid present in the graph.

**NetworKit class:** `NetworKit::BFS`

---

### `age_gds.shortestPath_dijkstra_stream`

```sql
age_gds.shortestPath_dijkstra_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Single-source shortest paths from `source_node` using Dijkstra. `distance` is
the sum of edge weights along the shortest path, or NULL if unreachable.

**NetworKit class:** `NetworKit::Dijkstra`

---

### `age_gds.deltaStepping_stream`

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
is used as an equivalent for graphs with non-negative edge weights.

**NetworKit class:** `NetworKit::Dijkstra`

---

### `age_gds.bellmanFord_stream`

```sql
age_gds.bellmanFord_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
```

Single-source shortest paths, intended as Bellman-Ford SSSP.

**Substitution:** NetworKit's `AlgebraicBellmanFord<Matrix>` requires a matrix
instantiation and MinPlusSemiring, which is impractical to use here.
`NetworKit::Dijkstra` is used as a fallback for non-negative weight graphs.
Negative edge weights are not supported.

**NetworKit class:** `NetworKit::Dijkstra`

---

### `age_gds.dfs_stream`

```sql
age_gds.dfs_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, depth int)
```

Depth-first search from `source_node`. Returns each reachable node with its
DFS tree depth (source node is depth 0). Unreachable nodes are omitted.

**NetworKit:** `NetworKit::Traversal::DFSEdgesFrom` (from `<networkit/graph/DFS.hpp>`)

---

### `age_gds.randomWalk_stream`

```sql
age_gds.randomWalk_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    walk_length int    DEFAULT 10,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint)
```

Returns the sequence of nodes visited during a random walk of `walk_length`
steps starting from `source_node`. The result includes `source_node` as the
first row. Nodes may repeat. The walk stops early if a dead-end (no neighbors)
is reached.

**Substitution:** NetworKit has no `RandomWalk` class. Implemented using
`Aux::Random::choice` + `Graph::forNeighborsOf`.

---

### `age_gds.spanningTree_stream`

```sql
age_gds.spanningTree_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint, cost float8)
```

Returns the edges of a spanning forest of the graph. For a connected graph
returns `node_count - 1` edges.

**NetworKit class:** `NetworKit::SpanningForest`

---

### `age_gds.allShortestPaths_stream`

```sql
age_gds.allShortestPaths_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, distance float8)
```

All-pairs shortest paths. Returns `node_count²` rows (including self-pairs
with distance 0). `distance` is NULL for unreachable pairs. Complexity is
O(n²); use with care on large graphs.

**NetworKit class:** `NetworKit::APSP`

---

### `age_gds.maxFlow_stats`

```sql
age_gds.maxFlow_stats(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    target_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE(flow float8, "computeMillis" bigint)
```

Maximum flow from `source_node` to `target_node`. Both nodes must be in the
graph and must be distinct. Returns one row with the flow value and computation
time in milliseconds.

**NetworKit class:** `NetworKit::EdmondsKarp`

**Note:** This function calls `graph.indexEdges()` on the cached graph, which
modifies internal edge-ID metadata. This is safe for subsequent algorithm calls
but means the graph is no longer in a pristine state after this function runs.

---

## 7. Similarity

---

### `age_gds.nodeSimilarity_stream`

```sql
age_gds.nodeSimilarity_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, similarity float8)
```

Jaccard-index similarity for all unordered node pairs (u, v) where u < v.
`similarity` is in [0.0, 1.0]. Returns `node_count * (node_count - 1) / 2`
rows.

**NetworKit class:** `NetworKit::JaccardIndex` (via LinkPredictor interface)

---

### `age_gds.knn_stream`

```sql
age_gds.knn_stream(
    graph_name text DEFAULT NULL,
    top_k      int  DEFAULT 5,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, similarity float8, rank int)
```

K-nearest neighbors by Jaccard similarity. For each node returns up to `top_k`
most similar neighbors sorted descending by similarity. `rank` is 1-based.

**NetworKit class:** `NetworKit::JaccardIndex`

---

## 8. Link prediction

All link-prediction functions score **all currently unconnected node pairs**
using `runAll()` and return one row per pair.

---

### `age_gds.linkprediction_adamicAdar_stream`

```sql
age_gds.linkprediction_adamicAdar_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Adamic-Adar index: sum of `1 / log(deg(w))` for common neighbors `w`.

**NetworKit class:** `NetworKit::AdamicAdarIndex`

---

### `age_gds.linkprediction_commonNeighbors_stream`

```sql
age_gds.linkprediction_commonNeighbors_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Common-neighbors count.

**NetworKit class:** `NetworKit::CommonNeighborsIndex`

---

### `age_gds.linkprediction_preferentialAttachment_stream`

```sql
age_gds.linkprediction_preferentialAttachment_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Preferential attachment: product of node degrees.

**NetworKit class:** `NetworKit::PreferentialAttachmentIndex`

---

### `age_gds.linkprediction_resourceAllocation_stream`

```sql
age_gds.linkprediction_resourceAllocation_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Resource allocation index: sum of `1 / deg(w)` for common neighbors `w`.

**NetworKit class:** `NetworKit::ResourceAllocationIndex`

---

## 9. Embeddings

### `age_gds.node2vec_stream`

```sql
age_gds.node2vec_stream(
    graph_name      text    DEFAULT NULL,
    embedding_dim   int     DEFAULT 64,
    walk_length     int     DEFAULT 80,
    walks_per_node  int     DEFAULT 10,
    p               float8  DEFAULT 1.0,
    q               float8  DEFAULT 1.0,
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, embedding float8[])
```

Node2Vec biased random-walk embeddings. Returns one row per node with a
`float8[]` array of length `embedding_dim`.

**Parameters:**
- `embedding_dim` — vector dimensionality, default 64.
- `walk_length` — length of each random walk (L), default 80.
- `walks_per_node` — number of walks per node (N), default 10.
- `p` — return parameter (controls likelihood of revisiting the previous node),
  default 1.0.
- `q` — in-out parameter (controls BFS vs DFS tendency), default 1.0.

Parameters ≤ 0 are silently reset to their defaults.

**NetworKit class:** `NetworKit::Node2Vec`

---

## 10. Write-back

Write-back functions compute an algorithm and then persist the scores back to
the corresponding AGE nodes as a Cypher property, using `SET n.<property> = <score>`.

---

### `age_gds.pageRank_write`

```sql
age_gds.pageRank_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'pagerank',
    damping_factor  float8  DEFAULT NULL,
    max_iterations  int     DEFAULT NULL,
    tolerance       float8  DEFAULT NULL,
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
```

Computes PageRank and writes each node's score to the AGE property named by
`write_property`. For named projections, the target AGE graph is resolved from
`age_gds.graphs.age_graph`.

---

### `age_gds.betweenness_write`

```sql
age_gds.betweenness_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'betweenness',
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
```

Computes betweenness centrality and writes each node's score to the AGE
property named by `write_property`.

---

## Catalog tables

### `age_gds.graphs`

```sql
CREATE TABLE age_gds.graphs (
    name             text        PRIMARY KEY,
    age_graph        text        NOT NULL DEFAULT '',
    node_query       text,
    edge_query       text,
    node_count       bigint,
    edge_count       bigint,
    directed         boolean     NOT NULL DEFAULT false,
    weighted         boolean     NOT NULL DEFAULT false,
    live             boolean     NOT NULL DEFAULT false,
    file_path        text,           -- snapshot .bin path (NULL for live)
    nodes_file_path  text,           -- live index .nodes path (NULL for snapshot)
    edges_file_path  text,           -- live index .edges path (NULL for snapshot)
    created_at       timestamptz NOT NULL DEFAULT now(),
    projected_by     text
);
```

The catalog table. `projected_by` stores the role that created the projection
but is not exposed through `graph_list()`. `live = true` indicates a live-index
projection; `false` indicates a snapshot. Direct reads are supported but
`graph_list()` is the recommended interface.

---

### `age_gds.index_triggers`

```sql
CREATE TABLE age_gds.index_triggers (
    graph_name   text    NOT NULL REFERENCES age_gds.graphs(name) ON DELETE CASCADE,
    schema_name  text    NOT NULL,
    table_name   text    NOT NULL,
    trigger_name text    NOT NULL,
    PRIMARY KEY (graph_name, schema_name, table_name)
);
```

Tracks which triggers have been installed on AGE label tables for live-index
projections. Rows are cascade-deleted when the corresponding `graphs` entry is
dropped. `graph_drop` reads this table to remove the triggers before deleting
the `graphs` row.

---

## GUC settings

| GUC | Type | Default | Context | Description |
|-----|------|---------|---------|-------------|
| `age_gds.session_cache_mb` | int | 512 | USERSET | Per-backend graph cache size limit in MB. Minimum 1, maximum 65536. |
| `age_gds.data_dir` | string | `$PGDATA/age_gds` | SUSET | Directory where `.bin` projection files are stored. `$PGDATA` is expanded to the PostgreSQL data directory. |
