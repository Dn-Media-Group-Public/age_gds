# age_gds — Apache AGE Graph Data Science Extension

PostgreSQL C/C++ extension providing graph analytics for Apache AGE, using
NetworKit as the algorithm backend, with function names and parameters
compatible with Neo4j Graph Data Science (GDS) where possible.

---

## Goals

- Run graph algorithms (centrality, community detection, path finding, etc.)
  on graphs stored in Apache AGE
- Mirror Neo4j GDS function names and config parameters for familiarity
- Support both one-shot query-scoped execution and persistent named projections
- Scale to large graphs via NetworKit's parallel C++ algorithms

---

## Dependencies

| Dependency | How obtained |
|---|---|
| PostgreSQL (dev headers) | `apt-get install postgresql-server-dev-XX` |
| Apache AGE (headers + lib) | build from source, system install |
| NetworKit | git submodule (not in apt on any Ubuntu/Debian suite) |
| OpenMP | `apt-get install libomp-dev` (NetworKit runtime dep) |
| build-essential | `apt-get install build-essential` |
| CMake ≥ 3.16 | `apt-get install cmake` |

NetworKit is not packaged for apt on any Ubuntu or Debian suite. It is
installed as a git submodule and built as part of the extension's CMake build.
Its dependencies (compiler, OpenMP) are available via apt.

---

## Architecture

```
PostgreSQL session backend (OS process)
├── SQL/Cypher call → age_gds C++ function
├── Graph Loader (AGE → nk::Graph)         [bridge/age_loader.cpp]
├── Per-session nk::Graph LRU cache        [catalog/session_cache.cpp]
│     └── populated by deserializing from disk
├── Algorithm Layer                         [algorithms/]
│     └── thin wrappers over NetworKit classes
└── Result Layer                            [util/srf_helper.cpp]
      └── tuplestore / SRF back to PostgreSQL

Disk (persistent, shared across all sessions)
├── $PGDATA/age_gds/<name>.bin             binary serialized graph
└── age_gds.graphs (PostgreSQL table)      projection metadata

OS page cache
└── mmap on .bin files — shared physical pages across concurrent readers,
    no explicit locking needed for reads
```

### Why Not Shared Memory

PostgreSQL uses OS processes (one per client connection), not threads. There
is no shared heap. Sharing a C++ object such as `nk::Graph` across backends
would require serializing it into a shared memory segment (DSM) and
deserializing a copy into each backend's heap on access — no simpler than
reading from disk, and with additional complexity (locking, version tracking,
crash recovery).

A background worker process holding the catalog in its heap and communicating
with session backends via `shm_mq` avoids serialization of the graph itself
but introduces a different set of problems:
- every result row crosses a process boundary (IPC overhead for `.stream()`)
- worker crash loses all projected graphs simultaneously
- concurrent algorithm runs on the same graph need explicit locking
- signal handling (query cancel) is harder with threads in C

The disk-backed catalog avoids all of this. The OS page cache provides
efficient cross-session sharing of serialized graph data without any
application-level shared memory management.

---

## Graph Catalog

### Metadata Table

```sql
CREATE TABLE age_gds.graphs (
    name          text PRIMARY KEY,
    age_graph     text        NOT NULL,   -- source AGE graph name
    node_query    text,                   -- Cypher used for node projection
    edge_query    text,                   -- Cypher used for edge projection
    node_count    bigint,
    edge_count    bigint,
    directed      boolean     NOT NULL DEFAULT false,
    weighted      boolean     NOT NULL DEFAULT false,
    file_path     text        NOT NULL,   -- absolute path to .bin file
    created_at    timestamptz NOT NULL DEFAULT now(),
    projected_by  text                    -- pg_authid.rolname
);
```

`graph_project` writes a row (within the calling transaction — projection is
atomic: either the file and row both exist, or neither does).
`graph_drop` deletes the row and unlinks the file.
`graph_list` is a plain `SELECT`.
The table survives server restart; projections are durable.

### Binary File Format

Files written to `$PGDATA/age_gds/<name>.bin`. Written atomically:
write to `<name>.bin.tmp`, then `rename(2)`.

```
Header (64 bytes):
  magic:        uint64   0x4147455F4744530A  ("AGE_GDS\n")
  version:      uint32
  flags:        uint32   bit 0 = directed, bit 1 = weighted
  node_count:   uint64
  edge_count:   uint64
  reserved:     bytes[24]

Node table (node_count × 8 bytes):
  graphid[]:    int64[]  AGE graphids, indexed by nk node index (0-based)

Edge table (edge_count × 20 bytes):
  src[]:        uint32   nk node index
  dst[]:        uint32   nk node index
  weight[]:     float64
```

Loaded via `mmap`. The kernel page cache then serves as an implicit
shared buffer: multiple sessions mapping the same file share physical pages.

### Per-Session Cache

Each backend keeps a `std::unordered_map<string, ProjectedGraph>` in its
heap. On first access the `.bin` file is `mmap`'d and deserialized into a
`nk::Graph`. Subsequent calls in the same session reuse the deserialized
object without I/O.

Configurable LRU eviction via GUC `age_gds.session_cache_mb` (default 512).
When a new deserialization would exceed the limit, the least recently used
projection is evicted.

```
Disk:     source of truth — persistent, visible to all sessions
Session:  hot cache — deserialized nk::Graph, LRU-evicted, rebuilt on miss
```

### Staleness

Projections are explicitly managed by the user, matching GDS semantics.
The system does not auto-invalidate when the underlying AGE graph changes.
`created_at` in the catalog table lets users compare projection age against
their knowledge of data updates.

A helper function `age_gds.graph_is_stale(name)` compares `created_at`
against `pg_stat_user_tables` modification counters for the relevant AGE
label tables — not a guarantee but a useful signal.

---

## Node ID Mapping

AGE uses 64-bit `graphid` values. NetworKit requires contiguous 0-based
integer node indices. A bidirectional map is built during projection and
stored in the binary file (the node table above) and in the per-session
`ProjectedGraph` struct.

```cpp
// bridge/node_id_map.h
struct NodeIdMap {
    std::unordered_map<int64_t, nk::node> graphid_to_nk;
    std::unordered_map<nk::node, int64_t> nk_to_graphid;

    nk::node intern(int64_t gid) {
        auto [it, inserted] = graphid_to_nk.emplace(gid, graphid_to_nk.size());
        if (inserted) nk_to_graphid[it->second] = gid;
        return it->second;
    }

    int64_t resolve(nk::node n) const { return nk_to_graphid.at(n); }
};
```

`std::unordered_map` is used throughout (not PostgreSQL's HTAB) because the
node map lives entirely in the backend's private C++ heap. HTAB is only needed
for shared-memory data structures, which this design avoids.

---

## SQL Interface

### Graph Lifecycle

```sql
-- Project a named graph (persists to disk)
SELECT age_gds.graph_project(
    'myGraph',
    $$MATCH (n:Person) RETURN id(n) AS id, n.age AS age$$,
    $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
      RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);

-- List all projections
SELECT * FROM age_gds.graph_list();
-- → (name, age_graph, node_count, edge_count, directed, weighted,
--    created_at, projected_by, memoryUsage)

-- Check existence
SELECT age_gds.graph_exists('myGraph');

-- Check staleness hint
SELECT age_gds.graph_is_stale('myGraph');

-- Drop
SELECT age_gds.graph_drop('myGraph');
```

### Algorithm Functions

Each algorithm exposes three modes, matching GDS naming:

```sql
-- stream: one row per node (or edge)
SELECT * FROM age_gds.pageRank_stream(
    graph_name     => 'myGraph',
    damping_factor => 0.85,
    max_iterations => 20,
    tolerance      => 1e-7
);
-- → (nodeId bigint, score float8)

-- write: stores result as property on AGE nodes
SELECT age_gds.pageRank_write(
    graph_name     => 'myGraph',
    write_property => 'pagerank',
    damping_factor => 0.85
);

-- stats: aggregate summary only
SELECT * FROM age_gds.pageRank_stats(
    graph_name => 'myGraph'
);
-- → (ranIterations int, didConverge bool, computeMillis bigint,
--    minScore float8, maxScore float8, meanScore float8)
```

### Config: Named Parameters and JSON

Both call styles are supported. Named parameters take precedence over JSON
keys; JSON fills unset parameters; defaults apply last.

```sql
-- named parameters
SELECT * FROM age_gds.pageRank_stream(
    graph_name     => 'myGraph',
    damping_factor => 0.85
);

-- JSON config (GDS-style, useful for scripting)
SELECT * FROM age_gds.pageRank_stream(
    graph_name => 'myGraph',
    config     => '{"dampingFactor": 0.85, "maxIterations": 20}'
);
```

JSON parsing uses PostgreSQL's built-in `jsonb` infrastructure
(`JsonbIteratorNext`) — no external JSON library needed.

### Query-Scoped (Anonymous) Execution

For one-shot queries where no persistent projection is needed:

```sql
SELECT * FROM age_gds.pageRank_stream(
    age_graph      => 'mygraph',
    node_query     => 'MATCH (n:Person) RETURN id(n)',
    edge_query     => 'MATCH (a:Person)-[r:KNOWS]->(b) RETURN id(a),id(b)',
    damping_factor => 0.85
);
```

Builds `nk::Graph` in `CurrentMemoryContext`, runs algorithm, returns results,
frees memory. No file written, no catalog row. Equivalent to GDS's anonymous
Cypher projection.

---

## Algorithm Coverage

### Centrality

| GDS function | NetworKit class |
|---|---|
| `gds.pageRank` | `centrality.PageRankNibble` / power iteration |
| `gds.betweenness` | `centrality.Betweenness` / `ApproxBetweenness` |
| `gds.closeness` | `centrality.Closeness` |
| `gds.harmonicCentrality` | `centrality.HarmonicCloseness` |
| `gds.degree` | `centrality.DegreeCentrality` |
| `gds.eigenvector` | `centrality.EigenvectorCentrality` |
| `gds.hits` | `centrality.HITS` |
| `gds.articleRank` | custom (PageRank variant) |
| `gds.articulationPoints` | `components.BiconnectedComponents` |

### Community Detection

| GDS function | NetworKit class |
|---|---|
| `gds.louvain` | `community.PLM` |
| `gds.leiden` | `community.ParallelLeiden` |
| `gds.labelPropagation` | `community.LabelPropagation` |
| `gds.wcc` | `components.ConnectedComponents` |
| `gds.scc` | `components.StronglyConnectedComponents` |
| `gds.kCore` | `centrality.CoreDecomposition` |
| `gds.triangleCount` | `centrality.LocalClusteringCoefficient` |
| `gds.localClusteringCoefficient` | `centrality.LocalClusteringCoefficient` |
| `gds.modularity` | `community.Modularity` |

### Path Finding

| GDS function | NetworKit class |
|---|---|
| `gds.shortestPath.dijkstra` | `distance.Dijkstra` |
| `gds.allShortestPaths` | `distance.APSP` |
| `gds.deltaStepping` | `distance.SSSP` |
| `gds.bellmanFord` | `distance.BellmanFord` |
| `gds.bfs` | `traversal.BFS` |
| `gds.dfs` | `traversal.DFS` |
| `gds.randomWalk` | `sampling.RandomWalk` |
| `gds.spanningTree` | `graph.SpanningForest` |
| `gds.maxFlow` | `flow.EdmondsKarp` |

### Similarity and Link Prediction

| GDS function | NetworKit class |
|---|---|
| `gds.nodeSimilarity` | `linkprediction.JaccardIndex` |
| `gds.knn` | custom k-NN on similarity matrix |
| `gds.linkprediction.adamicAdar` | `linkprediction.AdamicAdarIndex` |
| `gds.linkprediction.commonNeighbors` | `linkprediction.CommonNeighborsIndex` |
| `gds.linkprediction.preferentialAttachment` | `linkprediction.PreferentialAttachmentIndex` |
| `gds.linkprediction.resourceAllocation` | `linkprediction.ResourceAllocationIndex` |

### Node Embeddings

| GDS function | NetworKit class |
|---|---|
| `gds.node2vec` | `embedding.Node2Vec` |

---

## Project Structure

```
age_gds/
├── CMakeLists.txt              # primary build: finds PG, AGE, NetworKit
├── Makefile                    # thin PGXS wrapper
├── age_gds.control
├── third_party/
│   └── networkit/              # git submodule
├── sql/
│   └── age_gds--1.0.sql        # extension DDL
├── src/
│   ├── age_gds.cpp             # PG_MODULE_MAGIC, _PG_init, GUC registration
│   ├── catalog/
│   │   ├── graph_project.cpp   # graph_project(), graph_drop(), graph_list()
│   │   ├── session_cache.cpp   # per-session LRU nk::Graph cache
│   │   └── serialization.cpp   # .bin read/write, mmap
│   ├── bridge/
│   │   ├── age_loader.cpp      # execute node/edge Cypher, build nk::Graph
│   │   └── node_id_map.h       # graphid ↔ nk::node bidirectional map
│   ├── algorithms/
│   │   ├── centrality.cpp
│   │   ├── community.cpp
│   │   ├── pathfinding.cpp
│   │   ├── similarity.cpp
│   │   ├── linkprediction.cpp
│   │   └── embedding.cpp
│   └── util/
│       ├── srf_helper.cpp      # set-returning function boilerplate
│       ├── config_parser.cpp   # named params + jsonb → config structs
│       └── writeback.cpp       # write algorithm results to AGE node props
└── test/
    └── sql/
        ├── graph_project.sql
        ├── centrality.sql
        ├── community.sql
        └── pathfinding.sql
```

---

## Build System

CMakeLists.txt detects PostgreSQL via `pg_config`, AGE via a find module,
and builds NetworKit from the submodule. C++17 required. OpenMP enabled.

```cmake
find_package(PostgreSQL REQUIRED)
find_package(OpenMP REQUIRED)
add_subdirectory(third_party/networkit)
# AGE headers located via AGE_ROOT or pkg-config
```

---

## Implementation Phases

| Phase | Scope |
|---|---|
| 1 | Build system, extension skeleton, `_PG_init`, GUC registration |
| 2 | AGE→NetworKit bridge (`age_loader`), node ID map |
| 3 | Binary serialization, `graph_project` / `graph_drop` / `graph_list` |
| 4 | Per-session LRU cache, `mmap` loader |
| 5 | Config parser (named params + jsonb), SRF helper |
| 6 | Core centrality algorithms (PageRank, Betweenness, Closeness, Degree) |
| 7 | Community detection (Louvain, LPA, WCC, SCC, K-Core) |
| 8 | Path finding (Dijkstra, BFS, APSP) |
| 9 | Write-back to AGE node properties |
| 10 | Link prediction and similarity |
| 11 | Node embeddings (Node2Vec) |
| 12 | `graph_is_stale`, memory estimation, benchmarks |
