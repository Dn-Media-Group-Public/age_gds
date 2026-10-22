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
│       │                       triangleCount, localClusteringCoefficient, modularity,
│       │                       conductance, cliqueCounting
│       ├── pathfinding.cpp     bfs, dijkstra, deltaStepping, bellmanFord, dfs,
│       │                       randomWalk, spanningTree, allShortestPaths, maxFlow,
│       │                       topologicalSort, longestPath, bridges
│       ├── similarity.cpp      nodeSimilarity (bounded top-K/bottom-K heaps;
│       │                       JACCARD/OVERLAP/COSINE), knn (via JaccardIndex)
│       ├── linkprediction.cpp  adamicAdar, commonNeighbors, preferentialAttachment,
│       │                       resourceAllocation (streaming + single-pair
│       │                       scalar variants)
│       └── embedding.cpp       node2vec (via NetworKit::Embedding::
│                               BiasedRandomWalk + learnEmbeddings)
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

**Slots vs NetworKit ids.** Records and edges address nodes by *slot* (position
in the `.nodes` file). `index_storage_deserialize` assigns NetworKit ids with
`graph.addNode()` while skipping tombstones, so `nk == slot` only while the
file has no dead slots. `NodeIdMap` is therefore not a graphid -> slot map;
`IndexDeserializedGraph::node_slot_by_graphid` is, and the diff uses it (O(1)
instead of a linear `index_storage_find_node` scan per added edge). The diff
applies through `IndexBatchWriter`, which opens both files once and writes the
headers in `flush()`; `version_counter` is bumped afterwards.

**Whole-graph projections.** `node_query`/`edge_query` NULL means "the whole
AGE graph": `age_load_graph` reads ids (and, if weighted, `weight`) with SQL from
the label-table inheritance parents. `ProjectedGraph::whole_graph` records this
in memory (distinct from `age_graph` empty = no AGE source recorded); readers of
the stored query text (`reweight_from_property`, `read_node_property_as_*`)
read from `properties` with SQL when the query is empty.

See also: [Plan: Faster diff-and-apply reindex](../implemented-plans/faster-diff-reindex.md)

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

See also: [Plan 09: Live Graph Index](implemented-plans/09-live-graph-index.md)

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

## Per-call node-property read

Mirroring the weight-override mechanism above, `ProjectedGraph`
(`src/catalog/session_cache.h`) also caches the projection's `node_query`
text (`age_gds.graphs.node_query`, populated by `graph_project`), alongside
`age_graph`/`edge_query`, in every place `edge_query` is populated:
`session_cache_get`'s cache-miss SPI lookup, its flat-storage
version-mismatch reload, its incremental/live-index reload, and the
anonymous-load path in `get_or_load_graph()`. `apply_weight_override()`'s
reweighted-copy path also carries `node_query` over so a single call can
combine `relationship_weight_property` with a node-property read.

`graph_index()`-created live indices do **not** currently record
`node_query` in their catalog INSERT (only `age_graph`/`edge_query`), so
node-property reads against a pure `graph_index()` graph raise the same
"re-run graph_project to populate them" error as a pre-existing catalog row
would — a known limitation, not fixed as part of this plan (out of its
declared scope; see
[Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)).

`src/util/partition_post.h`/`.cpp` provides the shared read mechanism:

- `read_node_property_as_partition()` re-runs `node_query` (substituting its
  `RETURN id(<alias>) AS id` projection for `RETURN id(<alias>) AS id,
  <alias>.<property> AS value`, the same alias-detection-and-substitution
  approach `reweight_from_property()` uses for edge queries) and builds a
  `NetworKit::Partition`. Nodes without a value get their own singleton
  community; distinct property values map to fresh ids starting at
  `graph.upperNodeIdBound()` so they can never collide with a default
  singleton id, and so `Partition::upperBound()` stays proportional to graph
  size rather than to unrelated raw property values. Used by
  `labelPropagation_stream`'s (rejected — see below) `seed_property` design
  and `modularity_stats`'s `community_property`.
- `read_node_property_as_values()` uses the same query mechanism but returns
  a raw per-node numeric value instead of a partition, for
  `localClusteringCoefficient_stream`'s `triangle_count_property`.
- `consecutive_ids()`/`filter_min_community_size()` are thin wrappers over
  `NetworKit::Partition::compact()`/`::remove()` — no hand-rolled
  renumbering or filtering logic. `filter_min_community_size` must run
  *after* any `consecutive_ids()` pass on the same `Partition`: `remove()`
  sets an entry to `NetworKit::none`, and `compact()` run afterwards could
  attempt to renumber it. `emit_partition()` (in
  `src/algorithms/community.cpp`) skips any node for which
  `Partition::contains()` is false, so filtered-out nodes are omitted from
  the result set (Neo4j `minCommunitySize` semantics: excluded, not
  reassigned).

### A NetworKit gap found during this plan: `labelPropagation_stream`'s `seed_property`

`docs/plans/02-community-detection-parity.md`'s design called for
`seed_property` to read a node property into a `NetworKit::Partition` and
pass it as `NetworKit::PLP`'s `baseClustering` constructor argument
(`PLP(G, baseClustering, theta)`). Inspecting the vendored
`third_party/networkit/include/networkit/community/PLP.hpp`/`.cpp` found
that this constructor's initializer list sets only `updateThreshold` —
`maxIterations` (declared `count maxIterations;` with no default member
initializer) is never assigned on this path, and `PLP::run()`'s loop
condition (`nIterations < maxIterations`) reads it unconditionally. There is
no public setter to fix it after construction (`PLP` only exposes
`setUpdateThreshold()`). Using this constructor is therefore relying on an
uninitialized read — undefined behavior, not delegation to a real
NetworKit algorithm.

Per this project's rules (never hand-roll a graph algorithm, never rely on
undefined behavior to fake correctness), `seed_property` is accepted in
`labelPropagation_stream`'s SQL signature (for forward compatibility/API
shape) but always raises `ERRCODE_FEATURE_NOT_SUPPORTED` when non-NULL. This
is the same category of gap as `node_weight_property` (already excluded
from this plan's scope for a different but related reason: NetworKit's
`Graph` has no node-weight concept at all).

See also: [Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)

---

## `conductance_stats`/`cliqueCounting_stats`: new community-adjacent procedures (`src/algorithms/community.cpp`)

`conductance_stats` calls `NetworKit::Conductance::getQuality(partition, G)`,
which requires a literal 2-partition: `Conductance.cpp` indexes fixed-size
two-element arrays (`vol[2]`, etc.) directly by each node's cluster id with
no bounds check, so ids outside `{0,1}` are undefined behavior, not a
caught error. This is incompatible with plan 02's
`read_node_property_as_partition()`, which deliberately assigns *internal*
dense subset ids offset above `graph.upperNodeIdBound()` rather than
passing property values straight through as subset ids (see the
"Node-property read" helper section above) — reusing it here would build a
`Partition` that silently violates `Conductance`'s precondition.
`conductance_stats` instead reuses `read_node_property_as_values()` (the
other plan 02 helper, already used by `localClusteringCoefficient_stream`)
to read the raw numeric `community_property`, validates every node's value
is exactly `0.0` or `1.0` (erroring — `ERRCODE_INVALID_PARAMETER_VALUE` —
on any other value or a missing one), and constructs the
`NetworKit::Partition` directly from those literal ids.

`cliqueCounting_stats` calls `NetworKit::MaximalCliques(G, callback)`
(the same class already vetted in
`docs/neo4j-gds-parity-report.md` §7) and buckets counts by clique size as
the callback fires — no materialization of the full clique list. This
counts *maximal* cliques only: NetworKit has no primitive enumerating every
clique of every size (maximal or not), so the result is not an exact port
of Neo4j GDS's per-node `counts` semantics, just the closest thing
`MaximalCliques` can produce as a thin wrapper.

See also: [Plan: New Graph-Theoretic Procedures](implemented-plans/06-new-graph-procedures.md)

---

## Community detection: `recurse`/`iterations` are approximations, not exact ports

Two parameters added by
[Plan: Community Detection Algorithm Parity Fixes](implemented-plans/02-community-detection-parity.md)
stand in for a Neo4j GDS parameter without being numerically equivalent to
it — **do not treat them as exact ports**:

- `louvain_stream`'s `recurse` (passed straight through to `NetworKit::PLM`'s
  own `recurse` constructor argument) approximates Neo4j's `maxLevels`:
  `true` (default) is PLM's normal recursive multilevel coarsening, `false`
  is a single level only. There is no numeric cap in between — `maxLevels =
  3` has no NetworKit equivalent exposed via this boolean.
- `leiden_stream`'s `iterations` (passed straight through to
  `NetworKit::ParallelLeiden`'s own `iterations` constructor argument) is
  only a loose analogue of Neo4j's `maxLevels`: Neo4j's parameter bounds
  coarsening *depth* within one Leiden run, while NetworKit's `iterations`
  repeats the *entire* algorithm some number of times. They answer
  different questions and happen to share the "how much work" flavor, not
  the same mechanism.

Both defaults were also changed from the underlying NetworKit class's own
constructor default (`PLM`'s `maxIter` default is 32, not 10;
`ParallelLeiden`'s `iterations` default is 3, not 10) to the values this
plan settled on — see `docs/reference/community-detection.md` for each
function's exact wording.

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
| `bridges_stream`         | Bridges              | `BiconnectedComponents`   | An edge is a bridge iff its biconnected component's node set has exactly 2 members (see "New graph-theoretic procedures" below) |
| `deltaStepping_stream`   | Delta-stepping SSSP | `Dijkstra`                   | NetworKit has no `DeltaStepping` class (no `delta` parameter to add; tracked in `docs/neo4j-gds-parity-external-libs.md`, not a plan-03 item) |
| `triangleCount_stream`   | Triangle count      | `LocalClusteringCoefficient` | Triangles computed as `round(lcc * deg * (deg-1) / 2)` |
| `betweenness_stream` (`sampling_size` set) | Approximate betweenness | `ApproxBetweenness` | `sampling_size` maps to `epsilon = 1/sqrt(sampling_size)`, fixed `delta = 0.1`; `sampling_seed` calls `Aux::Random::setSeed()` first. Requires an undirected graph — its diameter-estimation step does not support directed input (a NetworKit limitation, see `test/sql/12-centrality-parity.sql`) |
| `maxFlow_stats`/`minCostMaxFlow_stats` (`alpha`) | Cost-scaling | *(unsupported)* | Not exposed by `SuccessiveShortestPathMinCostFlow`; documented gap, not a substitution |

`bellmanFord_stream` and `randomWalk_stream` used to appear in this table
(Dijkstra and hand-rolled `Aux::Random::choice` substitutions,
respectively); both are now backed by real, purpose-built NetworKit classes
(`AlgebraicBellmanFord<CSRMatrix>` and
`NetworKit::Embedding::BiasedRandomWalk`) -- see the "Path-finding and flow
algorithms" section below.

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

## Path-finding and flow algorithms (`src/algorithms/pathfinding.cpp`)

Plan 03 added several new NetworKit dependencies to this file:
`AlgebraicBellmanFord<CSRMatrix>` (real Bellman-Ford with negative-cycle
detection, replacing the previous Dijkstra substitution), `AStar`/
`AStarGeneral`/`STSP` (A* with a caller-supplied heuristic), `MultiTargetBFS`
(multi-target unweighted shortest paths with built-in early termination),
and `SuccessiveShortestPathMinCostFlow` (min-cost max-flow, reading capacity
and supply/demand from named `Graph` attributes rather than constructor
arguments).

### `NetworKit::Embedding::BiasedRandomWalk`

`randomWalk_stream` delegates to `NetworKit::Embedding::BiasedRandomWalk` --
the same P/Q-biased ("return"/"in-out") walker that `NetworKit::Node2Vec`
wraps internally. Node2Vec's *public*, installed header
(`include/networkit/embedding/Node2Vec.hpp`) only exposes the trained
embedding vectors via `getFeatures()`, not the underlying walks -- so it
cannot serve `randomWalk_stream`, which needs the node sequences themselves.

`BiasedRandomWalk.hpp`/`AliasSampler.hpp` live beside their own `.cpp` files
in `third_party/networkit/networkit/cpp/embedding/` rather than under
`third_party/networkit/include/networkit/` (NetworKit's public, installed
header tree). They are still real NetworKit code from the pinned submodule,
compiled into the same `networkit` static library `age_gds` already links
against (confirmed via `nm` on the built library) -- CLAUDE.md forbids
stubbing/aliasing NetworKit types, not using a header that happens to live
under `cpp/` instead of `include/`. `CMakeLists.txt` adds that one directory
to `age_gds`'s private include path with a comment explaining why.

`BiasedRandomWalk::preprocessNode()` reads real edge weights via
`Graph::forNeighborsOf`'s `weight` callback parameter, so
`relationship_weight_property` (via `apply_weight_override()`, plan 00)
works transparently.

**Plan 05 update:** `embedding.cpp`'s `node2vec_stream` no longer goes
through `NetworKit::Node2Vec` at all -- it now calls
`NetworKit::Embedding::BiasedRandomWalk` and
`NetworKit::Embedding::learnEmbeddings` directly, the same two primitives
`Node2Vec::run()` itself calls internally
(`third_party/networkit/networkit/cpp/embedding/Node2Vec.cpp`). This was
required to expose `window_size`/`iterations` as SQL parameters: `Node2Vec::
run()` hardcodes `winSize=10, iterations=1` in its call to
`learnEmbeddings()` and does not accept them as constructor arguments, and
`Node2Vec` is declared `final` so it cannot be subclassed to override
`run()`. `learnEmbeddings()` (declared in the same non-installed
`LearnEmbedding.hpp` covered by the include-path addition above) is the
literal skip-gram trainer NetworKit ships; calling it directly is not a
hand-rolled algorithm, just skipping a wrapper class that hides two of its
parameters. `node2vec_stream` replicates the contiguous-node-id and
no-isolated-nodes checks `Node2Vec`'s constructor used to perform, since
those checks are no longer run implicitly.

### Shared flow-graph transforms (`src/algorithms/flow_common.h`/`.cpp`)

`maxFlow_stats` and `minCostMaxFlow_stats` both need to reduce a
multi-source/multi-sink and/or node-capacity-constrained flow problem to the
single-source/single-sink, edge-capacitated shape NetworKit's flow classes
expect. Two graph *transformations* (not algorithms) are shared:

- `augment_super_source_sink()` -- adds a super-source/super-sink node
  connected to every requested source/sink via an edge capacity large
  enough to never bottleneck a real path (a finite, safely-large value, not
  `infinity()` -- `EdmondsKarp`'s arithmetic produces `NaN` on true
  infinite capacities).
- `split_node_capacities()` -- splits every node `u` into `(u_in, u_out)`
  joined by an edge weighted at `u`'s capacity, so a node-capacity
  constraint becomes an ordinary edge-capacity constraint before handing
  the graph to `EdmondsKarp`/`Dinic`.

`minCostMaxFlow_stats` needed a variant of the same node-splitting idea with
cost (graph weight) and capacity (a named edge attribute) kept separate --
implemented inline in that function rather than via
`split_node_capacities()`, since the shared helper's node-capacity edges use
graph *weight* as capacity (correct for `maxFlow_stats`, where
`EdmondsKarp`/`Dinic` treat weight as capacity) which would collide with
weight's "cost" meaning in the min-cost-flow context.

### `shortestPath_dijkstra_stream` / `shortestPath_astar_stream` path shape

Both return `(total_cost float8, node_ids bigint[], costs float8[])`, with
`node_ids` including both the source and target node. The two underlying
NetworKit base classes disagree on this by default:
`SSSP::getPath()` (used by the Dijkstra-family functions) includes both
endpoints; `STSP::getPath()` (used by `AStar`) explicitly excludes them.
`shortestPath_astar_stream` manually prepends/appends the source/target to
normalize to the same shape. `costs` is the cumulative edge-weight sum from
source to each node in `node_ids`; for the Dijkstra-family functions this
comes for free from the already-computed distance vector
(`SSSP::distance()`), while `shortestPath_astar_stream` (whose `AStar`
instance does not expose per-node distances, only the total) recomputes it
by summing real edge weights along the returned path.

### `allShortestPaths_dijkstra_stream`'s `target_nodes` deviation

The plan suggested `NetworKit::MultiTargetDijkstra` for the `target_nodes`
case, but that class's `STSP` base hardcodes `storePred = false` (no path
reconstruction) for its multi-target constructor -- using it would silently
drop the path output that is this function's entire reason for existing
(as opposed to the older, distance-only `allShortestPaths_stream`).
Instead, when `target_nodes` is given, the wrapper runs one
`NetworKit::Dijkstra` per requested target (each with `storePaths = true`
and that target passed as the early-stop `target` constructor argument) --
still fully delegated to `NetworKit::Dijkstra`, just invoked once per
requested target rather than once for the whole graph.

See also: [Plan: Path-finding & Flow Algorithm Parity Fixes](implemented-plans/03-pathfinding-parity.md)

### New graph-theoretic procedures: `topologicalSort_stream`/`longestPath_stream`/`bridges_stream`

`topologicalSort_stream` and `longestPath_stream` both construct
`NetworKit::TopologicalSort(pg.graph)` and call `.run()`. That constructor
throws `std::runtime_error` if the graph is not directed, and `run()`
throws `std::runtime_error("Error: the input graph has cycles.")` if the
graph is not acyclic -- both cases are caught and re-raised as
`ereport(ERROR, ERRCODE_INVALID_PARAMETER_VALUE, ...)` rather than
producing a partial or silently wrong order.

Both functions then walk `TopologicalSort::getResult()`'s node order once,
computing a per-node distance with a plain array-reduction DP pass (no new
traversal algorithm): `topologicalSort_stream`'s optional `maxDistance`
column increments by 1 per hop (`dist[v] = max(dist[u]) + 1`, over
`pg.graph.forNeighborsOf(u, ...)`, which only visits outgoing edges for
directed graphs); `longestPath_stream` accumulates real edge weight
instead (`dist[v] = max(dist[u] + weight(u,v))`). Both leave source nodes
(no predecessors) at distance 0.

`bridges_stream` derives from `NetworKit::BiconnectedComponents` --
already used by `articulationPoints_stream` in `centrality.cpp` -- with no
new NetworKit dependency: an edge is a bridge iff it is the sole edge of
its biconnected component, which is exactly the case where that
component's `getComponents()` node set has 2 members. One-pass filter over
`BiconnectedComponents::getComponents()`.

See also: [Plan: New Graph-Theoretic Procedures](implemented-plans/06-new-graph-procedures.md)

---

## Similarity: bounded top-K/bottom-K heaps (`src/algorithms/similarity.cpp`)

`nodeSimilarity_stream` used to materialize the full `O(n²)` set of
unordered node pairs before scoring any of them -- the report's flagged
scalability concern. It now scores each pair inline in the same
`u`/`v` double loop (or the `source_node_filter`/`target_node_filter` cross
product, when given) and immediately pushes the result into per-node
bounded heaps: a size-`top_k` min-heap keeps each source node's highest
scores seen so far (evict the smallest when a higher score arrives), and a
size-`bottom_k` max-heap does the mirror image for the lowest scores. This
bounds memory to `O(n·(top_k+bottom_k))` instead of `O(n²)`, while the
scoring computation itself is still delegated to
`NetworKit::JaccardIndex::run(u, v)` (unweighted `JACCARD`) per pair --
identical numerical results to the pre-plan implementation, just without
the memory blowup, and with `similarity_cutoff`/`degree_cutoff`/
`upper_degree_cutoff`/`use_components` as additional pre/post-filters on
the same loop. `OVERLAP`/`COSINE`/weighted-`JACCARD` are closed-form
set-arithmetic formulas over `NetworKit::NeighborhoodUtility` (the same
helper `JaccardIndex` uses internally) and `Graph::degree()`/`Graph::weight()`
-- not new NetworKit classes, per the plan.

Any directed input graph is converted to an undirected view
(`NetworKit::GraphTools::toUndirected`) once, up front, applied uniformly
regardless of `similarity_metric` -- `NetworKit::LinkPredictor::run()`
(underlying unweighted `JACCARD`) unconditionally throws
`std::invalid_argument` on a directed graph, while the manual
`OVERLAP`/`COSINE`/weighted-`JACCARD` formulas would otherwise silently
follow only out-edges on a directed graph without erroring. Converting
once, for every metric, keeps results independent of which metric happens
to be requested.

See also: [Plan: Similarity & Link-Prediction Parity Fixes](implemented-plans/04-similarity-linkprediction-parity.md)

---

## Link prediction: `direction` is a NetworKit gap for non-`'BOTH'` (`src/algorithms/linkprediction.cpp`)

The new single-pair scalar `linkprediction_*` functions accept a
`direction` parameter (`'BOTH'`/`'OUTGOING'`/`'INCOMING'`) matching Neo4j's
`gds.alpha.linkprediction.*` config shape, but only `'BOTH'` is actually
supported. `NetworKit::LinkPredictor::run()` (the base method
`AdamicAdarIndex`/`CommonNeighborsIndex`/`PreferentialAttachmentIndex`/
`ResourceAllocationIndex` all inherit) unconditionally throws
`std::invalid_argument("Only undirected graphs accepted.")` whenever
`Graph::isDirected()` is true -- there is no directed-aware alternative on
any of the four classes to delegate an `OUTGOING`/`INCOMING`-restricted
score to. Passing `direction => 'OUTGOING'` or `'INCOMING'` raises a clear
SQL error (`ERRCODE_FEATURE_NOT_SUPPORTED`) documenting exactly this,
rather than silently ignoring the parameter or hand-rolling directed
neighbor-set arithmetic (forbidden by project rules) -- the same "refuse
with a clear error" precedent set by `labelPropagation_stream`'s
`seed_property` handling (see "A NetworKit gap found during this plan"
above).

See also: [Plan: Similarity & Link-Prediction Parity Fixes](implemented-plans/04-similarity-linkprediction-parity.md)

---

## Cypher-callable interface (`src/util/cypher_agtype.{h,cpp}`)

Every `age_gds` function historically was callable only as plain SQL
(`SELECT * FROM age_gds.pageRank_stream(...)`), never from inside an AGE
`cypher()` block. Each function now also has an `(agtype, agtype)` overload,
resolved by PostgreSQL's normal function-overloading rules (arity/argument
types), so `age_gds.pageRank_stream` (and every other wrapper) has two
independent implementations under the same SQL name: the original typed
SQL signature, unchanged, and the new `(graphName agtype, config agtype
DEFAULT NULL)` signature.

### Why `UNWIND` instead of `CALL ... YIELD`

AGE's Cypher grammar has no `CALL ... YIELD` clause (that is Neo4j's own
procedure framework, not part of core openCypher). A Cypher-callable
function can only appear as a **scalar expression** — it takes `agtype`
arguments and returns one `agtype` value; there is no mechanism for a single
call to fan out into multiple named output columns. The closest equivalent:

```cypher
-- Neo4j:      CALL gds.pageRank.stream('proj', {dampingFactor: 0.85})
--             YIELD nodeId, score RETURN nodeId, score
-- age_gds:
UNWIND age_gds.pageRank_stream('proj', {dampingFactor: 0.85}) AS row
RETURN row.nodeId, row.score
```

`*_stream` wrappers return a JSON array (one map per row) — iterate it with
`UNWIND`. `*_stats`/`*_write` wrappers return a single JSON map directly —
access fields with plain dot notation, no `UNWIND` (a bare map is not a
collection; `UNWIND` on one raises "cannot extract elements from an
object"). `graph_project`'s call shape is the one exception: it mirrors
Neo4j's real `gds.graph.project(graphName, nodeProjection,
relationshipProjection, configuration)` 4-argument shape rather than the
`(graphName, config)` 2-argument convention, since there is no "graph to
select" for a projection call. `graph_list`/`cache_stats` (zero-argument in
SQL) expose their Cypher overload under `graph_list_cypher()`/
`cache_stats_cypher()` instead — Postgres cannot overload two zero-argument
functions that differ only in return type.

### Function-name case sensitivity (AGE-specific, not standard SQL)

**AGE's Cypher parser resolves function names case-sensitively and does not
fold identifiers to lowercase**, unlike ordinary PostgreSQL SQL parsing.
`age_gds.shortestPath_dijkstra_stream(...)` written with that exact case
inside a `cypher()` block raises `function ... does not exist`, even though
the identical spelling works perfectly as plain SQL and the underlying
catalog function name is (like any unquoted SQL identifier) folded to
lowercase at `CREATE FUNCTION` time. **Every mixed-case function must be
called with its all-lowercase spelling inside Cypher** —
`age_gds.shortestpath_dijkstra_stream(...)`, not
`age_gds.shortestPath_dijkstra_stream(...)`. This was discovered empirically
while writing `test/sql/17-cypher-callable-pathfinding.sql`; functions whose
SQL name is already all-lowercase (`bfs_stream`, `dfs_stream`,
`bridges_stream`, ...) are unaffected.

### agtype ↔ jsonb ↔ text plumbing — no new hand-rolled parser

Confirmed via the sibling `age_sql` project (`age_sql/src/regexp.c`): most of
AGE's internal `agtype` API (iterators, structured field access,
`_agtype_build_*`) is **not exported** from `age.so`. Only `agtype_in`/
`agtype_out` are safe to call directly (Postgres's own fmgr dispatch depends
on their signatures staying stable). `src/util/cypher_agtype.cpp` uses
exactly this pair via `DirectFunctionCall`, treating agtype content as JSON
text, and does the actual structured parsing/building with PostgreSQL's
**core** `utils/jsonb.h` API (`Jsonb`, `JsonbIterator`, `pushJsonbValue`,
etc. — not AGE, not a new dependency), the same approach
`src/util/config_parser.cpp` already used for PageRank's `config jsonb`
parameter:

- **Read config**: `agtype_out(arg)` → text → `jsonb_in(text)` (all via
  `DirectFunctionCall`, all core/AGE-stable functions) → walk the resulting
  `Jsonb*` with `JsonbIterator`/`findJsonbValueFromContainer` to pull typed
  fields (`CypherConfig::get_string`/`get_double`/`get_int64_array`/...),
  erroring on unrecognized top-level keys (`CypherConfig::validate_keys`) —
  matches Neo4j's strict config validation.
- **Build result**: `CypherResultBuilder` wraps a `JsonbParseState`/
  `pushJsonbValue` tree (array-of-maps or a single map) → `JsonbValueToJsonb`
  → `JsonbToCString` → text → `agtype_in` → return.
- **Bare scalars**: the four single-pair `linkprediction_*` functions and
  `graph_exists`/`graph_is_stale` are already scalar in SQL (`RETURNS
  float8`/`boolean`) — their Cypher overloads return a bare agtype number/
  boolean (`double_to_agtype_scalar`/`bool_to_agtype_scalar`), no array/map
  packing.

This same `agtype_out` → text → `jsonb_in` pair (and its reverse, `jsonb_out`
→ text → `agtype_in`) is also exposed directly as public SQL functions
— `age_gds.agtype_to_jsonb`/`age_gds.jsonb_to_agtype`
(`age_gds_agtype_to_jsonb`/`age_gds_jsonb_to_agtype` in
`cypher_agtype.cpp`); no `CREATE CAST`s, since AGE >= 1.8.0 ships its own
(`sql/age_gds--1.0.sql`) — not just used internally for Cypher-callable config
parsing. See
[docs/reference/utility.md](reference/utility.md#age_gdsagtype_to_jsonb--age_gdsjsonb_to_agtype).

### Implementation pattern per function

Each function's existing "core" — everything after argument resolution,
before the SRF emit loop — was extracted into a shared internal function
(e.g. `bridges_core`, `run_node_similarity`, `do_graph_project`) returning
an in-memory result. Two thin wrappers call it: the existing SQL SRF entry
point (unchanged behavior/signature, now delegating to the extracted core)
and the new agtype entry point (parse `config` → call core → pack result via
`CypherResultBuilder`). No SPI round-trips between the two wrappers — both
call the same C++ function directly.

### Build

`age_gds.so` links against `age.so` (`CMakeLists.txt`'s `-l:age.so` via
`target_link_directories`/`target_link_libraries` — AGE's PostgreSQL module
has no `lib` prefix, so `find_library`'s `lib*` convention and a plain
absolute-path link both fail; see the CMakeLists.txt comment for why).
`age_gds.so` also needs `INSTALL_RPATH` set explicitly to `PG_PKGLIBDIR`:
CMake clears `RPATH` on install by default, and without it `CREATE
EXTENSION age_gds`'s `dlopen()` can't resolve the `age.so` `DT_NEEDED` entry
even though `age.so` is already resident (loaded via `sql/age_gds--1.0.sql`'s
`LOAD '$libdir/age';` line, required so symbol validation against `age.so`
succeeds at `CREATE FUNCTION` time). `age_gds.control` declares `requires =
'age'`.

See also: [Plan: Cypher-Callable Interface](implemented-plans/cypher-callable-interface.md),
[Plan: `agtype`/`jsonb` SQL Interop](implemented-plans/agtype-jsonb-sql-interop.md)

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
