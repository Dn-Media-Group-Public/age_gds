# Utility

### `age_gds.version`

**SQL**

```sql
age_gds.version() RETURNS text
```

Returns the extension version string (currently `"0.1.0"`). Useful to verify
the C++ entry point is reachable after `CREATE EXTENSION`.

**Cypher**

No Cypher-callable overload. Per
[Plan: Cypher-Callable Interface](../implemented-plans/cypher-callable-interface.md),
`age_gds.version` is used only to prove the shared plumbing during that
plan's implementation and was deliberately left SQL-only rather than given a
fabricated overload.

---

### `age_gds.agtype_to_jsonb` / `age_gds.jsonb_to_agtype`

**SQL**

```sql
age_gds.agtype_to_jsonb(a ag_catalog.agtype) RETURNS jsonb
age_gds.jsonb_to_agtype(j jsonb) RETURNS ag_catalog.agtype
```

Convert between AGE's `agtype` and Postgres's built-in `jsonb`, both ways.
Also registered as explicit casts:

```sql
properties::jsonb                  -- ag_catalog.agtype AS jsonb
'{"a": 1}'::jsonb::ag_catalog.agtype  -- jsonb AS ag_catalog.agtype
```

Both casts are explicit-only (no `AS IMPLICIT`/`AS ASSIGNMENT`) — a plain
`::` cast works with either, but neither will be picked up by implicit
coercion or function overload resolution. `STRICT`: a `NULL` input returns
`NULL`, not an error.

**Only plain scalar/map/array `agtype` values round-trip** — a full vertex,
edge, or path value (e.g. `n` from `MATCH (n) RETURN n`) is rendered by
`agtype_out` with an AGE type suffix (`::vertex`/`::edge`/`::path`), which
is not valid JSON and makes `agtype_to_jsonb`/the `::jsonb` cast raise
`invalid input syntax for type json`. Cast the `properties` column/map
itself (`properties::jsonb`, or `properties(n)::jsonb` from Cypher), not
the vertex/edge/path value.

Motivating case: AGE node/edge `properties` columns are `agtype`, which
`psycopg2` (and most non-AGE-aware clients) has no adapter for — callers
otherwise fetch `properties::text`, strip AGE's `::vertex`/`::edge`/`::path`
suffix, and `json.loads` it themselves. Casting to `jsonb` server-side
(`properties::jsonb`) lets `psycopg2`'s existing built-in `jsonb` adapter
decode the value automatically, with no client-side parsing at all. The
reverse cast exists for symmetry and for write-path callers building a
`jsonb` value client-side that they want to store in an AGE property.

**Cypher**

No Cypher-callable overload, for either direction. `jsonb` has no
representation in Cypher's type system — Cypher property values are
natively `agtype` already, so a Cypher caller never needs either
conversion. Both functions are useful only to a plain-SQL caller reading or
writing AGE's underlying tables directly.

See also:
[Plan: `agtype`/`jsonb` SQL Interop](../implemented-plans/agtype-jsonb-sql-interop.md)

---

See also: [docs/reference/README.md](README.md)
