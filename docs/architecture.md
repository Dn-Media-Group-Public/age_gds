# age_gds Architecture

## What age_gds is and is not

`age_gds` is a thin glue layer between Apache AGE (graph storage in PostgreSQL)
and NetworKit (C++ graph-data-science library). Its job is exactly three steps:

1. Project an AGE Cypher graph into a `NetworKit::Graph`.
2. Call a NetworKit algorithm class on that graph.
3. Stream results back to the SQL caller as set-returning function rows.

`age_gds` is **not** a graph-algorithm library. It contains no hand-written
graph traversal, centrality math, community detection, or shortest-path
computation. Every algorithm is delegated to a NetworKit class. Where NetworKit
provides no suitable class, the code documents an explicit fallback (see
[Algorithm substitutions](#algorithm-substitutions) below).

---

## Directory and file layout

```
age_gds/
├── CMakeLists.txt              Build system; NetworKit submodule, OpenMP, PG headers
├── age_gds.control             PostgreSQL extension control file
├── sql/
│   └── age_gds--1.0.sql        SQL DDL: schema, tables, function declarations
├── src/
│   ├── age_gds.cpp             PG_MODULE_MAGIC, _PG_init, GUC registration, version()
│   ├── bridge/
│   │   ├── node_id_map.h       Bidirectional AGE graphid ↔ nk::node mapping
│   │   ├── age_loader.h        LoadedGraph struct + age_load_graph() declaration
│   │   └── age_loader.cpp      Cypher → NetworKit::Graph via SPI
│   ├── catalog/
│   │   ├── serialization.h     Binary .bin format constants + function declarations
│   │   ├── serialization.cpp   gds_serialize() / gds_deserialize()
│   │   ├── graph_project.cpp   graph_project, graph_drop, graph_list, graph_exists,
│   │   │                       graph_is_stale SQL entry points
│   │   ├── session_cache.h     ProjectedGraph, GraphHandle, cache API declarations
│   │   └── session_cache.cpp   Per-backend LRU cache, get_or_load_graph(), cache SQL fns
│   ├── util/
│   │   ├── config_parser.h     PageRankConfig struct + parse_pagerank_config() declaration
│   │   ├── config_parser.cpp   JSONB + named-param config parsing for PageRank
│   │   ├── srf_helper.h        SRFState + srf_begin/emit/end declarations
│   │   ├── srf_helper.cpp      Set-returning function boilerplate (tuplestore)
│   │   ├── writeback.h         writeback_scores() + lookup_age_graph() declarations
│   │   └── writeback.cpp       Cypher SET write-back, pageRank_write, betweenness_write
│   └── algorithms/
│       ├── centrality.cpp      pageRank, betweenness, closeness, harmonic, degree,
│       │                       eigenvector, HITS, articleRank, articulationPoints
│       ├── community.cpp       louvain, leiden, labelPropagation, wcc, scc, kCore,
│       │                       triangleCount, localClusteringCoefficient, modularity
│       ├── pathfinding.cpp     bfs, dijkstra, deltaStepping, bellmanFord, dfs,
│       │                       randomWalk, spanningTree, allShortestPaths, maxFlow
│       ├── similarity.cpp      nodeSimilarity, knn (via JaccardIndex)
│       ├── linkprediction.cpp  adamicAdar, commonNeighbors, preferentialAttachment,
│       │                       resourceAllocation
│       └── embedding.cpp       node2vec (via NetworKit::Node2Vec)
└── third_party/
    └── networkit/              NetworKit git submodule (built as static lib)
```

---

## The two execution paths

Every algorithm SQL function supports two mutually exclusive modes.

### Persistent projection (named graph)

```
graph_project('name', 'age_graph', node_query, edge_query)
  → age_load_graph()          (AGE Cypher → NetworKit::Graph + NodeIdMap)
  → gds_serialize()           (NetworKit::Graph → .bin file on disk)
  → INSERT age_gds.graphs     (catalog row with file_path, counts, metadata)

pageRank_stream(graph_name => 'name')
  → session_cache_get()       (cache hit: return cached ProjectedGraph)
                              (cache miss: gds_deserialize() + LRU insert)
  → NetworKit::PageRank.run()
  → SRF rows via tuplestore
```

The projection is durable: the `.bin` file and catalog row survive across
backend connections. Any backend that calls an algorithm with `graph_name`
will deserialize the file on first use and cache the result in its own
per-backend LRU cache.

### Anonymous execution (query-scoped)

```
pageRank_stream(age_graph => 'mygraph', node_query => ..., edge_query => ...)
  → age_load_graph()          (AGE Cypher → NetworKit::Graph in memory only)
  → GraphHandle{owned}        (not inserted into session cache)
  → NetworKit::PageRank.run()
  → SRF rows via tuplestore
  (graph is destroyed when GraphHandle goes out of scope)
```

The anonymous path loads and discards the graph within the same SQL function
call. It does not populate the session cache (`entry_count` stays 0). It is
useful for one-off queries where the overhead of writing a `.bin` file is
undesirable.

`graph_name` and `age_graph` are mutually exclusive. Passing both raises
`ERRCODE_INVALID_PARAMETER_VALUE`. Passing neither also raises an error.

---

## Data flow: AGE Cypher → result rows

```
AGE graph (PostgreSQL)
  │
  │  SPI + ag_catalog.cypher()
  ↓
age_load_graph()
  ├─ node_query result → NodeIdMap::intern() + Graph::addNode()
  └─ edge_query result → Graph::addEdge() (with optional weight)
  ↓
LoadedGraph { NetworKit::Graph, NodeIdMap }
  │
  │  [persistent path only]
  ├─ gds_serialize()  →  <data_dir>/<name>.bin
  ├─ INSERT age_gds.graphs (file_path, node_count, edge_count, ...)
  │
  │  [algorithm call]
  ├─ session_cache_get() OR anonymous GraphHandle
  ↓
ProjectedGraph { graph, id_map, directed, weighted, approx_bytes, last_used }
  │
  │  NetworKit algorithm class (e.g. PageRank, Betweenness, PLM)
  │  algo.run()
  ↓
scores / partition / distances
  │
  │  graph.forNodes() / forEdges()
  │  id_map.resolve(nk_node) → AGE graphid
  ↓
SRFState (tuplestore)
  │
  │  tuplestore_putvalues()
  ↓
SQL result rows
```

---

## Binary `.bin` file format

Files are written by `gds_serialize()` in `src/catalog/serialization.cpp` and
read by `gds_deserialize()`. The format is a flat binary file with native-endian
integers (host byte order).

```
Offset  Size  Type      Field
------  ----  --------  -----
     0     8  uint64    magic = 0x4147455F4744530A ("AGE_GDS\n")
     8     4  uint32    version = 1
    12     4  uint32    flags: bit0 = directed, bit1 = weighted
    16     8  uint64    node_count
    24     8  uint64    edge_count
    32    24  uint8[24] reserved (zeroed)
    56    —   —         end of header (56 bytes total, padded to 64 with reserved)

Node table  (node_count × 8 bytes, immediately after header):
    +0     8  int64     AGE graphid; array index is the nk::node index

Edge table  (edge_count × 24 bytes, immediately after node table):
    +0     8  uint64    src nk::node index
    +8     8  uint64    dst nk::node index
   +16     8  float64   edge weight (1.0 if unweighted)
```

**Atomicity**: `gds_serialize()` writes to a `.tmp` sibling file, then renames
it to the final path. On transaction abort, a per-backend `XactCallback`
(`cleanup_bin_xact_cb`) unlinks the file.

---

## Session cache

Implemented in `src/catalog/session_cache.cpp`.

- **Storage**: a file-static `std::unordered_map<std::string, ProjectedGraph>`
  keyed by projection name. The map lives for the lifetime of the backend
  process.

- **Cache miss**: `session_cache_get()` looks up the `file_path` from
  `age_gds.graphs` via SPI, calls `gds_deserialize()`, computes an approximate
  byte cost (`node_count * 8 + edge_count * 20`), evicts LRU entries until
  the new entry fits within the `age_gds.session_cache_mb` GUC limit, then
  inserts the `ProjectedGraph`.

- **LRU eviction**: `evict_lru()` does a linear scan of the map and removes
  the entry with the oldest `last_used` (`std::chrono::steady_clock::time_point`).
  It is called in a loop until sufficient space is available or the cache is
  empty.

- **Cache hit**: the `last_used` timestamp is refreshed on every access.

- **Invalidation**: `session_cache_invalidate(name)` is called by `graph_drop`.
  `session_cache_clear()` runs on backend exit via `on_proc_exit`.

- **Lifetime**: the cache is per-backend (process-local). There is no shared
  memory or cross-backend invalidation.

- **Memory estimation**: `approx_bytes = node_count * 8 + edge_count * 20`.
  This is the same formula used by `graph_list()` to report `memoryUsage`.

- **GraphHandle**: a thin wrapper returned by `get_or_load_graph()`. For named
  projections it holds a `const ProjectedGraph *` into the cache. For anonymous
  execution it holds an `std::optional<ProjectedGraph>` (owned storage).

---

## How algorithm functions work

All algorithm SQL functions follow the same pattern:

1. **Argument extraction** — read `graph_name` / `age_graph` / query strings
   from `PG_FUNCTION_ARGS`. Check for NULL with `PG_ARGISNULL`.

2. **Graph resolution** — call `get_or_load_graph()` (persistent cache or
   anonymous load) to obtain a `GraphHandle` and dereference `gh.pg` to get
   `const ProjectedGraph &`.

3. **NetworKit call** — construct the algorithm class with `pg.graph` and any
   parameters, call `.run()`, catch `std::exception` and re-raise as
   `ereport(ERROR, ...)`.

4. **SRF emit** — call `srf_begin()`, iterate over result values with
   `pg.id_map.resolve(nk_node)` to convert back to AGE graphids, call
   `srf_emit()` for each row, return `srf_end()`.

The `SRFState` helper (`src/util/srf_helper.cpp`) wraps the
`InitMaterializedSRF` / `tuplestore` pattern into three calls:
- `srf_begin(fcinfo, nattrs, types[])` — allocates the tuplestore and TupleDesc
  in the per-query memory context.
- `srf_emit(state, values[], nulls[])` — appends one row.
- `srf_end(state, fcinfo)` — attaches the tuplestore to `ReturnSetInfo` and
  returns `(Datum) 0`.

---

## Algorithm substitutions

NetworKit does not have a class for every named algorithm. Where a direct class
is missing, the code uses the best available substitute and documents it with a
comment in both the SQL DDL and the C++ source.

| SQL function             | Intended algorithm  | NetworKit class used         | Note |
|--------------------------|---------------------|------------------------------|------|
| `hits_stream`            | HITS (Kleinberg)    | `EigenvectorCentrality`      | Both auth and hub return the same eigenvector score; equivalent for undirected graphs |
| `articleRank_stream`     | ArticleRank         | `PageRank` (damping=0.85)    | No distinct ArticleRank class in NetworKit |
| `articulationPoints_stream` | Articulation points | `BiconnectedComponents`   | A node is an articulation point iff `getComponentsOfNode(u).size() > 1` |
| `deltaStepping_stream`   | Delta-stepping SSSP | `Dijkstra`                   | NetworKit has no `DeltaStepping` class |
| `bellmanFord_stream`     | Bellman-Ford SSSP   | `Dijkstra`                   | NetworKit's `AlgebraicBellmanFord<Matrix>` requires a matrix instantiation impractical to use here |
| `triangleCount_stream`   | Triangle count      | `LocalClusteringCoefficient` | Triangles computed as `round(lcc * deg * (deg-1) / 2)` |
| `randomWalk_stream`      | Random walk         | `Aux::Random::choice` + `Graph::forNeighborsOf` | No `RandomWalk` class in NetworKit |

---

## NetworKit integration constraints

### ERROR macro clash

NetworKit's `Log.hpp` (included transitively from almost any NetworKit header)
redefines `ERROR` as a variadic function-like macro used for its logging system.
PostgreSQL defines `ERROR` as the integer constant `21` used in `ereport()`.

Every `.cpp` file that includes both NetworKit and PostgreSQL headers must:

1. Include NetworKit headers **first**.
2. Include PostgreSQL headers inside `extern "C" { }`.
3. After all includes, unconditionally restore the PostgreSQL definition:

```cpp
#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21
```

This pattern appears in every source file that mixes the two libraries.

### Static library build

NetworKit is built as a static library (`NETWORKIT_STATIC ON`) so that
`age_gds.so` is self-contained. This avoids runtime link failures when the
extension is loaded into a PostgreSQL backend that has no `libnetworkit.so`
in its library search path.

`-fPIC` (`CMAKE_POSITION_INDEPENDENT_CODE ON`) is set globally because a
static library linked into a shared module (`.so`) must be compiled as
position-independent code.

### OpenMP

NetworKit uses OpenMP for parallelism. `find_package(OpenMP REQUIRED)` and
`target_link_libraries(age_gds PRIVATE OpenMP::OpenMP_CXX)` are mandatory.
If OpenMP is absent the build will fail with missing symbols at link time.

### Linker flag

```cmake
target_link_options(age_gds PRIVATE -Wl,--allow-shlib-undefined)
```

This permits PostgreSQL symbols (resolved at runtime by the backend) to be
left undefined in the `.so`.

---

## Build system notes

`CMakeLists.txt` drives the entire build:

- `add_subdirectory(third_party/networkit EXCLUDE_FROM_ALL)` builds NetworKit
  from the submodule. `EXCLUDE_FROM_ALL` prevents NetworKit's own install
  targets from running during `make install`.
- `pg_config --includedir-server` provides PostgreSQL server headers.
- `pg_config --pkglibdir` and `pg_config --sharedir` determine install paths
  for the `.so`, `.control`, and `.sql` files.
- The extension installs as a standard PostgreSQL extension; `CREATE EXTENSION
  age_gds` loads it into a session.

Before the first build, initialize the submodule:

```bash
git submodule update --init --recursive
```

After every build, verify NetworKit symbols are present:

```bash
nm build/age_gds.so | grep -c '_ZN10NetworKit'
# must be > 0
```
