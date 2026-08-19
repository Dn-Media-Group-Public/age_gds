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

### Relationship weight override (`weight_property` / `relationship_weight_property`)

Some algorithm functions accept a trailing `relationship_weight_property text
DEFAULT NULL` parameter (Neo4j GDS's `relationshipWeightProperty`
convention), letting a single call use a different AGE edge property as the
graph weight than whatever the graph was projected/loaded with. See
[Plan: Per-call relationship weight plumbing](implemented-plans/00-weight-plumbing.md)
for the full design.

- **Anonymous mode** (`age_graph`/`node_query`/`edge_query`): there is no
  dedicated override parameter, and none is needed — write the desired
  property directly into your own `edge_query`'s `weight` column
  (`... RETURN id(a) AS src, id(b) AS dst, r.<property> AS weight`), and pass
  `weighted := true`.
- **Named/cached graph mode** (`graph_name`): pass
  `relationship_weight_property => '<property>'`. Internally this re-runs the
  projection's stored `edge_query` against AGE for that property and applies
  the result to a private, reweighted copy of the cached graph for the
  duration of the call — the shared cache entry and every other backend's
  view of it are unaffected. Requires the query convention above
  (`RETURN id(a) AS src, id(b) AS dst, <expr> AS weight`, with a single
  relationship alias bound in the `MATCH` clause); raises an error if the
  named graph's catalog row has no recorded `edge_query` to substitute into
  (e.g. very old rows projected before this convention existed).
- `NULL` (the default) is always a no-op: the call behaves exactly as if the
  parameter were omitted.

Currently available on: `pageRank_stream`, `pageRank_stats`,
`betweenness_stream`, `degree_stream`, `eigenvector_stream`,
`articleRank_stream`, `louvain_stream`, `leiden_stream`,
`labelPropagation_stream`, `wcc_stream`, `modularity_stats`,
`shortestPath_dijkstra_stream`, `allShortestPaths_dijkstra_stream`,
`bellmanFord_stream`, `bellmanFord_stats`, `randomWalk_stream`,
`spanningTree_stream`, `allShortestPaths_stream`,
`shortestPath_astar_stream`, `nodeSimilarity_stream` (computes weighted
Jaccard rather than reweighting a NetworKit class -- see §7 for the
`similarity_metric = 'JACCARD'`-only restriction). `maxFlow_stats`'s
`capacity_property` reuses this exact same mechanism (it is, structurally,
a relationship-weight override -- `EdmondsKarp`/`Dinic` already treat graph
weight as capacity). Other algorithms adopt the same trailing parameter as
their own plans are implemented.

Note: passing the exact same property name the graph's projection already
uses for its `weight` column is a degenerate case for the override's
substitution logic (it can't distinguish "already this value" from "no
weight projection found" by string comparison) and raises an error rather
than silently no-oping; pick a different property, or don't pass the
override, in that situation.

See also: [Plan: Per-call Relationship Weight Plumbing](implemented-plans/00-weight-plumbing.md),
[Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md),
[Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

### Node-property read (`seed_property` / `community_property` / `triangle_count_property`)

A few community-detection functions read a *node* property (rather than a
relationship property) via the projection's stored `node_query`, in the same
spirit as the relationship-weight override above:

- **Named/cached graph mode** (`graph_name`): requires the graph's catalog
  row to have both `age_graph` and `node_query` recorded (populated by
  `graph_project`; **not** currently populated by `graph_index()` for live
  indices -- see [Plan: Community Detection Algorithm Parity
  Fixes](implemented-plans/02-community-detection-parity.md)). Requires
  `node_query` to follow the convention `RETURN id(<alias>) AS id`, with a
  single node alias bound in the `MATCH` clause; raises an error otherwise.
- Nodes without a value for the property default to their own singleton
  group (never grouped with unrelated nodes), so the result is always a
  fully valid `NetworKit::Partition`.
- `modularity_stats`'s `community_property` and
  `localClusteringCoefficient_stream`'s `triangle_count_property` are
  implemented this way. `labelPropagation_stream`'s `seed_property` is
  **not implemented** -- see that function's entry below for why.

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

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
age_gds.graph_reindex(
    name  text,
    force boolean DEFAULT NULL   -- NULL = mode-appropriate default
) RETURNS void
```

Behaviour depends on the projection's storage kind (`incremental`) and mode
(`live`):

| Storage                                   | `force` NULL (default) | `force = true`                          | `force = false`                          |
|--------------------------------------------|-------------------------|------------------------------------------|-------------------------------------------|
| flat (`graph_project` default)             | full rebuild            | full rebuild (accepted, no-op vs. default) | **error** — flat storage has no incremental mode |
| incremental, non-live (`graph_project(incremental := true)`) | diff-and-apply | full rebuild (fresh slot files) | diff-and-apply (explicit, same as default) |
| incremental, live (`graph_index`)          | compact                 | full re-load from AGE (rediscover labels, rebuild slot files, triggers stay installed) | compact (explicit, same as default) |

- **Full rebuild (flat)**: re-runs the stored `node_query`/`edge_query` and
  rewrites the `.bin` file, bumping `content_version` so every backend's
  session cache notices and reloads on next use (see
  [Session cache](architecture.md#session-cache)).

- **Diff-and-apply (incremental, non-live)**: re-runs `node_query`/`edge_query`
  and diffs the result against the current slot/tombstone tables by identity
  (AGE graphid for nodes, `(src_gid, dst_gid)` for edges). Adds/removes only
  the delta; an edge present in both with a changed weight is updated in place
  rather than tombstoned and re-appended. Bumps `version_counter` once.

- **Compact (live, default)**: rewrites the `.nodes`/`.edges` files without
  tombstoned entries. Does **not** reload data from AGE; only removes stale
  entries from the existing files.

- **Full re-load (live, `force := true`)**: rebuilds the slot files from a
  fresh whole-graph AGE scan (same as `graph_index`'s initial load) and
  installs triggers for any newly-discovered labels. Existing triggers are
  left in place.

All modes invalidate the calling backend's own session cache immediately;
every other backend's cache notices the `content_version`/`version_counter`
change on its own next access — no cross-backend signal is needed.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.
- `ERRCODE_INVALID_PARAMETER_VALUE` — `force := false` on a flat projection.
- `ERRCODE_INTERNAL_ERROR` — file I/O failure.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md), [Plan 10: Cross-Backend Cache Invalidation](implemented-plans/10-snapshot-version-cache.md)

---

### `age_gds.graph_project`

```sql
age_gds.graph_project(
    name        text,
    age_graph   text,
    node_query  text,
    edge_query  text,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false,
    incremental boolean DEFAULT false
) RETURNS void
```

Projects an AGE Cypher graph into a persistent NetworKit graph. Execution:

1. Runs `node_query` against `age_graph` via `ag_catalog.cypher()`. The query
   must return one column named `id` of type `agtype` containing the AGE
   graphid of each node.
2. Runs `edge_query` against `age_graph`. The query must return three columns:
   `src agtype`, `dst agtype`, `weight agtype`. The `weight` column is used
   only when `weighted = true`; otherwise edge weight defaults to `1.0`.
3. Serializes the resulting `NetworKit::Graph`:
   - `incremental = false` (default): to a flat `<age_gds.data_dir>/<name>.bin`
     file. Only a full rebuild (`graph_reindex`) can refresh it.
   - `incremental = true`: to slot/tombstone `<name>.nodes`/`<name>.edges`
     files — the same storage format live indices use, without installing any
     AGE triggers. `graph_reindex` can then diff-and-apply instead of
     rebuilding from scratch.
4. Inserts a row into `age_gds.graphs` with the projection metadata.

The operation is transactional: if the transaction is aborted, the created
file(s) are deleted by a registered `XactCallback`.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — `age_graph` does not exist in `ag_catalog.ag_graph`.
- `ERRCODE_INTERNAL_ERROR` — serialization or SPI failure.
- Duplicate `name` raises a unique-constraint violation from `age_gds.graphs`.

See also: [Plan 10: Cross-Backend Cache Invalidation](implemented-plans/10-snapshot-version-cache.md)

---

### `age_gds.graph_drop`

```sql
age_gds.graph_drop(name text) RETURNS void
```

Drops the projection:
- For `incremental = false` (flat) projections: deletes the catalog row,
  unlinks the `.bin` file, and evicts the entry from the calling backend's
  session cache.
- For `incremental = true` projections (`graph_index`, or `graph_project(
  incremental := true)`): unlinks the `.nodes`/`.edges` files instead.
- For **live index** (`live = true`) projections specifically: additionally
  drops all data triggers installed on AGE label tables for this index
  (entries in `age_gds.index_triggers` are cascade-deleted).

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
    file_path     text,
    created_at    timestamptz,
    "memoryUsage" bigint,
    live          boolean,
    incremental   boolean
)
```

Returns all rows from `age_gds.graphs`, ordered by `created_at`. The
`live` column is `true` for live-index projections and `false` otherwise. The
`incremental` column is `true` for slot/tombstone storage (`graph_index`, or
`graph_project(incremental := true)`) and `false` for flat `.bin` storage
(`live` implies `incremental`, not vice versa). The `memoryUsage` column is a
derived estimate: `node_count * 8 + edge_count * 20` bytes (approximate
in-memory cost of the deserialized graph).

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
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Computes PageRank scores. Delegates to `NetworKit::PageRank`.

**Parameters:**
- `damping_factor` — teleportation probability, default `0.85`.
- `max_iterations` — maximum power-iteration steps, default `20`.
- `tolerance` — convergence threshold, default `1e-7`.
- `config` — JSONB object; accepted keys: `"dampingFactor"`, `"maxIterations"`,
  `"tolerance"`. Named parameters override JSONB values when both are present.
- `relationship_weight_property` — see
  [Relationship weight override](#relationship-weight-override-weight_property--relationship_weight_property)
  above. Only meaningful with `graph_name` (named/cached graphs); ignored (via
  `apply_weight_override`'s anonymous-mode no-op) when using `age_graph`.
- `scaler` — optional post-processing pass applied to the score vector after
  PageRank has run, matching Neo4j GDS's `scaler` convention: one of
  `'MinMax'`, `'Max'`, `'Mean'`, `'Log'`, `'StdScore'`. `NULL` (default) skips
  scaling. An unrecognized value raises `ERRCODE_INVALID_PARAMETER_VALUE`.
  Implemented once in `src/util/scaler.h`/`.cpp`, shared with
  `eigenvector_stream` and `articleRank_stream`.

Scores sum to approximately 1.0 (before any `scaler`). All scores are
non-negative before scaling; `Log`/`StdScore` scaling can produce negative
or zero values.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

### `age_gds.pageRank_stats`

```sql
age_gds.pageRank_stats(
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
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

`relationship_weight_property` and `scaler` behave identically to
`pageRank_stream` (see above); `minScore`/`maxScore`/`meanScore` are computed
*after* `scaler` is applied.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

### `age_gds.betweenness_stream`

```sql
age_gds.betweenness_stream(
    graph_name                    text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL,
    sampling_size                 int  DEFAULT NULL,
    sampling_seed                 int  DEFAULT NULL,
    relationship_weight_property  text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Betweenness centrality: fraction of shortest paths passing through each node.

**Parameters:**
- `sampling_size` — when set (must be a positive integer), switches from
  exact `NetworKit::Betweenness` to `NetworKit::ApproxBetweenness`, matching
  Neo4j GDS's approximate-betweenness `samplingSize` convention. Internally
  maps to `epsilon = 1 / sqrt(sampling_size)` with a fixed `delta = 0.1`.
  **Requires an undirected graph** — `ApproxBetweenness`'s diameter-estimation
  step does not support directed input and raises an error there (a
  NetworKit limitation, not an age_gds restriction).
- `sampling_seed` — only meaningful together with `sampling_size`; calls
  `Aux::Random::setSeed()` before running, for reproducible sampling.
- `relationship_weight_property` — see
  [Relationship weight override](#relationship-weight-override-weight_property--relationship_weight_property)
  above.

**NetworKit class:** `NetworKit::Betweenness` (normalized=false) or, when
`sampling_size` is set, `NetworKit::ApproxBetweenness`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

### `age_gds.closeness_stream`

```sql
age_gds.closeness_stream(
    graph_name          text    DEFAULT NULL,
    age_graph           text    DEFAULT NULL,
    node_query          text    DEFAULT NULL,
    edge_query          text    DEFAULT NULL,
    use_wasserman_faust boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, score float8)
```

Closeness centrality. `use_wasserman_faust = false` (default) uses the
`GENERALIZED` variant, which handles disconnected graphs without dividing by
zero; `use_wasserman_faust = true` uses the `STANDARD` (Wasserman-Faust)
variant, matching Neo4j GDS's `useWassermanFaust` parameter.

**NetworKit class:** `NetworKit::Closeness` (variant=GENERALIZED or STANDARD)

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

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
    graph_name                    text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL,
    orientation                   text DEFAULT 'NATURAL',
    relationship_weight_property  text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Degree centrality (unnormalized node degree).

**Parameters:**
- `orientation` — one of `'NATURAL'` (out-degree, default), `'REVERSE'`
  (in-degree), or `'UNDIRECTED'` (in-degree + out-degree; on an already
  undirected graph this equals the plain degree). Matches Neo4j GDS's degree
  centrality `orientation` parameter. Any other value raises
  `ERRCODE_INVALID_PARAMETER_VALUE`.
- `relationship_weight_property` — see
  [Relationship weight override](#relationship-weight-override-weight_property--relationship_weight_property)
  above. When set, weighted degree is computed directly via
  `Graph::weightedDegree()`/`weightedDegreeIn()` (no `DegreeCentrality`
  instance is needed for the weighted case — those are already O(1) per
  node on the graph object).

**NetworKit class:** `NetworKit::DegreeCentrality` (normalized=false), or
`Graph::weightedDegree()`/`weightedDegreeIn()` directly when
`relationship_weight_property` is set

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

### `age_gds.eigenvector_stream`

```sql
age_gds.eigenvector_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Eigenvector centrality: node importance proportional to the importance of its
neighbors.

**Parameters:**
- `tolerance` — convergence threshold passed to
  `NetworKit::EigenvectorCentrality`'s constructor, default `1e-7`.
- `relationship_weight_property` — see
  [Relationship weight override](#relationship-weight-override-weight_property--relationship_weight_property)
  above.
- `scaler` — see `pageRank_stream`'s `scaler` parameter above; same allowed
  values, same shared implementation.

**Caveat:** `NetworKit::EigenvectorCentrality`'s power iteration has no
damping term. On a directed graph that is not irreducible (e.g. a directed
acyclic graph), the propagated score vector's magnitude can decay to zero,
which trips an internal NetworKit assertion. This is a limitation of the
vendored class on non-irreducible directed graphs, not an age_gds
restriction; prefer running this on undirected graphs, or directed graphs
known to be strongly connected.

**NetworKit class:** `NetworKit::EigenvectorCentrality`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

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
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

ArticleRank centrality.

**Parameters:**
- `damping_factor` — default `0.85`.
- `tolerance` — default `1e-7` (previously hardcoded to `1e-6`, which did not
  match Neo4j GDS's `1e-7` default — fixed as part of
  [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)).
- `relationship_weight_property` — see
  [Relationship weight override](#relationship-weight-override-weight_property--relationship_weight_property)
  above.
- `scaler` — see `pageRank_stream`'s `scaler` parameter above; same allowed
  values, same shared implementation.

`max_iterations` is not exposed (fixed internally at `20`) — matches
`pageRank_stream`'s default and NetworKit's `PageRank` has no separate
ArticleRank-specific iteration cap.

**Substitution:** NetworKit has no ArticleRank class. `NetworKit::PageRank`
is used, with the parameters above.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

### `age_gds.articulationPoints_stream`

```sql
age_gds.articulationPoints_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(
    "nodeId"                  bigint,
    "resultingComponentCount" int,
    "resultingComponentMin"   int,
    "resultingComponentMax"   int
)
```

Returns the set of articulation points (cut vertices) — nodes whose removal
increases the number of connected components — one row per articulation
point (not one row per graph node).

**Columns:**
- `nodeId` — the articulation point's AGE graphid.
- `resultingComponentCount`/`resultingComponentMin`/`resultingComponentMax`
  — the number of connected components, and the smallest/largest component
  size, that result from removing this single node from the graph. Computed
  by running `NetworKit::ConnectedComponents` on a private copy of the graph
  (`Graph::removeNode()`) for each reported articulation point; matches
  Neo4j GDS's articulation-points procedure returning per-point
  `componentSizes`/`componentCount`.

**Substitution:** NetworKit has no dedicated ArticulationPoints class. The
point set is computed via `BiconnectedComponents`: a node is an articulation
point if and only if `getComponentsOfNode(u).size() > 1`.

**NetworKit classes:** `NetworKit::BiconnectedComponents`,
`NetworKit::ConnectedComponents`

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

---

## 5. Community detection algorithms

All community/structure functions return one row per node.

---

### `age_gds.louvain_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.leiden_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.labelPropagation_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.wcc_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.scc_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.localClusteringCoefficient_stream`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

### `age_gds.modularity_stats`

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

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

## 6. Path-finding algorithms

---

### `age_gds.bfs_stream`

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

---

### `age_gds.shortestPath_dijkstra_stream`

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

---

### `age_gds.allShortestPaths_dijkstra_stream`

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
is used as an equivalent for graphs with non-negative edge weights. Excluded
from `docs/implemented-plans/03-pathfinding-parity.md` (no `delta` parameter
to add; tracked in `docs/neo4j-gds-parity-external-libs.md`).

**NetworKit class:** `NetworKit::Dijkstra`

---

### `age_gds.bellmanFord_stream`

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

---

### `age_gds.bellmanFord_stats`

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

---

### `age_gds.dfs_stream`

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

---

### `age_gds.randomWalk_stream`

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

---

### `age_gds.spanningTree_stream`

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

---

### `age_gds.allShortestPaths_stream`

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

---

### `age_gds.shortestPath_astar_stream`

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

---

### `age_gds.maxFlow_stats`

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

---

### `age_gds.minCostMaxFlow_stats`

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

---

See also: [Plan: Path-finding & Flow Algorithm Parity Fixes](implemented-plans/03-pathfinding-parity.md)

---

## 7. Similarity

See also: [Plan: Similarity & Link-Prediction Parity Fixes](implemented-plans/04-similarity-linkprediction-parity.md)

---

### `age_gds.nodeSimilarity_stream`

```sql
age_gds.nodeSimilarity_stream(
    graph_name                    text     DEFAULT NULL,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    similarity_cutoff             float8   DEFAULT 1e-42,
    degree_cutoff                 int      DEFAULT 1,
    top_k                         int      DEFAULT 10,
    top_n                         int      DEFAULT 0,
    relationship_weight_property  text     DEFAULT NULL,
    upper_degree_cutoff           bigint   DEFAULT 9223372036854775807,
    bottom_k                      int      DEFAULT 10,
    bottom_n                      int      DEFAULT 0,
    source_node_filter            bigint[] DEFAULT NULL,
    target_node_filter            bigint[] DEFAULT NULL,
    use_components                boolean  DEFAULT false,
    similarity_metric             text     DEFAULT 'JACCARD'
) RETURNS TABLE(node1 bigint, node2 bigint, similarity float8)
```

Similarity for node pairs, computed per source node's **bounded top-K /
bottom-K neighbor lists** rather than the full `O(n²)` pairwise matrix (see
`docs/architecture.md`'s "Similarity: bounded top-K/bottom-K heaps"
section for why). `similarity` is in `[0.0, 1.0]` for `JACCARD`/`OVERLAP`/
`COSINE`.

- `similarity_cutoff`: post-filter -- a scored pair is discarded if
  `similarity < similarity_cutoff`. Default `1e-42` effectively means
  "greater than zero".
- `degree_cutoff` / `upper_degree_cutoff`: pre-filter -- a node is only
  considered as a candidate if its degree is within
  `[degree_cutoff, upper_degree_cutoff]` (inclusive). Default excludes only
  isolated (degree-0) nodes.
- `top_k` / `top_n`: for each source node, keep only its `top_k` highest-scoring
  partners (bounded per-node max-heap, `O(n·top_k)` memory instead of
  materializing every pair); `top_n` (`0` = unlimited) then caps the *total*
  number of top-K rows returned across all nodes, globally, by score.
- `bottom_k` / `bottom_n`: same pattern, inverted -- keeps each node's
  `bottom_k` *lowest*-scoring partners (this is Neo4j's Filtered Node
  Similarity feature, folded into this same function rather than a separate
  `_filtered` function). **A default call returns up to `top_k + bottom_k`
  (default `20`) rows per source node** -- the highest- and lowest-scoring
  partners, deduplicated against each other when a node has few enough
  candidates that the two sets would otherwise overlap. Pass `bottom_k => 0`
  to get only the `top_k` behavior most callers expect.
- `source_node_filter` / `target_node_filter` (`bigint[]`, AGE node ids):
  when either is given, only pairs with `node1` drawn from
  `source_node_filter` (or all degree-eligible nodes, if omitted) and
  `node2` drawn from `target_node_filter` (or all degree-eligible nodes, if
  omitted) are considered; unlike the unfiltered mode, this makes each row
  directional (a pair might only appear as `(a, b)`, not also `(b, a)`).
  Raises an error if a listed node id is not in the graph.
- `use_components`: when `true`, runs `NetworKit::ConnectedComponents`
  first and only compares pairs within the same component.
- `similarity_metric`: `'JACCARD'` (default, `|A∩B|/|A∪B|`), `'OVERLAP'`
  (`|A∩B|/min(|A|,|B|)`), or `'COSINE'` (`|A∩B|/sqrt(|A|·|B|)`) over 0/1
  neighbor-membership vectors. Any other value raises an error.
- `relationship_weight_property`: computes **weighted Jaccard**
  (`sum(min(w_u,w_v)) / sum(max(w_u,w_v))` over the union of `u`'s and `v`'s
  neighbor edge weights) instead of the unweighted class -- closed-form
  arithmetic over the graph's edge weights, since NetworKit's `JaccardIndex`
  has no weighted variant. Only valid with `similarity_metric = 'JACCARD'`
  (raises an error otherwise); also raises an error if the resulting graph
  is not weighted (see the common "Relationship weight override" section
  above -- for anonymous graphs, `relationship_weight_property` is a no-op
  by design, so make sure `edge_query`/`weighted` already produce a
  weighted graph).
- Any directed input graph is converted to an undirected view first
  (`NetworKit::GraphTools::toUndirected`), uniformly for every
  `similarity_metric` -- `NetworKit::LinkPredictor::run()` (used for
  unweighted `JACCARD`) unconditionally rejects directed graphs, so this
  keeps behavior consistent across metrics instead of varying by which one
  happens to be requested.

**NetworKit classes:** `NetworKit::JaccardIndex` (unweighted `JACCARD`),
`NetworKit::NeighborhoodUtility` (`OVERLAP`/`COSINE`/weighted `JACCARD` --
the same helper `JaccardIndex` uses internally), `NetworKit::ConnectedComponents`
(`use_components`), `NetworKit::GraphTools::toUndirected` (directed input).

---

### `age_gds.knn_stream`

```sql
age_gds.knn_stream(
    graph_name         text   DEFAULT NULL,
    top_k              int    DEFAULT 10,
    age_graph          text   DEFAULT NULL,
    node_query         text   DEFAULT NULL,
    edge_query         text   DEFAULT NULL,
    similarity_cutoff  float8 DEFAULT 0
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, similarity float8, rank int)
```

K-nearest neighbors by Jaccard similarity. For each node returns up to `top_k`
most similar neighbors (score `>= similarity_cutoff`) sorted descending by
similarity. `rank` is 1-based. `top_k` default changed from `5` to `10` to
match Neo4j's `gds.knn.stream` default.

This remains a **topology-Jaccard substitution** for Neo4j's actual
`gds.knn.stream` semantics (K-nearest-neighbors over **node property
vectors**, not graph topology) -- a genuine NetworKit gap (no
property-vector / approximate-NN class); see
`docs/neo4j-gds-parity-external-libs.md` for the adopted-library plan to
close it (hnswlib), which is a separate, additive function, not a
modification of this one.

**NetworKit class:** `NetworKit::JaccardIndex`

---

## 8. Link prediction

`*_stream` functions score **all currently unconnected node pairs** using
`runAll()` and return one row per pair. The single-pair scalar functions
below (no `_stream` suffix, added by plan 04) instead score exactly one
`(node1, node2)` pair via the same predictor classes' base
`run(u, v)` method, matching Neo4j GDS's
`gds.alpha.linkprediction.<name>(node1, node2, {relationshipQuery,
direction})` spot-check shape. Both variants are kept -- the streaming form
is not deprecated.

See also: [Plan: Similarity & Link-Prediction Parity Fixes](implemented-plans/04-similarity-linkprediction-parity.md)

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

### `age_gds.linkprediction_adamicAdar` / `commonNeighbors` / `preferentialAttachment` / `resourceAllocation`

```sql
age_gds.linkprediction_adamicAdar(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
```
(same signature for `commonNeighbors`/`preferentialAttachment`/`resourceAllocation`)

Single-pair scalar score for exactly `(node1, node2)`. `graph_name` is
required -- these always run against an already-projected graph (there is no
`age_graph`/`node_query`/`edge_query` anonymous mode; use `relationship_query`
below for a per-call topology restriction instead).

- `relationship_query`: when given, re-projects the named graph's topology
  against this edge query instead of the one it was originally projected
  with -- this is an **actual different edge set**, not a weight
  substitution (unlike `relationship_weight_property` elsewhere), reusing
  the exact same anonymous-load mechanism `edge_query` provides elsewhere.
  Requires the graph's catalog row to have `age_graph`/`node_query`
  recorded (as usual for anything that needs to re-run a query against the
  projection's source graph).
- `direction`: `'BOTH'` (default), `'OUTGOING'`, or `'INCOMING'`.
  **Only `'BOTH'` is supported.** `NetworKit::LinkPredictor::run()`
  unconditionally throws `std::invalid_argument("Only undirected graphs
  accepted.")` whenever the graph is directed, and none of the four
  predictor classes offer a directed-aware alternative -- there is no
  NetworKit class to delegate an `OUTGOING`/`INCOMING`-restricted score to,
  and computing it by hand would mean hand-rolling directed neighbor-set
  arithmetic (forbidden by project rules). Passing `'OUTGOING'` or
  `'INCOMING'` raises a clear error rather than silently ignoring the
  parameter or computing an unsupported result; a directed input graph is
  still automatically converted to an undirected view for `'BOTH'`.

**NetworKit classes:** `NetworKit::AdamicAdarIndex` /
`NetworKit::CommonNeighborsIndex` / `NetworKit::PreferentialAttachmentIndex` /
`NetworKit::ResourceAllocationIndex` (base `LinkPredictor::run(u, v)`).

---

## 9. Embeddings

### `age_gds.node2vec_stream`

```sql
age_gds.node2vec_stream(
    graph_name                    text    DEFAULT NULL,
    embedding_dim                 int     DEFAULT 128,
    walk_length                   int     DEFAULT 80,
    walks_per_node                int     DEFAULT 10,
    p                             float8  DEFAULT 1.0,
    q                             float8  DEFAULT 1.0,
    age_graph                     text    DEFAULT NULL,
    node_query                    text    DEFAULT NULL,
    edge_query                    text    DEFAULT NULL,
    relationship_weight_property  text    DEFAULT NULL,
    window_size                   int     DEFAULT 10,
    iterations                    int     DEFAULT 1
) RETURNS TABLE("nodeId" bigint, embedding float8[])
```

Node2Vec biased random-walk embeddings. Returns one row per node with a
`float8[]` array of length `embedding_dim`.

**Parameters:**
- `embedding_dim` — vector dimensionality. **Default changed from 64 to 128**
  (plan 05) to match Neo4j GDS's `embeddingDimension` default.
- `walk_length` — length of each random walk (L), default 80.
- `walks_per_node` — number of walks per node (N), default 10.
- `p` — return parameter (controls likelihood of revisiting the previous node),
  default 1.0.
- `q` — in-out parameter (controls BFS vs DFS tendency), default 1.0.
- `relationship_weight_property` (plan 05) — optional edge weight property
  used when computing walk transition probabilities. `NetworKit::Embedding::
  BiasedRandomWalk::preprocessNode()` reads edge weights directly (confirmed
  by reading `networkit/cpp/embedding/BiasedRandomWalk.cpp`), so this behaves
  like every other `relationship_weight_property` parameter in this API (see
  `docs/architecture.md`'s weight-override section) -- named graphs get a
  reweighted private copy, anonymous graphs must bake the weight into
  `edge_query`'s `AS weight` projection instead. **Known limitation shared
  with every other `relationship_weight_property` parameter, not specific to
  `node2vec_stream`:** if the projection's stored `edge_query` already reads
  `AS weight` from a property of the same name being requested (e.g.
  `edge_query` projects `r.weight AS weight` and the caller passes
  `relationship_weight_property => 'weight'`), the rewritten query is
  textually identical to the original and `reweight_from_property()`
  mistakes that for "no `AS weight` projection found," raising an error
  instead of a no-op. Project under a different placeholder expression (e.g.
  `1.0 AS weight`) if the override property may collide with the base
  projection's name.
- `window_size` (plan 05) — skip-gram context window size, default 10.
- `iterations` (plan 05) — skip-gram training epochs, default 1.

Parameters ≤ 0 are silently reset to their defaults.

**NetworKit functions:** `NetworKit::Embedding::BiasedRandomWalk` (walk
generation) and `NetworKit::Embedding::learnEmbeddings` (skip-gram trainer),
called directly rather than through the public `NetworKit::Node2Vec` wrapper.
`Node2Vec::run()` hardcodes `winSize=10, iterations=1` when it calls
`learnEmbeddings()` and does not expose them as parameters (the class is also
`final`, so it cannot be subclassed to override this) -- both are the same
real, vendored NetworKit functions `Node2Vec::run()` itself calls internally,
just re-exposed with configurable `window_size`/`iterations`. This mirrors
the precedent set by `randomWalk_stream` reaching into
`networkit/cpp/embedding/` for `BiasedRandomWalk` (see
`docs/implemented-plans/03-pathfinding-parity.md`).

Excluded, out of scope: `negativeSamplingRate` -- hardcoded inside
`LearnEmbedding.cpp` ("value taken from original word2vec code"), not
exposed as a parameter by NetworKit; would require a NetworKit-side change.
FastRP and GraphSAGE have no NetworKit class (tracked in
`docs/neo4j-gds-parity-external-libs.md`).

See also: [Plan: Embedding algorithm parity fixes](implemented-plans/05-embeddings-parity.md)

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
