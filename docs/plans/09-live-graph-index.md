# Plan 09 — Live Graph Index

## Problem / Goal

Named projections (`graph_project`) are snapshots: stale the moment the
underlying AGE graph changes.  Anonymous projections require re-running
Cypher queries on every algorithm call.  Both require explicit user action
to stay fresh.

Goal: a third projection mode — a **live index** — that is created once,
maintained automatically via triggers on AGE label tables, and consumed
transparently by all existing algorithm functions.

---

## Design

### SQL API

Two separate creation paths; everything else is unified:

```sql
-- Live index (new): whole-graph, auto-maintained
SELECT age_gds.graph_index(
    'g',                -- index name (same namespace as projections)
    'my_age_graph',
    directed := true,
    weighted := false
);

-- Snapshot projection (unchanged)
SELECT age_gds.graph_project(
    'g', 'my_age_graph',
    '...', '...',       -- node_query, edge_query
    true, false
);

-- Unified post-creation API (both modes)
SELECT age_gds.graph_drop('g');       -- removes triggers for live; removes .bin for snapshot
SELECT age_gds.graph_list();          -- gains `live boolean` column
SELECT age_gds.graph_exists('g');     -- unchanged
SELECT age_gds.graph_is_stale('g');   -- always false for live; unchanged for snapshot
SELECT age_gds.graph_reindex('g');    -- live → compact; snapshot → full rebuild from queries

-- Algorithm calls: transparent; no change
SELECT * FROM age_gds.pageRank_stream(graph_name := 'g');
```

`graph_index` with `node_query` or `edge_query` → `ERRCODE_INVALID_PARAMETER_VALUE`.

### Backward compatibility

All changes are additive.  `graph_project`, all algorithm functions, and
`graph_drop`/`graph_exists`/`graph_is_stale` keep their current signatures.
`graph_list()` gains one column (`live`); queries selecting by name are
unaffected.

### Catalog schema changes

Add to `age_gds.graphs`:

| Column | Type | Default | Purpose |
|---|---|---|---|
| `live` | `boolean` | `false` | live index vs. snapshot |
| `nodes_file_path` | `text` | `NULL` | path to `.nodes` mmap file |
| `edges_file_path` | `text` | `NULL` | path to `.edges` mmap file |

`file_path` (`.bin`) stays for snapshots; `nodes_file_path`/`edges_file_path`
are set for live indices, `file_path` is NULL.

### Mmap file format

Two files per live index, both native-endian binary.

**`<name>.nodes`**

```
Header (64 bytes):
  [0]  uint64  magic            = 0x4147455F4E4F4445  ("AGE_NODE")
  [8]  uint64  version_counter  incremented on every mutation (atomic)
  [16] uint64  node_capacity    total slots (including tombstones)
  [24] uint64  live_node_count  non-tombstoned slots
  [32] uint32  flags            bit0=directed, bit1=weighted
  [36] uint8[28] reserved

Node records (node_capacity × 16 bytes):
  int64  graphid   AGE graphid; -1 = unused/tombstoned
  uint8  dead      1 = tombstoned
  uint8[7] padding
```

Slot index = `nk::node`.  Append node: extend file with `ftruncate`,
`mremap`; write record; increment `live_node_count`.  Delete node: set
`dead = 1`, decrement `live_node_count`.  **Edges for a dead node are NOT
tombstoned in the edge file** — the loader skips them at deserialize time.

**`<name>.edges`**

```
Header (32 bytes):
  [0]  uint64  magic            = 0x4147455F45444745  ("AGE_EDGE")
  [8]  uint64  edge_capacity    total slots (including tombstones)
  [16] uint64  live_edge_count
  [24] uint8[8] reserved

Edge records (edge_capacity × 32 bytes):
  uint64   src_nk     nk::node index of source
  uint64   dst_nk     nk::node index of destination
  float64  weight     1.0 if unweighted
  uint8    dead       1 = tombstoned
  uint8[7] padding
```

Append edge: extend file, write record, increment `live_edge_count`.
Delete edge: look up slot via in-memory `edge_file_index` map (see below),
set `dead = 1`, decrement `live_edge_count`.

### Locking

A named `LWLock` tranche (`age_gds_index`) is reserved in `_PG_init` via
`RequestNamedLWLockTranche` / `GetNamedLWLockTranche`.  One lock slot per
live index (up to a compile-time cap; can raise via GUC if needed).
Trigger mutations hold `LW_EXCLUSIVE`; reads (version-counter check) hold
`LW_SHARED`.  Lock slot looked up by hashing the graph name.

### Session cache additions

`ProjectedGraph` gains:

```cpp
bool live = false;
uint64_t index_version = 0;   // version_counter at last load
// path to mmap'd files (live only):
std::string nodes_file_path;
std::string edges_file_path;
// O(1) edge lookup: (src_nk, dst_nk) → slot index in edges file
std::unordered_map<
    std::pair<nk::node, nk::node>,
    uint64_t,
    PairHash
> edge_file_index;
```

On every cache hit for a live index:
1. `LW_SHARED` lock → read `version_counter` from nodes header.
2. If `version_counter == index_version`: use cached graph.
3. If different: reload from files (deserialize → new `NetworKit::Graph`
   + new `edge_file_index`), update `index_version`.

`PairHash` is a small helper in a new `src/util/pair_hash.h`.

### Deserialization from mmap files

At cache-miss time (or after version mismatch):

1. `mmap(MAP_SHARED)` both files.
2. Validate magic bytes in each header.
3. Iterate node records: skip `dead` slots; for each live slot call
   `id_map.intern(graphid)` and `graph.addNode()`.  Slot index → `nk::node`.
4. Iterate edge records: skip `dead` slots; skip any edge where
   `nodes[src].dead || nodes[dst].dead`; call `graph.addEdge(src, dst, w)`;
   record `edge_file_index[(src, dst)] = slot`.
5. `munmap` both files.
6. Store `index_version = version_counter` read before iteration.

### Trigger installation

When `graph_index(name, age_graph, ...)` is called:

1. Initial load: call `age_load_graph` (same as `graph_project`), write
   `.nodes` and `.edges` files from scratch.
2. Discover label tables by querying `ag_catalog.ag_label`:

```sql
SELECT name, kind   -- kind: 'v' = vertex, 'e' = edge
FROM ag_catalog.ag_label
WHERE graph = (SELECT graphid FROM ag_catalog.ag_graph WHERE name = $1)
```

3. For each label table `<age_graph>.<label>` install via SPI:

```sql
-- vertex label
CREATE TRIGGER age_gds_idx_v_<escaped_name>
    AFTER INSERT OR UPDATE OR DELETE
    ON <age_graph>.<label>
    FOR EACH ROW
    EXECUTE FUNCTION age_gds.index_vertex_trigger();

-- edge label
CREATE TRIGGER age_gds_idx_e_<escaped_name>
    AFTER INSERT OR UPDATE OR DELETE
    ON <age_graph>.<label>
    FOR EACH ROW
    EXECUTE FUNCTION age_gds.index_edge_trigger();
```

Both trigger functions receive the index name via `tgargs[0]` (passed as
trigger argument in the `CREATE TRIGGER` statement).

Trigger names are stored in a new catalog table `age_gds.index_triggers`
so `graph_drop` can remove them:

```sql
CREATE TABLE age_gds.index_triggers (
    graph_name   text  NOT NULL REFERENCES age_gds.graphs(name) ON DELETE CASCADE,
    trigger_name text  NOT NULL,
    table_schema text  NOT NULL,
    table_name   text  NOT NULL,
    PRIMARY KEY (graph_name, trigger_name)
);
```

### Late label addition

AGE inserts a row into `ag_catalog.ag_label` whenever a new vertex or edge
label is created (and therefore a new label table is created).  An
`AFTER INSERT` trigger on `ag_catalog.ag_label` catches this:

```sql
CREATE TRIGGER age_gds_label_watch
    AFTER INSERT ON ag_catalog.ag_label
    FOR EACH ROW
    EXECUTE FUNCTION age_gds.on_new_label();
```

**`age_gds.on_new_label()`** (C function):

1. From `NEW.graph` look up `ag_catalog.ag_graph.name` to get the AGE graph
   name.
2. Query `age_gds.graphs` for any live index on that graph name.
3. If none: return `NEW` immediately (no-op).
4. For each matching live index: install the appropriate data trigger
   (`index_vertex_trigger` or `index_edge_trigger`, chosen by `NEW.kind`)
   on the new label table, and insert a row into `age_gds.index_triggers`.

AGE creates the label table before inserting into `ag_catalog.ag_label`, so
`CREATE TRIGGER` on the new table succeeds unconditionally.  If multiple live
indices exist on
the same AGE graph (different index names), triggers are installed for all
of them.

This trigger is installed once at `CREATE EXTENSION` time and removed at
`DROP EXTENSION`.

### Trigger function logic

**`age_gds.index_vertex_trigger()`** (C function, `PG_FUNCTION_INFO_V1`):

- Retrieve index name from `tgargs[0]`.
- `BEFORE`/`AFTER`, `INSERT`/`UPDATE`/`DELETE` via `TriggerData`.
- INSERT: `NEW.id` → look up or create slot in nodes file; call
  `graph.addNode()` on cached graph if present.
- DELETE: `OLD.id` → find slot; set `dead = 1`; call `graph.removeNode()`
  on cached graph if present.
- UPDATE: if `OLD.id != NEW.id` (rare), treat as DELETE + INSERT.
  Property-only changes: no-op (properties not stored in index).
- Increment `version_counter` under `LW_EXCLUSIVE` lock.

**`age_gds.index_edge_trigger()`** (C function):

- INSERT: `NEW.start_id`, `NEW.end_id`, `NEW.properties` (weight extraction)
  → resolve `nk::node` indices from nodes file; append edge record; call
  `graph.addEdge()` on cached graph; update `edge_file_index`.
- DELETE: `OLD.start_id`, `OLD.end_id` → resolve nk indices; look up slot
  in `edge_file_index`; set `dead = 1`; call `graph.removeEdge()` on cached
  graph; remove from `edge_file_index`.
- UPDATE: treat as DELETE + INSERT.
- Increment `version_counter`.

### `graph_reindex`

For **live index**: compact the files.
1. Read all live node and edge records.
2. Write new `<name>.nodes.tmp` and `<name>.edges.tmp` (compact, no
   tombstones) using same format, new initial `version_counter`.
3. Take `LW_EXCLUSIVE` lock.
4. `rename()` both `.tmp` files atomically.
5. Invalidate session cache entry (`session_cache_invalidate(name)`).
6. Release lock.
Compaction runs synchronously in the calling backend.  Automatic
compaction (threshold-based GUC) is deferred to a future plan.

For **snapshot projection**: re-run `age_load_graph` + `gds_serialize`,
update `node_count`/`edge_count` in catalog.  Equivalent to
`graph_drop` + `graph_project` but without changing the name or requiring
the caller to re-create downstream references.

### `graph_drop` changes

If `live = true`:
1. Look up `age_gds.index_triggers` for this graph name.
2. For each row: execute `DROP TRIGGER <name> ON <schema>.<table>` via SPI.
3. Delete from `age_gds.index_triggers`.
4. Delete `.nodes` and `.edges` files.
5. Delete catalog row, evict from session cache.

Snapshot path unchanged.

### `graph_is_stale` changes

Early-return `false` if `live = true` for the named graph.

---

## Implementation order

1. Schema migration — add columns to `age_gds.graphs`; add
   `age_gds.index_triggers` table; add `graph_index`, `graph_reindex` SQL
   declarations; add `live` column to `graph_list` return type.
2. Shared memory — `RequestNamedLWLockTranche("age_gds_index", N)` in
   `_PG_init`; helper `index_lwlock(name)` returning the right slot.
3. `src/catalog/index_storage.cpp/.h` — mmap file creation, header
   read/write, node append/tombstone, edge append/tombstone, version
   counter increment, compaction (`index_compact`).
4. `src/util/pair_hash.h` — `PairHash` for `std::pair<nk::node, nk::node>`.
5. `ProjectedGraph` additions — `live`, `index_version`, `nodes_file_path`,
   `edges_file_path`, `edge_file_index` fields.
6. Deserialize-from-index path in `session_cache.cpp` — mmap read,
   `edge_file_index` build, version-counter check on cache hit.
7. `src/catalog/graph_index.cpp/.h` — `graph_index()` entry point: initial
   load, file creation, trigger installation, catalog insert.
8. Trigger C functions (`index_vertex_trigger`, `index_edge_trigger`) in
   `graph_index.cpp` — file mutation + in-process cache update.
9. `graph_drop` update — trigger removal + file deletion for live indices.
10. `graph_reindex` — new C entry point in `graph_project.cpp` (or its own
    file); handles both live (compact) and snapshot (rebuild) cases.
11. `graph_is_stale` update — early-return false for live.
12. `CMakeLists.txt` — add `index_storage.cpp`, `graph_index.cpp`.
13. `on_new_label` trigger — C function + `CREATE TRIGGER` on
    `ag_catalog.ag_label` in SQL file; installs data triggers on new label
    tables for any live index on that AGE graph.
14. Tests — new test file `test/sql/09-live-index.sql`; include a test that
    creates a label after `graph_index` and verifies new nodes/edges are
    captured.

---

## Files to change

| File | Change |
|---|---|
| `sql/age_gds--1.0.sql` | Add `live` column to `graphs` table; add `index_triggers` table; add `graph_index`, `graph_reindex` function declarations; update `graph_list` return type; add `on_new_label` trigger on `ag_catalog.ag_label` |
| `src/age_gds.cpp` | Add `RequestNamedLWLockTranche` in `_PG_init`; export new PG_FUNCTION_INFO_V1 symbols |
| `src/catalog/graph_project.cpp` | Update `graph_drop` (trigger removal), `graph_list` (live column), `graph_is_stale` (early-return); add `graph_reindex` |
| `src/catalog/session_cache.cpp` | Version-counter check on live cache hit; deserialize-from-index path; `edge_file_index` population |
| `src/catalog/session_cache.h` | `ProjectedGraph` new fields |
| `src/catalog/index_storage.cpp` | **New** — mmap file management |
| `src/catalog/index_storage.h` | **New** |
| `src/catalog/graph_index.cpp` | **New** — `graph_index()`, trigger functions, `on_new_label()` |
| `src/catalog/graph_index.h` | **New** |
| `src/util/pair_hash.h` | **New** — `PairHash` |
| `CMakeLists.txt` | Add new `.cpp` sources |
| `test/sql/09-live-index.sql` | **New** — tests |

---

## Docs to update

| Doc | Change |
|---|---|
| `docs/api.md` | Document `graph_index`, `graph_reindex`; note `live` column in `graph_list`; note `graph_is_stale` behaviour for live |
| `docs/architecture.md` | Add section on live index: mmap format, trigger mechanism, version-counter invalidation, `edge_file_index` map |
