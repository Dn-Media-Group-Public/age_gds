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

See also: [docs/reference/README.md](README.md)
