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

## Algorithms

| Category | Functions |
|---|---|
| Centrality | PageRank, Betweenness, Closeness, Harmonic, Degree, Eigenvector, HITS, ArticleRank, Articulation Points |
| Community | Louvain, Leiden, Label Propagation, WCC, SCC, k-Core, Triangle Count, Local Clustering Coefficient, Modularity |
| Path-finding | BFS, Dijkstra, Delta-stepping, Bellman-Ford, DFS, Random Walk, Spanning Tree, All-Pairs Shortest Paths, Max Flow |
| Similarity | Node Similarity (Jaccard), KNN |
| Link Prediction | Adamic-Adar, Common Neighbors, Preferential Attachment, Resource Allocation |
| Embeddings | Node2Vec |

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
- [SQL API Reference](docs/api.md) — full signature and parameter description for every SQL function
- [Architecture](docs/architecture.md) — data flow, binary file format, session cache internals, NetworKit integration constraints, build system notes
