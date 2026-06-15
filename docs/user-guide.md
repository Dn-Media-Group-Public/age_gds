# age_gds User Guide

## Prerequisites

- Apache AGE PostgreSQL extension installed and loadable in the target database.
- PostgreSQL 14 or later (AGE requirement).
- The `age_gds` extension built and installed into the PostgreSQL extension
  directories (`pg_config --pkglibdir` and `pg_config --sharedir/extension`).
- OpenMP runtime (`libomp-dev` or equivalent) available on the server.

---

## Installation

### 1. Initialize the NetworKit submodule

```bash
git submodule update --init --recursive
```

### 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 3. Install

```bash
sudo cmake --install build
```

This copies `age_gds.so` to `pg_config --pkglibdir`, and `age_gds.control`
and `age_gds--1.0.sql` to `pg_config --sharedir/extension`.

### 4. Verify NetworKit linkage

```bash
nm build/age_gds.so | grep -c '_ZN10NetworKit'
# must print a number > 0
```

If the count is 0, NetworKit is not linked into the binary. Do not proceed
until this passes.

### 5. Create the extension in PostgreSQL

```sql
CREATE EXTENSION age;     -- Apache AGE must be loaded first
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

CREATE EXTENSION age_gds;
SELECT age_gds.version();  -- should return '0.1.0'
```

---

## Quick start: project a graph and run PageRank

### Create an AGE graph and add data

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

SELECT create_graph('social');

SELECT * FROM cypher('social', $$
    CREATE (:Person {name: 'Alice'}),
           (:Person {name: 'Bob'}),
           (:Person {name: 'Carol'}),
           (:Person {name: 'Dave'})
$$) AS (result agtype);

SELECT * FROM cypher('social', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'})
    CREATE (a)-[:KNOWS]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('social', $$
    MATCH (a:Person {name:'Bob'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('social', $$
    MATCH (a:Person {name:'Carol'}), (b:Person {name:'Dave'})
    CREATE (a)-[:KNOWS]->(b)
$$) AS (result agtype);
```

### Project the graph

```sql
SELECT age_gds.graph_project(
    name       => 'social_proj',
    age_graph  => 'social',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);
```

### Run PageRank

```sql
SELECT * FROM age_gds.pageRank_stream(graph_name => 'social_proj')
ORDER BY score DESC;
```

```
 nodeId | score
--------+-------
  12345 | 0.312
  12346 | 0.287
  ...
```

### Inspect projection metadata

```sql
SELECT name, node_count, edge_count, "memoryUsage"
FROM age_gds.graph_list();
```

### Clean up

```sql
SELECT age_gds.graph_drop('social_proj');
```

---

## The two execution modes

### Persistent projection

A persistent projection serializes the graph to a `.bin` file and registers it
in the `age_gds.graphs` catalog. Any backend can later run algorithms against
it by name without re-querying AGE. The graph is cached in per-backend memory
on first use.

Use persistent projections when:
- You will run multiple algorithms on the same graph.
- Query latency matters (amortizes the AGE load cost).
- You want to manage and inspect projections explicitly.

```sql
-- Create
SELECT age_gds.graph_project('proj', 'myagegraph', node_q, edge_q);

-- Use (any number of times, any algorithm)
SELECT * FROM age_gds.pageRank_stream(graph_name => 'proj');
SELECT * FROM age_gds.louvain_stream(graph_name  => 'proj');

-- Drop when done
SELECT age_gds.graph_drop('proj');
```

### Anonymous (query-scoped) execution

The anonymous mode loads the graph from AGE, runs the algorithm, and discards
the graph when the function returns. Nothing is written to disk and the session
cache is not populated.

Use anonymous execution when:
- You need a one-off result.
- You do not want to manage projections.
- The graph is small or changes frequently.

```sql
SELECT * FROM age_gds.pageRank_stream(
    age_graph  => 'myagegraph',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);
```

Anonymous calls do not affect `cache_stats()`:

```sql
SELECT entry_count FROM age_gds.cache_stats();
-- returns 0 after anonymous calls
```

---

## Graph projection: query requirements

### Node query

Must return **one column named `id`** of type `agtype` containing the AGE
graphid of each node. Use the built-in Cypher `id()` function:

```cypher
MATCH (n:Person) RETURN id(n) AS id
```

Any subset of nodes is valid. Nodes referenced by edges but not returned by the
node query are silently skipped during edge loading.

### Edge query

Must return **three columns** in this exact order:

| Position | Column name (agtype) | Content |
|----------|----------------------|---------|
| 1 | `src` | AGE graphid of the source node |
| 2 | `dst` | AGE graphid of the destination node |
| 3 | `weight` | Edge weight (numeric, or any literal) |

The `weight` column is used only when `weighted = true`; for unweighted
projections the value is ignored and all edges receive weight `1.0`. The column
must still be present in the result. Edges referencing a `src` or `dst` not
returned by the node query are silently skipped.

Example with weights:

```cypher
MATCH (a:Person)-[r:KNOWS]->(b:Person)
RETURN id(a) AS src, id(b) AS dst, r.weight AS weight
```

Example for unweighted projection (literal weight is fine):

```cypher
MATCH (a)-[r]->(b)
RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight
```

---

## Directed and weighted projections

```sql
-- Directed + weighted
SELECT age_gds.graph_project(
    name       => 'roads',
    age_graph  => 'map',
    node_query => $$MATCH (n:City) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:City)-[r:ROAD]->(b:City)
                   RETURN id(a) AS src, id(b) AS dst, r.distance AS weight$$,
    directed   => true,
    weighted   => true
);
```

- `directed = true` — edges are one-directional in NetworKit. SCC (`scc_stream`)
  requires a directed graph.
- `weighted = true` — the `weight` column from the edge query is used.
  Dijkstra and MaxFlow use edge weights. BFS ignores weights (hop count only).

---

## Running algorithms

### Named-parameter style (recommended)

```sql
SELECT * FROM age_gds.pageRank_stream(
    graph_name     => 'social_proj',
    damping_factor => 0.85,
    max_iterations => 50
);
```

### JSONB config style (PageRank only)

```sql
SELECT * FROM age_gds.pageRank_stream(
    graph_name => 'social_proj',
    config     => '{"dampingFactor": 0.9, "maxIterations": 30, "tolerance": 1e-8}'::jsonb
);
```

Named parameters (`damping_factor`, `max_iterations`, `tolerance`) override
values in the `config` JSONB object when both are supplied.

### PageRank summary statistics

```sql
SELECT "ranIterations", "didConverge", "computeMillis",
       "minScore", "maxScore", "meanScore"
FROM age_gds.pageRank_stats(graph_name => 'social_proj');
```

### Path-finding algorithms (source node required)

BFS, Dijkstra, DFS, random walk, and max-flow require a `source_node`. This
must be an AGE graphid that exists in the projected graph.

```sql
-- Find the highest-PageRank node first
SELECT "nodeId" FROM age_gds.pageRank_stream(graph_name => 'social_proj')
ORDER BY score DESC LIMIT 1;

-- Use it as the BFS source
SELECT * FROM age_gds.bfs_stream(
    graph_name  => 'social_proj',
    source_node => 12345
)
ORDER BY distance;
```

### Distances: NULL means unreachable

All single-source path functions return `NULL` for the `distance` column
when a node is unreachable. All-pairs shortest paths also returns `NULL` for
unreachable pairs.

```sql
SELECT "nodeId", distance
FROM age_gds.bfs_stream(graph_name => 'social_proj', source_node => 12345)
WHERE distance IS NOT NULL;  -- only reachable nodes
```

---

## Write-back: persisting scores to AGE

Write-back functions compute an algorithm and write each node's score back to
the AGE graph as a node property via Cypher `SET`.

```sql
-- Compute PageRank and store as node property 'pagerank'
SELECT age_gds.pageRank_write(
    graph_name     => 'social_proj',
    write_property => 'pagerank'
);

-- Read the scores back via Cypher
SELECT * FROM cypher('social', $$
    MATCH (n:Person)
    RETURN n.name, n.pagerank
$$) AS t(name agtype, pagerank agtype)
ORDER BY pagerank::float DESC;
```

```sql
-- Betweenness write-back
SELECT age_gds.betweenness_write(
    graph_name     => 'social_proj',
    write_property => 'betweenness'
);
```

Write-back also works in anonymous mode:

```sql
SELECT age_gds.pageRank_write(
    age_graph      => 'social',
    node_query     => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query     => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                       RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    write_property => 'pr_score'
);
```

Write-back is idempotent; calling it a second time overwrites the property.

---

## Link prediction and similarity

These functions score pairs of nodes.

### Node similarity (Jaccard)

```sql
-- All pairs similarity
SELECT node1, node2, similarity
FROM age_gds.nodeSimilarity_stream(graph_name => 'social_proj')
ORDER BY similarity DESC
LIMIT 10;

-- K nearest neighbors
SELECT "sourceNodeId", "targetNodeId", similarity, rank
FROM age_gds.knn_stream(graph_name => 'social_proj', top_k => 3)
ORDER BY "sourceNodeId", rank;
```

### Link prediction

All link-prediction functions score currently unconnected node pairs. Higher
score = more likely that an edge will form.

```sql
SELECT node1, node2, score
FROM age_gds.linkprediction_adamicAdar_stream(graph_name => 'social_proj')
ORDER BY score DESC LIMIT 10;

SELECT node1, node2, score
FROM age_gds.linkprediction_commonNeighbors_stream(graph_name => 'social_proj')
ORDER BY score DESC LIMIT 10;

SELECT node1, node2, score
FROM age_gds.linkprediction_preferentialAttachment_stream(graph_name => 'social_proj')
ORDER BY score DESC LIMIT 10;

SELECT node1, node2, score
FROM age_gds.linkprediction_resourceAllocation_stream(graph_name => 'social_proj')
ORDER BY score DESC LIMIT 10;
```

**Performance note:** all similarity and link-prediction functions enumerate
all O(n²) node pairs. They are unsuitable for very large graphs.

---

## Node embeddings

`node2vec_stream` produces a dense vector representation for each node using
biased random walks.

```sql
SELECT "nodeId", embedding
FROM age_gds.node2vec_stream(
    graph_name     => 'social_proj',
    embedding_dim  => 64,
    walk_length    => 80,
    walks_per_node => 10,
    p              => 1.0,
    q              => 1.0
);
```

To verify dimensions:

```sql
SELECT "nodeId", array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name    => 'social_proj',
    embedding_dim => 16
);
```

**Parameters:**
- `p` (return parameter) < 1.0 makes the walk more likely to return to the
  previous node; > 1.0 makes it less likely.
- `q` (in-out parameter) < 1.0 biases toward BFS-like exploration; > 1.0
  biases toward DFS-like exploration.
- At `p = q = 1.0` the walk is a standard uniform random walk.

---

## Staleness detection

A projection becomes stale if the underlying AGE graph has been modified after
the projection was created.

```sql
-- Check if projection may be out of date
SELECT age_gds.graph_is_stale('social_proj');
```

Returns `true` if `pg_stat_user_tables.n_mod_since_analyze > 0` for the AGE
graph's `_ag_label_vertex` table. Returns `false` if no statistics are
available.

To refresh a stale projection, drop and re-project:

```sql
SELECT age_gds.graph_drop('social_proj');
SELECT age_gds.graph_project(
    name       => 'social_proj',
    age_graph  => 'social',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);
```

---

## Session cache behavior

Each backend (PostgreSQL process) maintains its own in-memory graph cache.
The cache is populated on first algorithm use and persists for the lifetime
of the backend connection.

```sql
-- Inspect current backend's cache
SELECT entry_count, total_bytes, limit_bytes
FROM age_gds.cache_stats();

-- Pre-warm the cache before a latency-sensitive query
SELECT age_gds.cache_load('social_proj');

-- Now the algorithm call will be a cache hit
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'social_proj');
```

LRU eviction runs automatically when the cache exceeds `age_gds.session_cache_mb`.
The memory estimate per graph is `node_count * 8 + edge_count * 20` bytes.

Anonymous execution never populates the cache:

```sql
SELECT age_gds.cache_stats();  -- entry_count = 0 even after anonymous calls
```

---

## GUC settings reference

Set these in `postgresql.conf` or with `SET`/`ALTER SYSTEM`.

### `age_gds.session_cache_mb` (default: 512)

```sql
SET age_gds.session_cache_mb = 1024;  -- allow 1 GB per backend
```

Per-backend graph cache size limit in MB. When a new graph would exceed this
limit, the least-recently-used graph is evicted first. Can be changed at any
time (USERSET context). Range: 1–65536.

### `age_gds.data_dir` (default: `$PGDATA/age_gds`)

```sql
-- Requires superuser (SUSET context)
ALTER SYSTEM SET age_gds.data_dir = '/var/lib/postgresql/age_gds_files';
SELECT pg_reload_conf();
```

Directory where `.bin` projection files are stored. `$PGDATA` is expanded to
the PostgreSQL data directory at runtime. The directory is created automatically
(with mode 0700) on first `graph_project` call.

---

## Common errors and their meanings

### `AGE graph "X" does not exist`

```
ERROR:  AGE graph "mygraph" does not exist
```

The `age_graph` parameter does not match any entry in `ag_catalog.ag_graph`.
Check graph name spelling and ensure AGE is loaded (`LOAD 'age'`).

### `projected graph "X" does not exist`

```
ERROR:  projected graph "myproj" does not exist
```

The `graph_name` does not match any row in `age_gds.graphs`. Either the
projection was never created, was dropped, or was created in a different
database. Use `SELECT age_gds.graph_exists('myproj')` to verify.

### `supply graph_name OR age_graph, not both`

```
ERROR:  supply graph_name OR age_graph, not both
```

Both `graph_name` and `age_graph` were passed. Use one or the other.

### `must supply graph_name or age_graph`

```
ERROR:  must supply graph_name or age_graph
```

Neither `graph_name` nor `age_graph` was supplied. All algorithm functions
require exactly one graph source.

### `age_graph requires node_query and edge_query`

```
ERROR:  age_graph requires node_query and edge_query
```

In anonymous mode, both `node_query` and `edge_query` must be non-NULL.

### `source_node is required`

```
ERROR:  source_node is required
```

A path-finding function (`bfs_stream`, `shortestPath_dijkstra_stream`, etc.)
was called without a `source_node` parameter, or `source_node` was NULL.

### `source_node NNN not in graph`

```
ERROR:  source_node 12345 not in graph
```

The provided `source_node` is a valid bigint but is not the AGE graphid of any
node in the projected graph. Verify that the node was included by the
`node_query` used during projection.

### `source_node and target_node must be different`

```
ERROR:  source_node and target_node must be different
```

`maxFlow_stats` was called with `source_node = target_node`.

### `node query failed (SPI code N)`

```
ERROR:  age_load_graph: node query failed (SPI code -1)
```

The Cypher node query could not be executed via SPI. Common causes: syntax
error in the Cypher query, or `search_path` not set to include `ag_catalog`.
Ensure `LOAD 'age'` and `SET search_path = ag_catalog, ...` are in effect.

### `cannot open /path/to/file.bin: No such file or directory`

The catalog row exists but the `.bin` file is missing (deleted manually or
the projection was made in a different `age_gds.data_dir`). Drop the projection
and re-project:

```sql
SELECT age_gds.graph_drop('myproj');
SELECT age_gds.graph_project(...);
```

### `set-valued function called in context that cannot accept a set`

```
ERROR:  set-valued function called in context that cannot accept a set
```

An algorithm function that returns a table (`_stream` or `_stats`) was called
in a context where a set is not allowed (e.g., as a scalar subquery without a
wrapping `SELECT`). Use:

```sql
SELECT * FROM age_gds.pageRank_stream(...);       -- correct
SELECT (age_gds.pageRank_stream(...))."nodeId";   -- incorrect
```

---

## Example: community detection workflow

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

-- Project
SELECT age_gds.graph_project(
    name       => 'g',
    age_graph  => 'mydb',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);

-- Louvain communities
SELECT "nodeId", "communityId"
FROM age_gds.louvain_stream(graph_name => 'g')
ORDER BY "communityId", "nodeId";

-- Count communities
SELECT "communityId", count(*) AS members
FROM age_gds.louvain_stream(graph_name => 'g')
GROUP BY "communityId"
ORDER BY members DESC;

-- Modularity score of the Louvain partition
SELECT modularity FROM age_gds.modularity_stats(graph_name => 'g');

-- Weakly connected components
SELECT count(DISTINCT "componentId") AS num_components
FROM age_gds.wcc_stream(graph_name => 'g');

-- K-core numbers
SELECT "nodeId", "coreNumber"
FROM age_gds.kCore_stream(graph_name => 'g')
ORDER BY "coreNumber" DESC;

-- Clean up
SELECT age_gds.graph_drop('g');
```

---

## Example: full pipeline with write-back

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

SELECT age_gds.graph_project(
    name       => 'net',
    age_graph  => 'mynet',
    node_query => $$MATCH (n:User) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:User)-[r:FOLLOWS]->(b:User)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => true
);

-- Run PageRank and write scores back
SELECT age_gds.pageRank_write(
    graph_name     => 'net',
    write_property => 'pagerank'
);

-- Run betweenness and write back
SELECT age_gds.betweenness_write(
    graph_name     => 'net',
    write_property => 'betweenness'
);

-- Query the enriched nodes via Cypher
SELECT * FROM cypher('mynet', $$
    MATCH (n:User)
    RETURN n.name, n.pagerank, n.betweenness
    ORDER BY n.pagerank DESC
    LIMIT 10
$$) AS t(name agtype, pr agtype, bc agtype);

SELECT age_gds.graph_drop('net');
```
