# age_gds

Graph data science for Apache AGE — powered by NetworKit, called from SQL.

`age_gds` is a PostgreSQL C extension that bridges [Apache AGE](https://age.apache.org/) (property graph storage) and [NetworKit](https://networkit.github.io/) (high-performance C++ graph algorithms). Write a Cypher query to select your nodes and edges; get PageRank, community detection, shortest paths, embeddings, and more back as SQL rows — no external graph engine, no ETL pipeline, no separate service.

```sql
-- Project a named graph from AGE
SELECT age_gds.graph_project(
    name       => 'social',
    age_graph  => 'mydb',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);

-- Run PageRank — results are SQL rows
SELECT * FROM age_gds.pageRank_stream(graph_name => 'social')
ORDER BY score DESC;

-- Write scores back into the AGE graph
SELECT age_gds.pageRank_write(graph_name => 'social', write_property => 'pagerank');
```

## Why age_gds

- **Stay in PostgreSQL.** No external graph database, message queue, or sidecar process. Your graph lives in AGE; analytics run in the same transaction.
- **NetworKit does the math.** Every algorithm delegates to a NetworKit class. No hand-rolled BFS, no home-grown PageRank — battle-tested, parallelized C++ underneath.
- **Two execution modes.** Use a *persistent projection* (serialized to disk, cached per-backend) for repeated queries. Use *anonymous mode* for one-off analysis with no catalog overhead.
- **SQL-native results.** Every algorithm returns a set-returning function. Join results against your application tables, `ORDER BY`, `LIMIT`, write back to AGE — it is just SQL.
- **Write-back included.** `pageRank_write` and `betweenness_write` persist scores directly onto AGE nodes as Cypher properties.
- **Cheap, automatic cache invalidation.** `graph_reindex` in one backend is picked up by every other backend's session cache on its next use — no manual coordination. Optionally project with `incremental := true` for diff-based (rather than full-rebuild) reindexing on large graphs.

## Algorithms

| Category | Functions |
|---|---|
| [Centrality](docs/reference/centrality.md) | [PageRank](docs/reference/centrality.md#age_gdspagerank_stream), [Betweenness](docs/reference/centrality.md#age_gdsbetweenness_stream), [Closeness](docs/reference/centrality.md#age_gdscloseness_stream), [Harmonic](docs/reference/centrality.md#age_gdsharmoniccentrality_stream), [Degree](docs/reference/centrality.md#age_gdsdegree_stream), [Eigenvector](docs/reference/centrality.md#age_gdseigenvector_stream), [HITS](docs/reference/centrality.md#age_gdshits_stream), [ArticleRank](docs/reference/centrality.md#age_gdsarticlerank_stream), [Articulation Points](docs/reference/centrality.md#age_gdsarticulationpoints_stream) |
| [Community](docs/reference/community-detection.md) | [Louvain](docs/reference/community-detection.md#age_gdslouvain_stream), [Leiden](docs/reference/community-detection.md#age_gdsleiden_stream), [Label Propagation](docs/reference/community-detection.md#age_gdslabelpropagation_stream), [WCC](docs/reference/community-detection.md#age_gdswcc_stream), [SCC](docs/reference/community-detection.md#age_gdsscc_stream), [k-Core](docs/reference/community-detection.md#age_gdskcore_stream), [Triangle Count](docs/reference/community-detection.md#age_gdstrianglecount_stream), [Local Clustering Coefficient](docs/reference/community-detection.md#age_gdslocalclusteringcoefficient_stream), [Modularity](docs/reference/community-detection.md#age_gdsmodularity_stats), [Conductance](docs/reference/community-detection.md#age_gdsconductance_stats), [Clique Counting](docs/reference/community-detection.md#age_gdscliquecounting_stats) |
| [Path-finding](docs/reference/path-finding.md) | [BFS](docs/reference/path-finding.md#age_gdsbfs_stream), [Dijkstra](docs/reference/path-finding.md#age_gdsshortestpath_dijkstra_stream), [Delta-stepping](docs/reference/path-finding.md#age_gdsdeltastepping_stream), [Bellman-Ford](docs/reference/path-finding.md#age_gdsbellmanford_stream), [DFS](docs/reference/path-finding.md#age_gdsdfs_stream), [Random Walk](docs/reference/path-finding.md#age_gdsrandomwalk_stream), [Spanning Tree](docs/reference/path-finding.md#age_gdsspanningtree_stream), [All-Pairs Shortest Paths](docs/reference/path-finding.md#age_gdsallshortestpaths_stream), [A*](docs/reference/path-finding.md#age_gdsshortestpath_astar_stream), [Max Flow](docs/reference/path-finding.md#age_gdsmaxflow_stats), [Min-Cost Max Flow](docs/reference/path-finding.md#age_gdsmincostmaxflow_stats), [Topological Sort](docs/reference/path-finding.md#age_gdstopologicalsort_stream), [Longest Path](docs/reference/path-finding.md#age_gdslongestpath_stream), [Bridges](docs/reference/path-finding.md#age_gdsbridges_stream) |
| [Similarity](docs/reference/similarity.md) | [Node Similarity (Jaccard)](docs/reference/similarity.md#age_gdsnodesimilarity_stream), [KNN](docs/reference/similarity.md#age_gdsknn_stream) |
| [Link Prediction](docs/reference/link-prediction.md) | [Adamic-Adar](docs/reference/link-prediction.md#age_gdslinkprediction_adamicadar_stream), [Common Neighbors](docs/reference/link-prediction.md#age_gdslinkprediction_commonneighbors_stream), [Preferential Attachment](docs/reference/link-prediction.md#age_gdslinkprediction_preferentialattachment_stream), [Resource Allocation](docs/reference/link-prediction.md#age_gdslinkprediction_resourceallocation_stream) |
| [Embeddings](docs/reference/embeddings.md) | [Node2Vec](docs/reference/embeddings.md#age_gdsnode2vec_stream) |

## Quick install

### Debian package

```bash
# Build dependencies
sudo apt-get install debhelper postgresql-server-dev-all cmake g++ libgomp-dev git

# Initialize NetworKit submodule (once)
git submodule update --init --recursive

# Build and install the .deb
dpkg-buildpackage -us -uc -b
sudo dpkg -i ../postgresql-age-gds_1.0_*.deb
```

```sql
-- Load in your database
CREATE EXTENSION age;
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

CREATE EXTENSION age_gds;
SELECT age_gds.version();   -- '0.1.0'
```

### Build from source

```bash
# 1. Initialize the NetworKit submodule
git submodule update --init --recursive

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 3. Install into PostgreSQL
sudo cmake --install build

# 4. Verify NetworKit is linked
nm build/age_gds.so | grep -c '_ZN10NetworKit'   # must be > 0
```

```sql
-- 5. Load in your database
CREATE EXTENSION age;
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

CREATE EXTENSION age_gds;
SELECT age_gds.version();   -- '0.1.0'
```

**Dependencies:** PostgreSQL 14+, Apache AGE, OpenMP (`apt-get install libomp-dev`). NetworKit is built from the bundled submodule — no separate install needed.

## Documentation

- [User Guide](docs/user-guide.md) — installation, both execution modes, all algorithm categories, write-back, GUC settings, error reference, worked examples
- [Function Reference](docs/reference/README.md) — full signature and parameter description for every SQL and Cypher function
- [Architecture](docs/architecture.md) — data flow, binary file format, session cache internals, NetworKit integration constraints, build system notes
