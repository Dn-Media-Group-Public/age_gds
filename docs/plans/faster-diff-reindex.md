# Plan: faster diff-and-apply `graph_reindex` — O(1) node-slot lookup, single-open writes, SQL whole-graph load

## Problem / Goal

`age_gds.graph_reindex(name)` on an incremental, non-live projection
(`graph_project(..., incremental => true)`) runs `diff_apply_reindex`
(`src/catalog/graph_index.cpp`). It re-runs `node_query`/`edge_query`, diffs the
result against the stored slot/tombstone files by identity (graphid for nodes,
`(src_gid, dst_gid)` for edges), and applies only the delta.

Measured on an AGE graph of 290,191 vertices and 762,712 edges (PostgreSQL 16,
AGE PG16 branch, release-0.0.1), two separate costs dominate.

### Problem A: applying the delta is O(delta x nodes)

For every added edge, and for every removed node, the diff calls
`index_storage_find_node(nodes_path, graphid)` (`src/catalog/index_storage.cpp`)
to translate a graphid into a slot:

```cpp
for (uint64_t i = 0; i < hdr.node_capacity; i++) {
    NodeRecord rec{};
    pr_checked(fd, &rec, sizeof(rec), sizeof(hdr) + i * sizeof(NodeRecord), ...);   // one pread() per slot
    if (!rec.dead && rec.graphid == graphid) return i;
}
```

That is a linear scan of the nodes file, one `pread()` syscall per slot, executed
twice per added edge. Time for one `graph_reindex` after inserting N new edges:

| New edges | `graph_reindex` |
|---|---|
| 0 | 7.7 s |
| 10 | 8.1 s |
| 100 | 13.2 s |
| 400 | 27.3 s |
| 1,600 | 85.9 s |

After a fixed part, each new edge costs a steady ~48 ms, and that figure grows with
the number of nodes. A first reindex after a bulk load (hundreds of thousands of new
edges) does not finish in a reasonable time and, because the loop never calls
`CHECK_FOR_INTERRUPTS`, cannot be cancelled with `pg_cancel_backend` /
`pg_terminate_backend`.

### Problem B: the fixed ~7.7 s is almost entirely two Cypher reads

`age_load_graph` (`src/bridge/age_loader.cpp`) always runs the stored
`node_query`/`edge_query` through `ag_catalog.cypher(...)`. Even for the plain
whole-graph queries (`MATCH (n) RETURN id(n)` and
`MATCH (a)-[r]->(b) RETURN id(a), id(b), ...`), AGE's planner joins every edge to
both endpoint vertices (a nested loop with one index probe per edge label per
vertex, ~17M buffer hits), even if the endpoints are never used:
`MATCH ()-[r]->() RETURN start_id(r), end_id(r)` takes 5.2 s, no faster than the
join form (5.7-6.4 s). Splitting the query per label would not help: the same
joins run in pieces.

The loader only needs a stream of decimal graphids as text (plus a weight for
edges). Reading them with plain SQL from the inheritance parents
`<graph>._ag_label_vertex` (`id`) and `<graph>._ag_label_edge` (`start_id`,
`end_id`) covers every label, including labels created later, with no table list.
`graphid` has no cast to `bigint`, but its text output is the plain decimal id, so
the existing `SPI_getvalue` + `strtoll` loop works unchanged. Database-side cost
(rows produced and serialized to text, warm cache):

| Read | Cypher | Direct SQL | Speedup |
|---|---|---|---|
| all vertex ids (290,191) | 311 ms | 24 ms | ~13x |
| all edges, weight = 1 (762,712) | 6,241 ms | 82-124 ms | ~50-75x |

Of the 7.7 s no-change reindex, ~6.5 s is these two reads; with SQL reads it should
drop to roughly 1.3 s (deserialization, hash maps and the SPI row loop remain).
That estimate is confirmed or corrected by the benchmark below.

### Goal

Make the apply phase O(delta) instead of O(delta x nodes), and make the
whole-graph case load without Cypher, without changing what `graph_reindex`
produces. After this plan a reindex of a graph of this size should cost on the
order of a second plus a small per-edge cost, and a bulk reindex must finish and
be cancellable.

## Design

### 1. graphid -> slot map, built once per diff

Edge records and `index_storage_add_edge` address nodes by **slot** (position in
the nodes file). The diff works in graphids, so it needs graphid -> slot.

Do not reuse `old_dg.id_map.graphid_to_nk`: `index_storage_deserialize` assigns
NetworKit ids by `graph.addNode()` while skipping dead slots (`slot_to_nk` in
pass 1), so **`nk` equals the slot only while the file has no tombstones**. Any file
that has had a node killed would give wrong slots.

Change:

- `IndexDeserializedGraph` (`index_storage.h`) gains
  `std::unordered_map<int64_t, uint64_t> node_slot_by_graphid`, filled in pass 1
  of `index_storage_deserialize` where the slot index `i` and `rec.graphid` are
  both in hand. No extra I/O, one extra hash insert per live node.
- `diff_apply_reindex` uses that map for every lookup that currently calls
  `index_storage_find_node`:
  - removed node: look up its slot, kill it, erase it from the map;
  - added node: `index_storage_add_node` already returns the new slot; insert
    `gid -> slot`;
  - added edge: look up both endpoints in the map (present, because the node diff
    runs first; keep the existing "skip if missing" guard).
- `index_storage_find_node` is kept (still used by the live-index trigger path,
  one lookup per trigger call); it is no longer called from the diff.
- The diff's local `old_node_gids` set is redundant with the map's key set and is
  replaced by it (one fewer N-sized container).

Memory: one `int64 -> uint64` map of N entries, roughly 40 bytes per node
(~12 MB at 300k nodes, ~400 MB at 10M). The diff already holds two `NodeIdMap`s,
two gid sets and both edge maps, so this is a modest, transient increase.

### 2. Open each file once for the whole apply

`index_storage_add_node/kill_node/add_edge/kill_edge/set_edge_weight` each do
`open()`, read the header, `pread`/`pwrite` the record, `pwrite` the header,
`close()`. A diff of D operations pays about 5D syscalls plus D file opens.

Add a small RAII writer used only by `diff_apply_reindex`:

```cpp
class IndexBatchWriter {
public:
    IndexBatchWriter(const char *nodes_path, const char *edges_path);  // open both rdwr, read both headers
    uint64_t add_node(int64_t graphid);
    void     kill_node(uint64_t slot);
    uint64_t add_edge(uint64_t src_slot, uint64_t dst_slot, double weight);
    void     kill_edge(uint64_t slot);
    void     set_edge_weight(uint64_t slot, double weight);
    void     flush();          // write both headers (capacity/live counts), idempotent
    ~IndexBatchWriter();       // flush(), then close; destructor must not throw
};
```

- Headers (`node_capacity`, `live_node_count`, `edge_capacity`, `live_edge_count`)
  are kept in memory and written once in `flush()`; record writes remain single
  `pwrite`s at their offsets.
- Failure behaviour: the destructor flushes headers reflecting the operations
  actually applied, so an exception partway through leaves the same kind of state
  as today (records beyond `capacity` are ignored by readers; counts match applied
  operations). The existing exception translation to `ERRCODE_INTERNAL_ERROR` stays
  in `diff_apply_reindex`.
- `bump_version` stays a separate call after `flush()`, preserving the rule that
  readers only see a new `version_counter` once the files are consistent.
- The existing per-operation functions are untouched; the live-index trigger path
  keeps using them (one operation per trigger invocation).
- The caller still holds `acquire_index_lock` for the whole apply, as now.

Expected gain from this item alone is modest (microseconds per operation) next to
item 1; it is included because it is the same loop, and it is measured separately
so its contribution is known rather than assumed.

### 3. Optional queries: omitted means "the whole graph", read with SQL

`node_query` and `edge_query` become optional. When omitted (NULL), the projection
is the whole AGE graph (every vertex, every edge), and it is loaded with SQL from
the parents, not with Cypher. This is exactly what the usual whole-graph Cypher
queries mean, with a fast path instead of a planner-hostile join.

**API.**

- `graph_project(name, age_graph, node_query DEFAULT NULL, edge_query DEFAULT NULL,
  directed DEFAULT false, weighted DEFAULT false, incremental DEFAULT false)`.
  The parameters after the two queries already have defaults, so Postgres allows
  this, and callers using named or positional arguments keep working. The Cypher
  form (`graph_project('p', 'g', {nodeProjection...}, ...)`) and the anonymous-mode
  functions that take `age_graph, node_query, edge_query` accept NULL for both with
  the same meaning.
- The catalog columns `age_gds.graphs.node_query` / `edge_query` are already
  nullable; NULL now means whole graph. No schema change.
- Supplying exactly one of the two queries is allowed (e.g. a filtered node set
  with the default edge set is not meaningful, so it is rejected with a clear error:
  both omitted, or both given).

**Weight (uses the existing `weighted` flag, no new option).**

- `weighted = false`: every edge has weight 1. The SQL is
  `SELECT start_id, end_id FROM <graph>._ag_label_edge` plus a constant; it **never
  reads `properties`**, so it does not detoast or parse `r.weight`. This is the case
  that is optimized to the minimum and the case measured in the table above.
- `weighted = true`: the weight is `r.weight` (the `weight` property of the edge);
  an edge with no `weight`, or a non-numeric one, gets 1.0, matching the loader's
  current default. This reads `properties` for every edge
  (`ag_catalog.agtype_access_operator(properties, '"weight"'::agtype)` or
  equivalent; the exact form is confirmed at implementation) and is therefore slower
  than the unweighted case, but still avoids the endpoint joins.
- "Everywhere": every place that loads a whole graph honours the same rule, i.e.
  `weighted ? r.weight : 1`, with the unweighted path never touching `properties`.
  That includes the live index (`graph_index`) initial load and its `force=true`
  reload, which currently run a hard-coded Cypher query that selects `r.weight`
  regardless of the `weighted` flag; they switch to the same SQL loader.

**Loader.** `age_load_graph` takes nullable queries plus the `weighted` flag. For a
NULL query it runs the SQL read above through the same `SPI_execute` + `SPI_getvalue`
+ `strtoll` loop. The "skip edges whose endpoint is not in the node map" check stays,
so dangling edges behave as with the Cypher join. The `age_graph` existence check
stays.

**String safety.** The SQL loader builds statements from a graph name (the schema
holding `_ag_label_vertex` / `_ag_label_edge`) and, for weighted reads and the
property loaders, a property name. These must not be concatenated raw: the schema
is emitted as a quoted identifier (the `escape_sql_ident()` helper used elsewhere
in `graph_index.cpp`), and property names are passed as bound parameters
(`SPI_execute_with_args`) or as escaped literals, never spliced into the text. This
follows the rules in `docs/plans/dynamic-sql-string-safety.md`, which also rewrites
the existing `age_load_graph` / `reweight_from_property` string building; the two
plans touch the same functions in `age_loader.cpp`, so whichever lands second
rebases onto the first.

**Consumers of the stored query text.** Several features currently treat the stored
`edge_query`/`node_query` as Cypher text to re-run or rewrite, and some guard on
`edge_query.empty()`:

| Consumer | Today | With NULL queries |
|---|---|---|
| `graph_reindex` (diff-and-apply, force rebuild) | errors if a query is empty | uses the SQL loader; the "missing queries" check allows NULL |
| `reweight_from_property` / `weight_override.cpp` (per-call weight property override) | regex-edits the stored `edge_query` to swap the `AS weight` expression | reads the named property from `properties` with SQL, restricted to existing edges as now |
| Capacity / node-property loaders in `pathfinding.cpp`, `community.cpp`, `linkprediction.cpp` | re-run the stored `node_query`/`edge_query` text with a property substituted | read the named property from the vertex/edge `properties` with SQL |
| Guards `age_graph.empty() \|\| edge_query.empty()` (`weight_override.cpp:37`, `pathfinding.cpp:1704,1885`, `linkprediction.cpp:150`, `community.cpp:103`) | treat an empty query as "no source graph recorded" | must test `age_graph.empty()` only; "whole graph" is recorded as an explicit in-memory flag on the loaded projection, not as an empty string |

The in-memory `ProjectionGraph` (`session_cache.h`) gains a `whole_graph` flag so that
"query columns are NULL" is distinguishable from "no AGE source recorded"
(`age_graph` empty).

### Not in this plan

- Making the apply interruptible (`CHECK_FOR_INTERRUPTS` in the loop). Worth its own
  small change; not required to fix the complexity.
- Replacing diff-and-apply with change capture (the live-index trigger mechanism).
  Different trade-off (write overhead), separate discussion.
- Filtered projections without Cypher. A projection with a label or relationship
  filter still needs Cypher (or a future SQL form); this plan only optimizes the
  unfiltered case.

## Testing

Three layers: a small correctness test in the existing runner, a Cypher-vs-SQL
equivalence test, and a one-off benchmark in a throw-away container at realistic
size.

### A. Slot-map regression test (`test/sql/23-diff-reindex-slot-map.sql`, normal runner)

Small graph; exercises the case that makes the naive `id_map` approach wrong:

1. Project an incremental, non-live graph; note counts.
2. Delete a vertex that is **not** the last slot (and its edges) with direct SQL on the
   AGE label tables; `graph_reindex`. This creates tombstones.
3. Insert new vertices and edges, including edges whose endpoints lie both before and
   after the tombstoned slot; `graph_reindex` again. This second reindex deserializes
   a file with tombstones, so `nk != slot` and the graphid -> slot map must be correct.
4. Assert equivalence with a fresh projection: `graph_project` the same graph under
   another name and compare `node_count`/`edge_count` (from `graph_list`) and the
   output of deterministic algorithms (BFS reach set from a fixed source;
   weakly-connected-component count).
5. An edge weight change on an existing edge propagates (`set_edge_weight` path).

### B. Whole-graph SQL load equivalence (`test/sql/24-whole-graph-sql-load.sql`, normal runner)

On the shared test graph, project it twice: once with the explicit whole-graph Cypher
queries and once with both queries omitted. Assert identical `node_count`/`edge_count`
and identical results from deterministic algorithms, for each of:

- `weighted = false`: results equal, and a check that the omitted-query projection
  reads no `properties` (e.g. via a graph whose edges have a non-numeric `weight`
  property that would raise or change results if read);
- `weighted = true`: edges with a numeric `weight`, a missing `weight`, and a
  non-numeric `weight`; weighted shortest-path distances equal between the two
  projections (missing / non-numeric => 1.0);
- a graph that gains a new vertex label and edge label after projection: `graph_reindex`
  of the omitted-query projection picks them up;
- dangling edges (an edge row whose endpoint vertex row was deleted with direct SQL)
  are skipped identically;
- omitting exactly one of the two queries raises the documented error;
- the per-call weight override and a capacity/node-property loader work on an
  omitted-query projection and match the Cypher-defined one;
- `graph_index` (live) initial load and `force=true` reload give the same graph as
  before for weighted and unweighted.

### C. Benchmark in a throw-away container

Runs the existing `Dockerfile` image (PostgreSQL 16 + AGE PG16 + age_gds) with
`docker run --rm`, no volumes, so nothing persists. Driver: `docker/bench-reindex.sh`
with SQL in `docker/bench-reindex.sql` (not part of `run-tests.sh`; too slow for
every run).

**Dataset size: 300,000 vertices and 750,000 edges** (round numbers; the
measurements above were taken near this size). Optional scale check at 3x
(900,000 / 2,250,000) to confirm costs no longer grow with node count.

Data generation: pure SQL, directly into the AGE label tables, in committed chunks of
50,000 rows with a progress `RAISE NOTICE` per chunk (no single giant statement):

- vertices spread over a few labels (e.g. 6), small agtype properties;
- edges spread over a few edge labels (e.g. 8), endpoints chosen at random from the
  vertex ids (an id table built once and indexed), so edges fan out across the whole
  node file rather than clustering near the end; a numeric `weight` on most edges for
  the weighted runs.

Procedure, run on each of: (baseline) unmodified release-0.0.1; (+1) item 1 only;
(+1+2); (+1+2+3):

1. Seed the data; `graph_project(..., incremental => true)` with the usual
   whole-graph Cypher queries (baseline and +1, +1+2), and with both queries omitted
   (+1+2+3, `weighted` false and true); `ANALYZE`.
2. Initial project time and no-change `graph_reindex` time.
3. `graph_reindex` after each delta of new edges (random existing endpoints, plus 5% of
   the delta also introducing new vertices appended at the end of the node file):
   N in {100, 400, 1,600, 6,400, 25,600}. Wall time per row (`\timing`).
4. One reindex after a **bulk** delta of 50,000 new edges and the matching new
   vertices, intractable on the baseline; run with `statement_timeout` of 30 minutes
   and record "timed out" if hit.
5. Correctness: `graph_reindex(name, true)` (full rebuild) into a second projection of
   the same graph, compare node count, edge count, BFS reach set from a fixed source
   and component count with the incrementally maintained one.
6. Load-only timing at the 3x scale for Cypher vs SQL, unweighted and weighted, to
   record the weighted-read cost explicitly.

**Acceptance:**

- baseline reproduces the linear per-edge cost (tens of ms per edge) and the Problem A
  table within a factor of ~2, and reproduces the ~6 s Cypher edge read;
- with item 1, per-edge cost is below 1 ms: a 25,600-edge delta costs no more than
  ~1.5x the 0-edge time, and the 50,000-edge bulk reindex finishes in seconds beyond
  the fixed cost;
- with item 3 (unweighted), the no-change reindex and the initial project at 300k/750k
  take under ~2 s each (estimate: ~1.3 s), versus ~7.7 s for Cypher; the weighted SQL
  read is measured and reported (no hard threshold; it must still beat the Cypher read);
- the 3x scale check shows per-edge delta cost essentially unchanged versus 1x (it must
  no longer scale with node count);
- all equivalence checks in A, B and C.5 pass;
- the existing `test/sql/*.sql` suite (in particular `09-live-index.sql` and
  `10-snapshot-version-cache.sql`) still passes.

Results (baseline vs each stage, both scales) are recorded in this plan's "Results"
section when it is moved to `implemented-plans/`.

## Implementation order

1. Add `docker/bench-reindex.sh` + `.sql`; run on the unmodified build and record the
   baseline (confirms Problems A and B at benchmark scale before any change).
2. Add `node_slot_by_graphid` to `IndexDeserializedGraph`, filled in
   `index_storage_deserialize`; switch `diff_apply_reindex` to it; drop `old_node_gids`
   (item 1).
3. Add `test/sql/23-diff-reindex-slot-map.sql`; confirm it passes with and without
   tombstones. Re-run the benchmark; record item-1 results.
4. Add `IndexBatchWriter`; switch `diff_apply_reindex` to it (item 2). Re-run the
   benchmark; record the item-2 contribution.
5. Item 3, in order: nullable queries in `graph_project` (SQL signature, C entry points,
   `do_graph_project`); `whole_graph` flag on `ProjectionGraph`; SQL loader in
   `age_load_graph` (unweighted, then weighted); `graph_reindex`/`graph_index` use it;
   anonymous-mode entry points accept NULL; fix the `empty()` guards; SQL paths for
   the weight override and the property loaders.
6. Add `test/sql/24-whole-graph-sql-load.sql`; run the full suite.
7. Re-run the benchmark with queries omitted; record item-3 results.
8. Docs (below); move this plan to `implemented-plans/`.

## Files to change

| File | Change |
|---|---|
| `src/catalog/index_storage.h` | `node_slot_by_graphid` on `IndexDeserializedGraph`; declare `IndexBatchWriter` |
| `src/catalog/index_storage.cpp` | fill the map in `index_storage_deserialize`; implement `IndexBatchWriter` |
| `src/catalog/graph_index.cpp` | `diff_apply_reindex`: slot map and `IndexBatchWriter`, drop `old_node_gids`; live-index load and `force=true` reload use the SQL whole-graph loader; allow NULL queries in `graph_reindex` |
| `src/bridge/age_loader.{h,cpp}` | nullable `node_query`/`edge_query`; SQL whole-graph read (unweighted never reads `properties`; weighted reads `weight`, default 1.0); SQL path for the weight-property override |
| `src/catalog/graph_project.cpp` | accept NULL queries (both or neither); store NULL; Cypher-form entry point unaffected |
| `src/catalog/session_cache.{h,cpp}` | `whole_graph` flag on `ProjectionGraph`; load via the nullable-query loader |
| `src/util/weight_override.cpp` | no regex on NULL query; guard on `age_graph` only |
| `src/algorithms/pathfinding.cpp`, `community.cpp`, `linkprediction.cpp` | guards test `age_graph` only; capacity / node-property loaders read from `properties` with SQL for whole-graph projections |
| `sql/age_gds--1.0.sql` | `DEFAULT NULL` on `node_query`/`edge_query` of `graph_project` (and anonymous-mode functions) |
| `test/sql/23-diff-reindex-slot-map.sql` | **new**: tombstone/slot-map correctness and equivalence with a fresh projection |
| `test/sql/24-whole-graph-sql-load.sql` | **new**: omitted-query projection equals the explicit whole-graph Cypher projection, weighted and unweighted |
| `docker/bench-reindex.sh`, `docker/bench-reindex.sql` | **new**: throw-away-container benchmark at 300k / 750k (and 3x) |

## Docs to update

- `docs/reference/projection-management.md`: `graph_project` signature (queries
  optional, omitted = whole graph, SQL-loaded; `weighted` semantics); `graph_reindex`
  cost model (apply is O(delta); remaining cost is the full read plus deserialization).
- `docs/architecture.md`: nodes are addressed by slot, NetworKit ids are compacted past
  tombstones so slot != nk after any node removal (why the graphid -> slot map is
  separate from `NodeIdMap`); the whole-graph SQL loader; the `whole_graph` flag.
- `docs/user-guide.md` and `README.md` examples that pass the whole-graph queries
  explicitly: note the shorter form.
- Add "See also: [Plan: Faster diff-and-apply reindex](../implemented-plans/faster-diff-reindex.md)"
  in each updated section; update `docs/implemented-plans/index.md` when moved.
