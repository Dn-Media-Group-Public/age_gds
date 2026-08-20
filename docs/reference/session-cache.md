# Session cache

### `age_gds.cache_stats`

**SQL**

```sql
age_gds.cache_stats() RETURNS TABLE(
    entry_count int,
    total_bytes bigint,
    limit_bytes bigint
)
```

Returns one row describing the calling backend's session cache:
- `entry_count` — number of graphs currently loaded.
- `total_bytes` — sum of `approx_bytes` across all cached graphs.
- `limit_bytes` — the current `age_gds.session_cache_mb` GUC value in bytes.

**Cypher**

```cypher
RETURN age_gds.cache_stats_cypher()
```

- `graphName`: none — zero-argument in SQL, so (per
  [docs/architecture.md](../architecture.md)'s "Cypher-callable interface"
  section) the agtype overload is the differently-named
  `age_gds.cache_stats_cypher()` rather than an overload of `cache_stats`
  (Postgres cannot overload two zero-arg functions by return type alone).
- `config` keys: none.
- Output: single map `{entryCount, totalBytes, limitBytes}`.

---

### `age_gds.cache_load`

**SQL**

```sql
age_gds.cache_load(graph_name text) RETURNS void
```

Pre-warms the session cache by loading `graph_name` from disk if not already
cached. Equivalent to calling any algorithm with `graph_name` and discarding
the result; useful for pre-loading before latency-sensitive queries.
STRICT (NULL input returns NULL).

**Errors:**
- `ERRCODE_UNDEFINED_OBJECT` — projection does not exist.
- `ERRCODE_INTERNAL_ERROR` — `.bin` file cannot be read.

**Cypher**

```cypher
RETURN age_gds.cache_load('proj')
```

- `graphName`: the projection name, as a bare `agtype` string (no `config`
  argument — single `graphName` arg, matching the SQL form).
- Output: map `{graphName}`.

---

See also: [docs/reference/README.md](README.md)
