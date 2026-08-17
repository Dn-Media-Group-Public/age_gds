# Plan 10 — Cross-Backend Cache Invalidation + Optional Incremental Reindex for Named Projections

## Problem / Goal

Live indices ([Plan 09](../implemented-plans/09-live-graph-index.md)) are
versioned: every session's cache checks a `version_counter` in the `.nodes`
file header on each cache hit, so a mutation in one backend is picked up by
all other backends without cross-backend coordination.

Named projections (`graph_project`) have no equivalent. `graph_reindex` on a
snapshot rewrites the `.bin` file and calls `session_cache_invalidate` — but
only in the calling backend. Every *other* session that already has the
graph cached keeps serving the pre-reindex `NetworKit::Graph` until it is
LRU-evicted or the backend restarts. A manual reindex is silently invisible
to concurrent sessions.

Separately: `graph_project` can currently only be refreshed by fully
re-running its Cypher queries and rewriting the whole file. For large graphs
with small deltas, an incremental reindex (diff current query results
against what's stored, apply only the delta) can be much cheaper — the same
append/tombstone storage that live indices already use for trigger-driven
updates is equally usable for a *manually* triggered, query-diff-driven
update.

Goal:
1. Give every named graph (both storage kinds) a cheap, version-stamped,
   cross-backend cache-invalidation mechanism.
2. Let `graph_project` optionally opt into the same incremental storage
   format live indices use, enabling a diff-based `graph_reindex`, while
   keeping the current flat, whole-rebuild format as the default (this is
   what Neo4j GDS's `gds.graph.project` semantics assume, and what all
   existing callers rely on).

Storage format and update mechanism (query-diff-driven vs. AGE
trigger-driven) are orthogonal: incremental storage is available to *both*
`graph_project` and `graph_index`; only `graph_index` wires up AGE triggers
on top of it.

---

## Design

### Catalog: `incremental` becomes its own column, independent of `live`

`age_gds.graphs` currently has a single `live boolean` flag which today
implies "uses slot/tombstone storage, auto-updated by triggers." Split
those two concerns:

```sql
ALTER TABLE age_gds.graphs
    ADD COLUMN incremental boolean NOT NULL DEFAULT false;
    -- true  => nodes_file_path/edges_file_path slot/tombstone storage
    -- false => file_path flat .bin storage
```

`live` keeps its current meaning (AGE triggers installed, auto-updated).
`graph_index` always sets `incremental = true` (triggers require
append/tombstone storage — there is no flat-storage live index).
`graph_project` defaults `incremental = false`; passing `incremental :=
true` opts into slot/tombstone storage without installing any triggers.

|              | `live = false`                          | `live = true`            |
|--------------|------------------------------------------|--------------------------|
| `incremental = false` | **default `graph_project`** — flat `.bin`, full-rebuild-only reindex | not possible |
| `incremental = true`  | `graph_project(incremental := true)` — slot/tombstone, diff-or-full reindex | `graph_index` (unchanged) |

### Version stamping, both storage kinds

**Flat storage** (`src/catalog/serialization.h`/`.cpp`): repurpose 8 of the
24 reserved header bytes as `content_version` (uint64). Old files on disk
already zero that region (`reserved[24] = {}`), so they deserialize as
`content_version = 0` — no migration needed, no format-version bump.

```
magic          uint64   GDS_MAGIC
version        uint32   GDS_VERSION
flags          uint32
node_count     uint64
edge_count     uint64
content_version uint64  incremented on every graph_reindex   (was reserved[0..8])
reserved       uint8[16]                                     (was reserved[8..24])
```

New cheap reader, mirroring `index_storage_read_version`:

```cpp
uint64_t gds_read_version(const char *path);   // pread of 8 bytes, no full deserialize
```

`gds_serialize` gains a `uint64_t content_version` parameter. `graph_project`
creation passes `0`. `graph_reindex` (full-rebuild path, flat storage) passes
`old_version + 1`.

**Incremental storage**: already has `version_counter` in the `.nodes`
header (Plan 09) — reused as-is for `graph_project(incremental := true)`,
bumped on every diff-apply or full rebuild exactly like live indices bump it
on every trigger-driven mutation.

### Session cache

`ProjectedGraph.index_version` (currently live-only) is renamed
`content_version` and used for both storage kinds. `session_cache_get`'s
snapshot (flat) branch changes from "return cached graph unconditionally" to
the same check-then-reload pattern already used for the incremental branch:

1. `gds_read_version(pg.file_path)`.
2. Match `pg.content_version` → return cached graph.
3. Mismatch → `gds_deserialize`, replace cache entry.

The incremental branch (currently gated on `pg.live`) is regated on
`pg.incremental` instead — the version-check logic is identical whether or
not triggers are installed on top.

### Diff algorithm for incremental `graph_project` reindex

On `graph_reindex('g')` (default, no `force`, `incremental = true`,
`live = false`):

1. Re-run `node_query` / `edge_query` via `age_load_graph`-style SPI calls
   to get the *new* node graphid set and edge `(src_gid, dst_gid, weight)`
   triples — same query execution as a full rebuild.
2. Load the *current* node/edge slot tables from the `.nodes`/`.edges`
   files (already-live, non-tombstoned records) via the existing
   `index_storage` read path.
3. **Nodes**: set-diff by graphid.
   - In new, not in old → `index_storage` append (new slot).
   - In old, not in new → tombstone existing slot.
4. **Edges**: identity is `(src_gid, dst_gid)` — the same identity already
   used for `edge_file_index` in the live-index path, and the only identity
   available (queries return no edge graphid, only endpoints + weight).
   - In new, not in old → append edge slot (resolving endpoints to
     `nk::node` via the just-updated node slots).
   - In old, not in new → tombstone existing slot.
   - In both, weight differs → **update the existing slot's weight field in
     place** (no tombstone/re-append — this is a pure value overwrite, not
     an identity change).
5. Bump `version_counter` once at the end (single logical mutation, not per
   record) under the same advisory lock `graph_index.cpp` already uses for
   live-index mutation.
6. Update `node_count`/`edge_count` in the catalog row.

This reuses `index_storage`'s existing append/tombstone primitives
unchanged; the only new code is the diff (two hash-set comparisons) and the
weight-update-in-place primitive (new, since live-index triggers never need
it — an AGE `UPDATE` there is already handled as delete+insert).

### `graph_reindex` signature and semantics

```sql
CREATE FUNCTION age_gds.graph_reindex(
    name  text,
    force boolean DEFAULT NULL   -- NULL = mode-appropriate default
) RETURNS void
```

`force IS NULL` preserves the exact current call shape (`graph_reindex('g')`)
for all existing callers/scripts.

| Storage                                   | `force` NULL (default) | `force = true`                          | `force = false`                          |
|--------------------------------------------|-------------------------|------------------------------------------|-------------------------------------------|
| flat (`incremental = false`)               | full rebuild            | full rebuild (accepted, no-op vs. default — for signature completeness) | **error** `ERRCODE_INVALID_PARAMETER_VALUE`: flat storage has no incremental mode |
| incremental, `live = false` (`graph_project(incremental := true)`) | diff-and-apply | full rebuild (rewrite slot files from scratch via fresh `age_load_graph`) | diff-and-apply (explicit, same as default) |
| incremental, `live = true` (`graph_index`) | compact (Plan 09, unchanged) | full re-load from AGE (rediscover labels, rebuild slot files from scratch, triggers stay installed) | compact (explicit, same as default) |

### `graph_project` SQL signature

```sql
CREATE FUNCTION age_gds.graph_project(
    name        text,
    age_graph   text,
    node_query  text,
    edge_query  text,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false,
    incremental boolean DEFAULT false
) RETURNS void
```

Additive parameter with a default that preserves current behavior — no
existing call site changes.

### `graph_list` / `graph_drop`

- `graph_list()` gains an `incremental boolean` output column (alongside the
  existing `live` column).
- `graph_drop`: incremental (non-live) projections delete `.nodes`/`.edges`
  instead of `.bin`; no trigger cleanup needed (only `live = true` rows have
  `index_triggers` entries) — branch on `incremental`/`live` independently
  instead of the current single `live` branch.

### Out of scope

- `graph_is_stale` (drift detection vs. live AGE state without a reindex)
  is unrelated and unchanged.
- No new locking beyond the existing advisory-lock pattern from Plan 09.
- Anonymous/inline (`age_graph` + queries, no `graph_name`) calls are never
  cached across calls and are unaffected by any of this.
- Automatic/background reindexing (threshold-based GUC) stays deferred, as
  in Plan 09.

---

## Implementation order

1. **Catalog migration** — add `incremental boolean NOT NULL DEFAULT false`
   to `age_gds.graphs`.
2. `src/catalog/serialization.h`/`.cpp` — add `content_version` to the flat
   header, `gds_serialize`/`gds_deserialize` signature change, new
   `gds_read_version`.
3. `src/catalog/session_cache.h` — rename `ProjectedGraph::index_version` →
   `content_version`; add `incremental` field (replacing the `live`-gated
   branch condition where it means "uses slot storage").
4. `src/catalog/session_cache.cpp` — flat branch gains version-check-and-
   reload; incremental branch regated on `pg.incremental` instead of
   `pg.live`; set `content_version` correctly on cache-miss load for both
   kinds.
5. `src/catalog/index_storage.h`/`.cpp` — add weight-update-in-place
   primitive for an existing edge slot (identified by `edge_file_index`
   lookup), needed by the diff-apply path.
6. `src/catalog/graph_project.cpp` — `graph_project` gains the
   `incremental` parameter; when true, delegates initial write to
   `index_storage` (same file creation path `graph_index.cpp` uses) instead
   of `gds_serialize`; when false, passes `content_version = 0` to
   `gds_serialize` as before. `graph_drop`/`graph_list` branch on
   `incremental` independently of `live`.
7. `src/catalog/graph_index.cpp` — `graph_reindex` gains the `force`
   parameter and the three-way (flat / incremental-manual / live) dispatch
   table above; new diff-and-apply routine for the incremental-manual case
   (node/edge set-diff + append/tombstone/weight-update via
   `index_storage`); full-rebuild path for incremental-manual re-runs
   `age_load_graph` into fresh slot files (same as live's `force := true`
   path, reused).
8. `sql/age_gds--1.0.sql` — updated `graph_project`, `graph_reindex`,
   `graph_list` declarations; catalog migration statement.
9. Tests — `test/sql/10-*.sql`:
   - flat projection: reindex visible cross-backend (or same-backend via
     `cache_load` + count check, per existing test harness pattern).
   - `graph_project(incremental := true)`: create, mutate underlying AGE
     data, `graph_reindex('g')` (diff default) picks up added/removed
     nodes/edges and in-place weight changes; `graph_reindex('g', force :=
     true)` matches a from-scratch rebuild.
   - flat projection: `graph_reindex('g', force := false)` errors.

---

## Files to change

| File | Change |
|---|---|
| `sql/age_gds--1.0.sql` | `incremental` column on `graphs`; `graph_project` gains `incremental` param; `graph_reindex` gains `force` param; `graph_list` gains `incremental` output column |
| `src/catalog/serialization.h` | `content_version` field, `gds_read_version` declaration, `gds_serialize` param |
| `src/catalog/serialization.cpp` | Write/read `content_version`; implement `gds_read_version` |
| `src/catalog/session_cache.h` | Rename `index_version` → `content_version`; add `incremental` field |
| `src/catalog/session_cache.cpp` | Version-check-and-reload for flat branch; regate incremental branch on `incremental` not `live` |
| `src/catalog/index_storage.h` | Weight-update-in-place primitive declaration |
| `src/catalog/index_storage.cpp` | Implement weight-update-in-place |
| `src/catalog/graph_project.cpp` | `incremental` param handling in `graph_project`; `graph_drop`/`graph_list` branch on `incremental` |
| `src/catalog/graph_index.cpp` | `graph_reindex` three-way dispatch (`force` semantics per storage kind); diff-and-apply routine; full-rebuild-into-slot-files routine |
| `test/sql/10-*.sql` | **New** — reindex visibility + diff/force tests |

---

## Docs to update

| Doc | Change |
|---|---|
| `docs/architecture.md` | Document `content_version` in flat header (parallel to `version_counter`); document `incremental` as orthogonal to `live`; document the diff-and-apply algorithm and its edge-identity assumption `(src_gid, dst_gid)` |
| `docs/api.md` | `graph_project`'s new `incremental` parameter; `graph_reindex`'s new `force` parameter and the per-storage-kind semantics table; `graph_list`'s new `incremental` column |
