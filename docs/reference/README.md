# age_gds Function Reference

All functions live in the `age_gds` schema. Install with `CREATE EXTENSION age_gds`.

Every function documented in this reference has two calling forms: a plain
SQL form (`SELECT * FROM age_gds.<name>(...)`), and a Cypher-callable
`(agtype, agtype)` overload usable as a scalar expression inside a
`cypher()` block. See "Cypher-callable overloads" below for the shared
calling convention, and each theme file for the per-function config keys and
output shapes.

See also: [Plan: Per-Theme Function Reference Docs](../implemented-plans/function-reference-docs.md)

## Theme files

| Theme | Functions |
|---|---|
| [Projection management](projection-management.md) | [`graph_index`](projection-management.md#age_gdsgraph_index), [`graph_reindex`](projection-management.md#age_gdsgraph_reindex), [`graph_project`](projection-management.md#age_gdsgraph_project), [`graph_drop`](projection-management.md#age_gdsgraph_drop), [`graph_list`](projection-management.md#age_gdsgraph_list), [`graph_exists`](projection-management.md#age_gdsgraph_exists), [`graph_is_stale`](projection-management.md#age_gdsgraph_is_stale) |
| [Session cache](session-cache.md) | [`cache_stats`](session-cache.md#age_gdscache_stats), [`cache_load`](session-cache.md#age_gdscache_load) |
| [Utility](utility.md) | [`version`](utility.md#age_gdsversion) |
| [Centrality](centrality.md) | [PageRank](centrality.md#age_gdspagerank_stream), [Betweenness](centrality.md#age_gdsbetweenness_stream), [Closeness](centrality.md#age_gdscloseness_stream), [Harmonic](centrality.md#age_gdsharmoniccentrality_stream), [Degree](centrality.md#age_gdsdegree_stream), [Eigenvector](centrality.md#age_gdseigenvector_stream), [HITS](centrality.md#age_gdshits_stream), [ArticleRank](centrality.md#age_gdsarticlerank_stream), [Articulation Points](centrality.md#age_gdsarticulationpoints_stream) |
| [Community detection](community-detection.md) | [Louvain](community-detection.md#age_gdslouvain_stream), [Leiden](community-detection.md#age_gdsleiden_stream), [Label Propagation](community-detection.md#age_gdslabelpropagation_stream), [WCC](community-detection.md#age_gdswcc_stream), [SCC](community-detection.md#age_gdsscc_stream), [k-Core](community-detection.md#age_gdskcore_stream), [Triangle Count](community-detection.md#age_gdstrianglecount_stream), [Local Clustering Coefficient](community-detection.md#age_gdslocalclusteringcoefficient_stream), [Modularity](community-detection.md#age_gdsmodularity_stats), [Conductance](community-detection.md#age_gdsconductance_stats), [Clique Counting](community-detection.md#age_gdscliquecounting_stats) |
| [Path-finding](path-finding.md) | [BFS](path-finding.md#age_gdsbfs_stream), [Dijkstra](path-finding.md#age_gdsshortestpath_dijkstra_stream), [Dijkstra all-pairs](path-finding.md#age_gdsallshortestpaths_dijkstra_stream), [Delta-stepping](path-finding.md#age_gdsdeltastepping_stream), [Bellman-Ford](path-finding.md#age_gdsbellmanford_stream), [Bellman-Ford stats](path-finding.md#age_gdsbellmanford_stats), [DFS](path-finding.md#age_gdsdfs_stream), [Random Walk](path-finding.md#age_gdsrandomwalk_stream), [Spanning Tree](path-finding.md#age_gdsspanningtree_stream), [All-Pairs SP](path-finding.md#age_gdsallshortestpaths_stream), [A*](path-finding.md#age_gdsshortestpath_astar_stream), [Max Flow](path-finding.md#age_gdsmaxflow_stats), [Min-Cost Max Flow](path-finding.md#age_gdsmincostmaxflow_stats), [Topological Sort](path-finding.md#age_gdstopologicalsort_stream), [Longest Path](path-finding.md#age_gdslongestpath_stream), [Bridges](path-finding.md#age_gdsbridges_stream) |
| [Similarity](similarity.md) | [Node Similarity](similarity.md#age_gdsnodesimilarity_stream), [KNN](similarity.md#age_gdsknn_stream) |
| [Link prediction](link-prediction.md) | [Adamic-Adar](link-prediction.md#age_gdslinkprediction_adamicadar_stream), [Common Neighbors](link-prediction.md#age_gdslinkprediction_commonneighbors_stream), [Preferential Attachment](link-prediction.md#age_gdslinkprediction_preferentialattachment_stream), [Resource Allocation](link-prediction.md#age_gdslinkprediction_resourceallocation_stream) (stream forms), plus their [single-pair scalar forms](link-prediction.md#age_gdslinkprediction_adamicadar--commonneighbors--preferentialattachment--resourceallocation) |
| [Embeddings](embeddings.md) | [Node2Vec](embeddings.md#age_gdsnode2vec_stream) |
| [Write-back](write-back.md) | [`pageRank_write`](write-back.md#age_gdspagerank_write), [`betweenness_write`](write-back.md#age_gdsbetweenness_write) |

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
[Plan: Per-call relationship weight plumbing](../implemented-plans/00-weight-plumbing.md)
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
Jaccard rather than reweighting a NetworKit class -- see
[similarity.md](similarity.md) for the `similarity_metric = 'JACCARD'`-only
restriction). `maxFlow_stats`'s `capacity_property` reuses this exact same
mechanism (it is, structurally, a relationship-weight override --
`EdmondsKarp`/`Dinic` already treat graph weight as capacity). Other
algorithms adopt the same trailing parameter as their own plans are
implemented.

Note: passing the exact same property name the graph's projection already
uses for its `weight` column is a degenerate case for the override's
substitution logic (it can't distinguish "already this value" from "no
weight projection found" by string comparison) and raises an error rather
than silently no-oping; pick a different property, or don't pass the
override, in that situation.

See also: [Plan: Per-call Relationship Weight Plumbing](../implemented-plans/00-weight-plumbing.md),
[Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md),
[Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

### Node-property read (`seed_property` / `community_property` / `triangle_count_property`)

A few community-detection functions read a *node* property (rather than a
relationship property) via the projection's stored `node_query`, in the same
spirit as the relationship-weight override above:

- **Named/cached graph mode** (`graph_name`): requires the graph's catalog
  row to have both `age_graph` and `node_query` recorded (populated by
  `graph_project`; **not** currently populated by `graph_index()` for live
  indices -- see [Plan: Community Detection Algorithm Parity
  Fixes](../implemented-plans/02-community-detection-parity.md)). Requires
  `node_query` to follow the convention `RETURN id(<alias>) AS id`, with a
  single node alias bound in the `MATCH` clause; raises an error otherwise.
- Nodes without a value for the property default to their own singleton
  group (never grouped with unrelated nodes), so the result is always a
  fully valid `NetworKit::Partition`.
- `modularity_stats`'s `community_property` and
  `localClusteringCoefficient_stream`'s `triangle_count_property` are
  implemented this way. `labelPropagation_stream`'s `seed_property` is
  **not implemented** -- see that function's entry in
  [community-detection.md](community-detection.md) for why.

See also: [Plan: Community Detection Algorithm Parity Fixes](../implemented-plans/02-community-detection-parity.md)

### nodeId values

All `nodeId` / `node1` / `node2` / `sourceNodeId` / `targetNodeId` columns
return the AGE internal graphid (`bigint`). These are the same values returned
by `id(n)` in Cypher.

### Unreachable nodes

Distance functions (BFS, Dijkstra, etc.) return `NULL` for the `distance`
column when a node is unreachable from the source. The `nodeId` column is
always non-NULL.

---

## Cypher-callable overloads

Every function documented in the theme files (except internal trigger
functions) also has an `(agtype, agtype)` overload usable as a scalar
expression inside a `cypher()` block — AGE's grammar has no `CALL ... YIELD`,
so consume `*_stream` results with `UNWIND` and `*_stats`/`*_write` results
(single maps) with plain dot access:

```cypher
UNWIND age_gds.pagerank_stream('proj', {dampingFactor: 0.85}) AS row
RETURN row.nodeId, row.score
```

**Function names are case-sensitive inside `cypher()`** (AGE does not fold
identifiers) — call any mixed-case function (`shortestPath_dijkstra_stream`,
`labelPropagation_stream`, `articleRank_stream`, ...) using its all-lowercase
spelling (`shortestpath_dijkstra_stream`). See
[docs/architecture.md's "Cypher-callable interface"
section](../architecture.md#cypher-callable-interface-srcutilcypher_agtypehcpp)
for the full design and
[docs/user-guide.md's "Calling algorithms from inside Cypher"
section](../user-guide.md#calling-algorithms-from-inside-cypher) for worked
examples.

The call shape is `age_gds.<name>(graphName agtype, config agtype DEFAULT
NULL)` for every function except:
- `graph_project`: `(graphName, nodeProjection, relationshipProjection,
  config)`, matching Neo4j's real 4-argument shape (see
  [projection-management.md](projection-management.md)).
- `graph_list`/`cache_stats`: zero-argument in SQL, so the agtype overload
  is a differently-named function, `graph_list_cypher()`/
  `cache_stats_cypher()` (Postgres can't overload two zero-arg functions by
  return type alone).

`config` keys are Neo4j's camelCase config names, not the SQL functions'
`snake_case` parameter names. Each theme file documents config keys and
output shape per function, in a **SQL** / **Cypher** pair per entry.
`agtype` scalar config values (strings, numbers, booleans, arrays) map
directly from Cypher's map-literal syntax, e.g. `{sourceNode: 12345,
maxDepth: 3, targetNodes: [1,2,3]}`.

See also: [Plan: Cypher-Callable Interface](../implemented-plans/cypher-callable-interface.md)

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
