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
│   │   ├── index_storage.h     Live-index .nodes/.edges file layout + declarations
│   │   ├── index_storage.cpp   Live-index file I/O: init, append, compact
│   │   ├── graph_index.cpp     graph_index, graph_reindex, vertex/edge triggers,
│   │   │                       on_new_label (ag_label watch), install_label_trigger
│   │   ├── session_cache.h     ProjectedGraph (incl. live fields), GraphHandle, cache API
│   │   └── session_cache.cpp   Per-backend LRU cache; live-index version-counter check
│   ├── util/
│   │   ├── config_parser.h     PageRankConfig struct + parse_pagerank_config() declaration
│   │   ├── config_parser.cpp   JSONB + named-param config parsing for PageRank
│   │   ├── srf_helper.h        SRFState + srf_begin/emit/end declarations
│   │   ├── srf_helper.cpp      Set-returning function boilerplate (tuplestore)
│   │   ├── writeback.h         writeback_scores() + lookup_age_graph() declarations
│   │   ├── writeback.cpp       Cypher SET write-back, pageRank_write, betweenness_write
│   │   └── pair_hash.h         PairHash for std::pair<nk::node, nk::node>
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
  → gds_serialize()           (NetworKit::Graph → .bin file on disk, content_version=0)
  → INSERT age_gds.graphs     (catalog row with file_path, counts, metadata)

pageRank_stream(graph_name => 'name')
  → session_cache_get()       (cache hit: gds_read_version() check — reload if changed)
                              (cache miss: gds_deserialize() + LRU insert)
  → NetworKit::PageRank.run()
  → SRF rows via tuplestore
```

The projection is durable: the `.bin` file and catalog row survive across
backend connections. Any backend that calls an algorithm with `graph_name`
will deserialize the file on first use and cache the result in its own
per-backend LRU cache. A `graph_reindex` in one backend bumps `content_version`
in the `.bin` header; every other backend's next cache hit notices the
mismatch and reloads (see [Session cache](#session-cache) below).

### `incremental` storage (optional, both `graph_project` and `graph_index`)

`age_gds.graphs.incremental` is orthogonal to `live`: it selects the on-disk
*storage format* (slot/tombstone `.nodes`/`.edges` files, the same format live
indices use), independent of whether AGE triggers are installed on top of it.

|              | `live = false`                          | `live = true`            |
|--------------|------------------------------------------|--------------------------|
| `incremental = false` | **default `graph_project`** — flat `.bin`, full-rebuild-only reindex | not possible |
| `incremental = true`  | `graph_project(incremental := true)` — slot/tombstone, diff-or-full reindex | `graph_index` (unchanged) |

```
graph_project('name', 'age_graph', node_query, edge_query, incremental => true)
  → age_load_graph()             (AGE Cypher → NetworKit::Graph + NodeIdMap)
  → index_storage_write_initial() (slot/tombstone .nodes/.edges files, version_counter=1)
  → INSERT age_gds.graphs        (incremental=true, nodes_file_path, edges_file_path)
```

`graph_reindex('name')` on an `incremental`, non-`live` projection re-runs
`node_query`/`edge_query`, diffs the result against the current slot tables by
identity (AGE graphid for nodes, `(src_gid, dst_gid)` for edges), and applies
only the delta via `index_storage`'s existing append/tombstone primitives plus
a new weight-update-in-place primitive (`index_storage_set_edge_weight`) for
edges whose identity is unchanged but whose weight changed. `graph_reindex(
'name', force := true)` instead discards the diff and rewrites the slot files
from scratch (`index_storage_write_initial`), like a fresh `graph_project`.

This reuses the exact same session-cache version-check machinery live indices
already use (`incremental` gates that code path, not `live`).

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

### Live index (trigger-maintained)

```
graph_index('name', 'age_graph')
  → age_load_all_labels()     (scan all vertex/edge label tables via ag_label)
  → NetworKit::Graph built    (no Cypher; direct table scans)
  → index_storage_init()      (write .nodes + .edges files, set version counter)
  → install_label_trigger()   (AFTER INSERT/DELETE trigger on each label table)
  → INSERT age_gds.graphs (live=true, nodes_file_path, edges_file_path)
  → INSERT age_gds.index_triggers (one row per installed trigger)

-- vertex inserted via direct SQL on test_graph."Person":
INSERT trigger fires → index_storage_append_node() → version_counter++

-- algorithm called:
pageRank_stream(graph_name => 'name')
  → session_cache_get():
      compare stored version_counter vs catalog
      if changed: index_storage_load() → rebuild ProjectedGraph from .nodes/.edges
  → NetworKit::PageRank.run()
  → SRF rows
```

**Version counter**: a `uint64_t` appended to the `.nodes` file header.
Every trigger increment writes it atomically via `pwrite`. `session_cache_get`
reads it via `pread` without a lock (worst case: spurious reload).

**Advisory locks**: write operations use `pg_advisory_xact_lock_int8` (FNV-1a
hash of index name) to serialize concurrent writes to the index files. Locks
are transaction-scoped (auto-released at commit/rollback). No
`shared_preload_libraries` required.

**AGE 1.7.0 trigger bypass**: AGE uses `simple_table_tuple_insert` (an
internal catalog-level heap insertion that bypasses the normal executor trigger
path) for ALL Cypher DML — both vertex/edge inserts and ag_label inserts. This
means:
- Vertices and edges inserted via `CREATE` in Cypher never fire the live-index
  triggers.
- New label tables created via Cypher never fire `age_gds_label_watch`.
- Only direct SQL `INSERT`/`DELETE` on AGE label tables is captured.

To incorporate Cypher mutations: `graph_drop` + `graph_index` (full rebuild).
`graph_reindex` for live indices only compacts existing files; it does not
reload from AGE.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md)

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

- **Cache miss**: `session_cache_get()` looks up `file_path`/`nodes_file_path`/
  `edges_file_path`/`incremental` from `age_gds.graphs` via SPI, deserializes
  (`gds_deserialize()` for flat storage, `index_storage_deserialize()` for
  incremental storage), computes an approximate byte cost
  (`node_count * 8 + edge_count * 20`), evicts LRU entries until the new entry
  fits within the `age_gds.session_cache_mb` GUC limit, then inserts the
  `ProjectedGraph`.

- **LRU eviction**: `evict_lru()` does a linear scan of the map and removes
  the entry with the oldest `last_used` (`std::chrono::steady_clock::time_point`).
  It is called in a loop until sufficient space is available or the cache is
  empty.

- **Cache hit — version check**: every cache hit re-checks a cheap
  `content_version` on disk before returning the cached graph, regardless of
  storage kind:
  - Incremental storage (`ProjectedGraph::incremental == true`): `pread`s
    `version_counter` from the `.nodes` header via `index_storage_read_version()`.
  - Flat storage (`incremental == false`): `pread`s `content_version` from the
    `.bin` header via `gds_read_version()`.

  A mismatch against the cached `ProjectedGraph::content_version` triggers a
  full reload and replaces the cache entry; a match just refreshes `last_used`
  and returns the cached graph. This makes a `graph_reindex` performed by one
  backend visible to every other backend's cache without any explicit
  cross-backend signal — each backend notices on its own next access.

- **Invalidation**: `session_cache_invalidate(name)` is called by `graph_drop`
  and by the reindexing backend itself (so it doesn't wait for its own version
  check). `session_cache_clear()` runs on backend exit via `on_proc_exit`.

- **Lifetime**: the cache is per-backend (process-local); there is no shared
  memory. Cross-backend visibility of mutations is achieved purely through the
  on-disk `content_version`/`version_counter` check described above, not
  through any shared cache state.

See also: [Plan 10: Cross-Backend Cache Invalidation](implemented-plans/10-snapshot-version-cache.md)

- **Memory estimation**: `approx_bytes = node_count * 8 + edge_count * 20`.
  This is the same formula used by `graph_list()` to report `memoryUsage`.

- **GraphHandle**: a thin wrapper returned by `get_or_load_graph()`. For named
  projections it holds a `const ProjectedGraph *` into the cache. For anonymous
  execution it holds an `std::optional<ProjectedGraph>` (owned storage).

---

## Per-call weight override

Weight is normally decided once, at projection time, by the `weighted` flag
passed to `graph_project`/`graph_index`/`age_load_graph`. Many Neo4j GDS
procedures accept a per-call `relationshipWeightProperty` that lets a single
invocation pick a different edge property as the weight, without
re-projecting. `weight_property`/`relationship_weight_property` implements
this convention for age_gds — see
[Plan: Per-call relationship weight plumbing](implemented-plans/00-weight-plumbing.md).

The two execution paths satisfy the convention differently:

- **Anonymous execution** (`age_graph`/`node_query`/`edge_query` supplied
  directly): no special mechanism exists or is needed. The caller already
  controls the weight column by writing the desired property expression into
  their own `edge_query`'s `weight` column (`... RETURN id(a) AS src, id(b)
  AS dst, r.<property> AS weight`). `apply_weight_override()`
  (`src/util/weight_override.h`/`.cpp`) is a no-op on an anonymous
  `GraphHandle` (`gh.owned.has_value()`).

- **Named/cached graph** (`graph_name` resolves through `session_cache_get`):
  the topology was projected once and cached, so there is no per-call
  opportunity to change the weight source without an explicit mechanism.
  `apply_weight_override(GraphHandle &gh, const char *weight_property)`:
  1. No-ops if `weight_property` is `NULL`, or if `gh` is anonymous (see
     above).
  2. Otherwise calls `reweight_from_property()` (`src/bridge/age_loader.h`/
     `.cpp`), which re-runs the *stored* `edge_query` for the projection
     (recorded in `age_gds.graphs.edge_query` at `graph_project`/
     `graph_index` time, exposed on the cached `ProjectedGraph` as
     `age_graph`/`edge_query`) against AGE via SPI, but with the query's
     final `RETURN` projection substituted to select `weight_property`
     instead of whatever expression the original query used. The
     relationship alias is extracted from the query's `MATCH` clause (the
     `-[alias...]->` pattern) so the substitution reads
     `<alias>.<weight_property>` rather than assuming a fixed alias name.
     This substitution assumes the query follows the convention documented
     above (`RETURN id(a) AS src, id(b) AS dst, <expr> AS weight`); if the
     `"... AS weight"` projection can't be located, `reweight_from_property`
     raises rather than guessing.
  3. Applies the resulting weights to a **weighted copy** of the cached
     `NetworKit::Graph` (`NetworKit::Graph(g, /*weighted*/ true,
     g.isDirected())`) — the shared session-cache entry is never mutated in
     place. Only edges already present in the cached topology are
     reweighted (`Graph::hasEdge()` is checked before `Graph::setWeight()`,
     since `setWeight()` on a nonexistent pair inserts it); rows the reweight
     query returns for pairs outside the cached topology are silently
     skipped, matching `age_load_graph`'s own "unknown node" skip behavior.
  4. Repoints `gh.pg` at the reweighted copy, stored in `gh.owned` (so the
     wrapper's normal `const ProjectedGraph &pg = *gh.pg;` pattern is
     unaffected).

  A named graph whose catalog row predates this mechanism (empty
  `age_graph`/`edge_query`, e.g. very old rows, or `graph_index` live indices
  created before `graph_index.cpp` started recording the fixed edge query
  template it uses internally) raises `ereport(ERROR, ...)` rather than
  silently ignoring the override.

  `reweight_from_property()` detects "did the substitution actually happen"
  by comparing the substituted query string to the original — if they're
  byte-identical, it treats that as "no `AS weight` projection found" and
  throws `std::runtime_error`. This is a false positive in one degenerate
  case: overriding with the *same* property name the projection's own
  `edge_query` already used produces an identical string too, so that call
  also raises. `apply_weight_override()` (`src/util/weight_override.cpp`)
  wraps the call to `reweight_from_property()` in a `try`/`catch` and
  re-raises via `ereport(ERROR, ...)` — without this, the uncaught
  `std::runtime_error` would cross the C/C++ boundary into PostgreSQL's `.so`
  entry point and abort the whole backend process instead of returning a SQL
  error (see `test/sql/12-centrality-parity.sql`'s degenerate-override case).

  Cost: the reweighted copy is O(V+E), the same cost every other per-call
  graph variant already pays (e.g. `wcc_stream`'s `threshold` edge filter).

Wired into `pageRank_stream`/`pageRank_stats` (the reference implementation
and regression test, `test/sql/11-weight-override.sql`), and, per
[Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md),
into `betweenness_stream`, `degree_stream`, `eigenvector_stream`, and
`articleRank_stream`. Other algorithm wrappers adopt the same one-line
pattern at their own `get_or_load_graph()` call site: `GraphHandle gh =
get_or_load_graph(...); apply_weight_override(gh, weight_property);`.

See also: [Plan: Per-call Relationship Weight Plumbing](implemented-plans/00-weight-plumbing.md),
[Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

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
| `betweenness_stream` (`sampling_size` set) | Approximate betweenness | `ApproxBetweenness` | `sampling_size` maps to `epsilon = 1/sqrt(sampling_size)`, fixed `delta = 0.1`; `sampling_seed` calls `Aux::Random::setSeed()` first. Requires an undirected graph — its diameter-estimation step does not support directed input (a NetworKit limitation, see `test/sql/12-centrality-parity.sql`) |

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)
(added `ApproxBetweenness` as a new NetworKit dependency alongside the
existing exact `Betweenness`).

### Shared post-processing helpers

- `src/util/scaler.h`/`.cpp` — `apply_scaler(scores, scaler)` implements the
  Neo4j-GDS-style `scaler` parameter (`MinMax`, `Max`, `Mean`, `Log`,
  `StdScore`) as a one-pass post-processing step over a score vector, after
  the NetworKit algorithm has already run. Shared by `pageRank_stream`/
  `_stats`, `eigenvector_stream`, and `articleRank_stream`. An unrecognized
  scaler name throws `std::invalid_argument`, caught by each wrapper and
  turned into `ereport(ERROR, ...)`.

See also: [Plan: Centrality Algorithm Parity Fixes](implemented-plans/01-centrality-parity.md)

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
