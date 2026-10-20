# Projection management

### `age_gds.graph_index`

**SQL**

```sql
age_gds.graph_index(
    name      text,
    age_graph text,
    directed  boolean DEFAULT false,
    weighted  boolean DEFAULT false
) RETURNS void
```

Creates a **live index** projection. Execution:

1. Reads all vertex rows from every vertex-label table in `age_graph`, building
   a `NetworKit::Graph` keyed by AGE graphid.
2. Reads all edge rows from every edge-label table, adding edges.
3. Serializes the initial snapshot to two binary files:
   `<age_gds.data_dir>/<name>.nodes` and `<age_gds.data_dir>/<name>.edges`.
4. Installs AFTER INSERT and AFTER DELETE row-level triggers on every vertex and
   edge label table that exists at creation time. Each trigger atomically appends
   to the `.nodes`/`.edges` file and increments a version counter.
5. Installs `age_gds_label_watch` AFTER INSERT trigger on `ag_catalog.ag_label`
   (shared; one trigger covers all live indices).
6. Records a row in `age_gds.graphs` with `live = true`.

**AGE 1.7.0 trigger bypass limitation:** AGE uses `simple_table_tuple_insert`
for all Cypher DML, bypassing PostgreSQL row-level triggers. This means:
- Vertices and edges inserted via Cypher (`CREATE`) are **not** captured by the
  live index.
- New label tables created via Cypher are **not** detected by the
  `age_gds_label_watch` trigger.

Only mutations issued as direct SQL `INSERT`/`DELETE` on AGE label tables fire
the live-index triggers.

To incorporate Cypher mutations into the live index, drop and recreate it:

```sql
SELECT age_gds.graph_drop('my_index');
SELECT age_gds.graph_index(name => 'my_index', age_graph => 'my_graph');
```

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — `age_graph` does not exist.
- `ERRCODE_INVALID_PARAMETER_VALUE` — `name` already exists with a different
  mode (live vs. snapshot).
- Duplicate `name` raises a unique-constraint violation from `age_gds.graphs`.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md)

**Cypher**

No Cypher-callable overload. `graph_index` has no `(agtype, agtype)` overload
in `sql/age_gds--1.0.sql` — live-index creation is SQL-only; use
`age_gds.graph_project` from Cypher instead (its `incremental` config key
gets you slot/tombstone storage without triggers).

---

### `age_gds.graph_reindex`

**SQL**

```sql
age_gds.graph_reindex(
    name  text,
    force boolean DEFAULT NULL   -- NULL = mode-appropriate default
) RETURNS void
```

Behaviour depends on the projection's storage kind (`incremental`) and mode
(`live`):

| Storage                                   | `force` NULL (default) | `force = true`                          | `force = false`                          |
|--------------------------------------------|-------------------------|------------------------------------------|-------------------------------------------|
| flat (`graph_project` default)             | full rebuild            | full rebuild (accepted, no-op vs. default) | **error** — flat storage has no incremental mode |
| incremental, non-live (`graph_project(incremental := true)`) | diff-and-apply | full rebuild (fresh slot files) | diff-and-apply (explicit, same as default) |
| incremental, live (`graph_index`)          | compact                 | full re-load from AGE (rediscover labels, rebuild slot files, triggers stay installed) | compact (explicit, same as default) |

- **Full rebuild (flat)**: re-runs the stored `node_query`/`edge_query` and
  rewrites the `.bin` file, bumping `content_version` so every backend's
  session cache notices and reloads on next use (see
  [Session cache](../architecture.md#session-cache)).

- **Diff-and-apply (incremental, non-live)**: re-runs `node_query`/`edge_query`
  and diffs the result against the current slot/tombstone tables by identity
  (AGE graphid for nodes, `(src_gid, dst_gid)` for edges). Adds/removes only
  the delta; an edge present in both with a changed weight is updated in place
  rather than tombstoned and re-appended. Bumps `version_counter` once.
  Cost model: the apply phase is O(delta) (graphid -> slot is a hash lookup,
  both files are opened once); what remains is the full read of the AGE graph
  plus deserializing the slot files, independent of the delta size.
  See also: [Plan: Faster diff-and-apply reindex](../implemented-plans/faster-diff-reindex.md)

- **Compact (live, default)**: rewrites the `.nodes`/`.edges` files without
  tombstoned entries. Does **not** reload data from AGE; only removes stale
  entries from the existing files.

- **Full re-load (live, `force := true`)**: rebuilds the slot files from a
  fresh whole-graph AGE scan (same as `graph_index`'s initial load) and
  installs triggers for any newly-discovered labels. Existing triggers are
  left in place.

All modes invalidate the calling backend's own session cache immediately;
every other backend's cache notices the `content_version`/`version_counter`
change on its own next access — no cross-backend signal is needed.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.
- `ERRCODE_INVALID_PARAMETER_VALUE` — `force := false` on a flat projection.
- `ERRCODE_INTERNAL_ERROR` — file I/O failure.

See also: [Plan 09: Live Graph Index](../implemented-plans/09-live-graph-index.md), [Plan 10: Cross-Backend Cache Invalidation](../implemented-plans/10-snapshot-version-cache.md)

**Cypher**

```cypher
RETURN age_gds.graph_reindex('proj', {force: true})
```

- `graphName`: the projection name.
- `config` keys: `force`.
- Output: map `{graphName}`.

---

### `age_gds.graph_project`

**SQL**

```sql
age_gds.graph_project(
    name        text,
    age_graph   text,
    node_query  text    DEFAULT NULL,
    edge_query  text    DEFAULT NULL,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false,
    incremental boolean DEFAULT false
) RETURNS void
```

Projects an AGE Cypher graph into a persistent NetworKit graph. Omit both
`node_query` and `edge_query` (NULL) to project the **whole graph**: every
vertex and every edge, including labels created later, is then read with plain
SQL from `<graph>._ag_label_vertex` / `<graph>._ag_label_edge` instead of
through Cypher (much faster: no endpoint joins). Giving exactly one of the two
is an error. With `weighted = false` every edge has weight 1 and `properties`
is never read; with `weighted = true` the weight is the edge's numeric `weight`
property (missing or non-numeric gives 1.0). The catalog stores NULL queries
for such a projection; `graph_reindex`, the live index (`graph_index`) and the
per-call weight / node-property overrides all use the SQL path for it.
See also: [Plan: Faster diff-and-apply reindex](../implemented-plans/faster-diff-reindex.md)

With explicit queries, execution is:

1. Runs `node_query` against `age_graph` via `ag_catalog.cypher()`. The query
   must return one column named `id` of type `agtype` containing the AGE
   graphid of each node.
2. Runs `edge_query` against `age_graph`. The query must return three columns:
   `src agtype`, `dst agtype`, `weight agtype`. The `weight` column is used
   only when `weighted = true`; otherwise edge weight defaults to `1.0`.
3. Serializes the resulting `NetworKit::Graph`:
   - `incremental = false` (default): to a flat `<age_gds.data_dir>/<name>.bin`
     file. Only a full rebuild (`graph_reindex`) can refresh it.
   - `incremental = true`: to slot/tombstone `<name>.nodes`/`<name>.edges`
     files — the same storage format live indices use, without installing any
     AGE triggers. `graph_reindex` can then diff-and-apply instead of
     rebuilding from scratch.
4. Inserts a row into `age_gds.graphs` with the projection metadata.

The operation is transactional: if the transaction is aborted, the created
file(s) are deleted by a registered `XactCallback`.

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — `age_graph` does not exist in `ag_catalog.ag_graph`.
- `ERRCODE_INTERNAL_ERROR` — serialization or SPI failure.
- Duplicate `name` raises a unique-constraint violation from `age_gds.graphs`.

See also: [Plan 10: Cross-Backend Cache Invalidation](../implemented-plans/10-snapshot-version-cache.md)

**Cypher**

```cypher
RETURN age_gds.graph_project('proj', '*', '*', {ageGraph: 'my_graph', directed: true, weighted: true})
```

- `graphName`: the new projection's name (not a graph *source* — there is no
  existing graph to select here, `graphName` names what's being created).
- Signature: `(graphName, nodeProjection, relationshipProjection, config)`,
  matching Neo4j's real 4-argument shape rather than the `(graphName, config)`
  convention used elsewhere. `nodeProjection`/`relationshipProjection`: `'*'`
  or a map of label/type → `{}` (`'properties'` is unsupported and raises an
  error if given).
- `config` keys: `ageGraph` (required), `directed`, `weighted`, `incremental`.
- Output: map `{graphName}`.

---

### `age_gds.graph_drop`

**SQL**

```sql
age_gds.graph_drop(name text) RETURNS void
```

Drops the projection:
- For `incremental = false` (flat) projections: deletes the catalog row,
  unlinks the `.bin` file, and evicts the entry from the calling backend's
  session cache.
- For `incremental = true` projections (`graph_index`, or `graph_project(
  incremental := true)`): unlinks the `.nodes`/`.edges` files instead.
- For **live index** (`live = true`) projections specifically: additionally
  drops all data triggers installed on AGE label tables for this index
  (entries in `age_gds.index_triggers` are cascade-deleted).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.

**Cypher**

```cypher
RETURN age_gds.graph_drop('proj')
```

- `graphName`: the projection name (no `config` argument — single `graphName`
  arg, matching the SQL form).
- Output: map `{graphName}`.

---

### `age_gds.graph_list`

**SQL**

```sql
age_gds.graph_list() RETURNS TABLE(
    name          text,
    age_graph     text,
    node_count    bigint,
    edge_count    bigint,
    directed      boolean,
    weighted      boolean,
    file_path     text,
    created_at    timestamptz,
    "memoryUsage" bigint,
    live          boolean,
    incremental   boolean
)
```

Returns all rows from `age_gds.graphs`, ordered by `created_at`. The
`live` column is `true` for live-index projections and `false` otherwise. The
`incremental` column is `true` for slot/tombstone storage (`graph_index`, or
`graph_project(incremental := true)`) and `false` for flat `.bin` storage
(`live` implies `incremental`, not vice versa). The `memoryUsage` column is a
derived estimate: `node_count * 8 + edge_count * 20` bytes (approximate
in-memory cost of the deserialized graph).

**Cypher**

```cypher
RETURN age_gds.graph_list_cypher()
```

- `graphName`: none — zero-argument in SQL, so the agtype overload is the
  differently-named `age_gds.graph_list_cypher()` (Postgres cannot overload
  two zero-arg functions by return type alone).
- `config` keys: none.
- Output: array of catalog rows (one map per `age_gds.graphs` row).

---

### `age_gds.graph_exists`

**SQL**

```sql
age_gds.graph_exists(name text) RETURNS boolean
```

Returns `true` if a projection named `name` exists in the catalog, `false`
otherwise. STRICT (NULL input returns NULL).

**Cypher**

```cypher
RETURN age_gds.graph_exists('proj')
```

- `graphName`: the projection name (no `config` argument — single `graphName`
  arg).
- Output: bare boolean.

---

### `age_gds.graph_is_stale`

**SQL**

```sql
age_gds.graph_is_stale(name text) RETURNS boolean
```

Returns `true` if the underlying AGE graph has been modified since the last
`ANALYZE` run. Internally queries `pg_stat_user_tables` for the AGE graph
schema's `_ag_label_vertex` table and checks `n_mod_since_analyze > 0`.

Always returns `false` for **live index** projections (they are never stale
by definition — mutations are captured immediately via triggers).

Returns `false` if no statistics row exists (cannot determine staleness).
STRICT (NULL input returns NULL).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection `name` does not exist.

**Cypher**

```cypher
RETURN age_gds.graph_is_stale('proj')
```

- `graphName`: the projection name (no `config` argument — single `graphName`
  arg).
- Output: bare boolean.

---

See also: [docs/reference/README.md](README.md)
