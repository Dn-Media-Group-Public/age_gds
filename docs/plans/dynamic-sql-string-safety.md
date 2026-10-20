# Plan: Dynamic SQL/Cypher string-building safety

## Problem / Goal

Live pentesting (PG16 + AGE + age_gds in a throwaway Docker container) found
several bugs in how `age_gds` builds strings it hands to `SPI_execute`, all
sharing the same root cause: dynamic `cypher(...)`/SQL text built by string
concatenation from caller-supplied values.

1. **Path traversal / arbitrary file write** — `do_graph_project()`
   (`src/catalog/graph_project.cpp`) builds file paths with
   `snprintf(file_path, MAXPGPATH, "%s/%s.bin", data_dir_str, name_str)`
   where `name_str` is the caller-supplied projection name, completely
   unvalidated. Confirmed: `graph_project('../../../../tmp/pwned', ...)`
   wrote its `.bin` file to `/var/tmp/pwned.bin`, outside
   `age_gds.data_dir` entirely. Same pattern applies to the `.nodes`/`.edges`
   paths in incremental mode.

2. **SQL injection via dollar-quote breakout** — `age_load_graph()` and
   `reweight_from_property()` (`src/bridge/age_loader.cpp`) build
   `ag_catalog.cypher(...)` calls by string concatenation:
   ```cpp
   std::string sql = "SELECT * FROM ag_catalog.cypher('" + age_graph_name
                    + "', $$ " + node_query + " $$) AS (id agtype)";
   ```
   `node_query`/`edge_query` are meant to be Cypher text only, but nothing
   stops them containing `$$`, which closes the dollar-quote early and lets
   the rest be parsed as arbitrary SQL by `SPI_execute`. Confirmed:
   embedding `$$) AS (id agtype); SELECT (string_agg(rolname||...))::int
   FROM pg_authid; --` in `node_query` leaked all role names via an
   error-based read. (Postgres itself blocked DDL/DML — `CREATE ROLE`/
   `UPDATE` fail with "not allowed in a non-volatile function" inside
   `SPI_execute(..., read_only=true, ...)` — so today's confirmed impact is
   read-only, but it fully bypasses whatever access boundary the
   Cypher-only interface was meant to enforce.) `age_graph_name` has the
   same problem for a plain `'`.

3. **SQL injection with full write access** — `writeback_scores()`
   (`src/util/writeback.cpp`) has the exact same dollar-quote-breakout bug
   as #2, in the string used by `pagerank_write`/`betweenness_write` to
   push algorithm scores back onto nodes:
   ```cpp
   snprintf(sql, sizeof(sql),
       "SELECT * FROM ag_catalog.cypher('%s', $$ "
       "MATCH (n) WHERE id(n) = %lld SET n.%s = %f RETURN n "
       "$$) AS (v agtype)",
       age_graph, (long long)graphid, property, score);
   int ret = SPI_execute(sql, false, 0);
   ```
   `property` (the caller-supplied `write_property` argument) and
   `age_graph` (when passed directly rather than resolved via the catalog)
   are both unvalidated. Unlike #2, this call uses `SPI_execute(sql, false,
   0)` — **not** read-only — so the DDL/DML restriction that limited #2's
   impact doesn't apply here. Confirmed: calling `pagerank_write` with
   `write_property` set to
   `x = 1 RETURN n $$) AS (v agtype); CREATE ROLE pwned_write LOGIN
   PASSWORD 'x'; SELECT 1 --` actually created the role — full DDL
   execution, not just read-only exfiltration.

4. **SQL injection via unescaped identifier in a DDL string** —
   `install_label_trigger()` (`src/catalog/graph_index.cpp`), reachable
   from `graph_index()` and the `on_new_label` trigger, builds:
   ```cpp
   snprintf(sql, sizeof(sql),
       "CREATE TRIGGER %s AFTER INSERT OR UPDATE OR DELETE ON %s.%s "
       "FOR EACH ROW EXECUTE FUNCTION %s('%s')",
       trig_q, schema_q, label_q, fn, index_name);
   int ret = SPI_execute(sql, false, 0);
   ```
   `trig_q`/`schema_q`/`label_q` are properly escaped via
   `escape_sql_ident()` (double-quote doubling) — but `index_name` (the
   caller-supplied projection `name`) is dropped unescaped into a
   *single*-quoted string-literal argument, and again `read_only=false`.
   Confirmed: `graph_index()` called with `name` set to
   `evilidx'); CREATE ROLE pwned_index LOGIN PASSWORD 'x' --` broke out of
   the literal and created the role.

5. **Same dollar-quote breakout as #2, missed in the first pass** —
   `run_node_property_query()` (`src/util/partition_post.cpp`), used by
   every community-detection SQL wrapper that reads a named node property
   (e.g. `minCommunitySize`/seed-partition support), builds:
   ```cpp
   std::string sql = "SELECT * FROM ag_catalog.cypher('" + age_graph_name
                    + "', $$ " + new_q + " $$) AS (id agtype, value agtype)";
   int ret = SPI_execute(sql.c_str(), true, 0);
   ```
   from an unvalidated `age_graph_name` and an unvalidated `property`
   (spliced into the rewritten query text). `read_only=true` here, so same
   ceiling as #2 (read-only exfiltration).

Bugs #3 and #4 are strictly worse than #2: because they run with
`read_only=false`, the "Postgres blocks DDL/DML inside SPI" ceiling that
limited #2's confirmed impact to read-only exfiltration does **not** apply
— injected `CREATE ROLE`/similar DDL executes for real. Any fix here has to
cover all five sites, not just the two originally found.

We're not trying to eliminate all dynamic string-building — `cypher()`
fundamentally requires its query argument as a source-text `Const`, and AGE
rejects a bound parameter there ("a name constant is expected"), confirmed
empirically. The goal is to make every place that builds one of these
strings do so safely, in a way that composes when `age_gds`, `cypher()`,
and `age_sql.sql_row()` end up nested inside each other arbitrarily deep
(e.g. a projection query that calls `age_sql.sql_row()`, whose SQL text
itself contains another `cypher(...)` call).

## Design

Two independent problems, two independent mechanisms — don't conflate them:

### 1. Identifier-shaped values: validate, don't escape

`name` (projection name → file path component) and `age_graph` (an AGE
graph name) are never meant to be arbitrary text — they're identifiers.
Escaping them is the wrong tool (it invites exactly this kind of bug via
composability): instead, reject anything that isn't a safe identifier
before it touches a file path or a concatenated SQL string.

```cpp
// src/util/sql_safety.h
void validate_safe_identifier(const char *value, const char *argname);
```

Rule: same as an unquoted SQL identifier (matches Postgres table-name
rules) — `^[A-Za-z_][A-Za-z0-9_$]*$`, length bounded to `NAMEDATALEN - 1`
(63 bytes, Postgres's own identifier limit — `name` values longer than
this are already silently truncated by Postgres itself when used as a
real identifier, so rejecting them outright avoids a confusing mismatch
between the validated string and what Postgres actually stores).
`ereport(ERROR, ERRCODE_INVALID_PARAMETER_VALUE)` on violation, naming the
offending argument.

Applied to:
- `name_str` and `age_graph_str` in `do_graph_project()` — closes both the
  path traversal (no `/`, `..`, NUL possible) and the graph-name injection
  (no `'` possible) in one check.
- Each `label`/`type` string in `translate_projection()`
  (`graph_project.cpp`) — currently backtick-quoted into the generated
  Cypher (`` MATCH (n:`" + labels[i] + "`) ``); a label containing a
  backtick or newline is a Cypher-level (not SQL-level) injection into the
  generated query text. Same validator, since AGE labels are identifiers
  too.
- `weight_property` in `reweight_from_property()` — concatenated as
  `<alias>.<weight_property>` with no validation today.
- `property` (the `write_property` argument) in `writeback_scores()`
  (`writeback.cpp`) — concatenated as `n.<property>` in the generated
  `SET` clause; this is the `read_only=false` DDL/DML site (#3), so
  closing it matters more than the read-only ones.
- `age_graph` in `writeback_scores()` when passed in directly (not
  resolved via `lookup_age_graph()`'s catalog lookup).
- `index_name` in `install_label_trigger()` (`graph_index.cpp`) — dropped
  unescaped into a single-quoted string-literal argument of a generated
  `CREATE TRIGGER ... EXECUTE FUNCTION ...('%s')`; this is the other
  `read_only=false` DDL site (#4). (`escape_sql_ident()` already correctly
  double-quote-escapes `schema_q`/`label_q`/`trig_q` in that function —
  only the single-quoted `index_name` argument was missed.)
- `age_graph_name` and `property` in `run_node_property_query()`
  (`partition_post.cpp`) — same shape as the `age_loader.cpp` sites.

This mechanism is *not* meant to be reusable for `node_query`/`edge_query`
— those are meant to hold arbitrary Cypher, including quotes and
backticks. Trying to validate-away danger there would just break
legitimate queries. That's problem 2.

### 2. Free-text query bodies: per-call dollar-quote delimiter

For `node_query`, `edge_query`, the rewritten query in
`reweight_from_property()`, and the rewritten query in
`run_node_property_query()` (`partition_post.cpp`) — arbitrary Cypher text
that must be wrapped in a dollar-quoted literal — replace the hardcoded
`$$` with a delimiter generated fresh for that specific string, guaranteed
not to occur inside it:

(`writeback_scores()`'s `SET n.<property> = <score>` body isn't free text —
once `property` is validated as a safe identifier via mechanism 1, above,
it can no longer contain `$$`, so no dynamic delimiter is needed there;
validation alone closes that site.)

```cpp
// src/util/sql_safety.h
std::string pick_dollar_tag(const std::string &body);
```

Implementation: start from a fixed prefix (e.g. `"agegds_q"`), append an
incrementing counter, and check `"$" + candidate + "$"` is not a substring
of `body`; increment and retry until no collision. (A collision requires
the attacker to predict the exact tag before submitting `body` in the same
call where it's checked against — not achievable across independent
calls.) Call sites become:

```cpp
std::string tag = pick_dollar_tag(node_query);
std::string sql = "SELECT * FROM ag_catalog.cypher('" + age_graph_name
                 + "', $" + tag + "$ " + node_query + " $" + tag + "$) AS (id agtype)";
```

**Why this composes under arbitrary nesting:** each layer only has to
satisfy one local invariant — "my delimiter doesn't appear in the exact
string I'm wrapping, checked at the point I wrap it." It never needs to
know what delimiter an inner or outer layer chose, or whether the content
itself came from another cypher()/sql_row() layer. If `node_query` itself
contains a call to `age_sql.sql_row('...')` whose SQL text contains
another `cypher(...)` call, that inner text is just more bytes inside
`body` as far as `pick_dollar_tag` is concerned — the collision check
still holds by construction.

### Where this code lives

age_gds (C++) and age_sql (C) are independent extensions/repos that both
build dynamic `cypher()`/SQL text and will both need this. Per discussion,
we're **duplicating** a small self-contained helper in each rather than
introducing a cross-extension dependency or a new shared artifact — the
two implementations (`validate_safe_identifier`, `pick_dollar_tag`) are
each under ~30 lines, and age_gds/age_sql already don't share code today.

This plan only covers the age_gds side. age_sql's `sql_row` doesn't build
`cypher()` text dynamically today (it only does `$name → $N` rewriting on
the caller-supplied template, per its own `AGENTS.md` constraint), so it's
not currently vulnerable — but the moment it or any future age_sql feature
starts composing `cypher(...)` calls, it needs the same two helpers. That
should be a separate plan in the `age_sql` repo, written in that repo's
own plan-writing session; flagging it here so it isn't forgotten.

## Implementation order

1. Add `src/util/sql_safety.h` / `sql_safety.cpp`: `validate_safe_identifier()`
   and `pick_dollar_tag()`. Leave a `test_*.cpp`-free, `assert`-based
   `main()`/self-check exercising: valid identifier passes, `../x` and
   `it's` and empty string are rejected, and `pick_dollar_tag` returns a
   tag that doesn't collide with a body already containing `$agegds_q0$`.
2. Wire `validate_safe_identifier()` into `do_graph_project()` for
   `name_str` and `age_graph_str`, into `translate_projection()` for each
   label/type, into `reweight_from_property()` for `weight_property`, into
   `writeback_scores()` for `property` and `age_graph`, and into
   `graph_index()`/`install_label_trigger()` for `name_str`/`index_name`.
3. Replace the hardcoded `$$` sites with `pick_dollar_tag()`-selected
   delimiters: `age_load_graph`'s node/edge queries and
   `reweight_from_property`'s query (`age_loader.cpp`), and
   `run_node_property_query`'s query (`partition_post.cpp`).
4. Add `test/sql/99-security-hardening.sql` covering all five confirmed
   sites: a `graph_project`/`graph_index` call with a path-traversal
   `name` (expect `ERROR`), one with a `'` in `age_graph` (expect
   `ERROR`), one with a `'`/`--` in `graph_index`'s `name` (expect
   `ERROR`, proving #4 is closed), one with a malicious `write_property`
   on `pagerank_write` (expect `ERROR`, proving #3 is closed), and one
   with literal `$$` inside `node_query` (expect it to load correctly,
   proving the fix doesn't just reject that input but actually handles
   it).
5. Update `docs/architecture.md` with a "Dynamic SQL/Cypher string safety"
   section documenting the two mechanisms and the composability argument.
6. Add the constraint to `AGENTS.md`'s "Non-negotiable constraints":
   *"Never concatenate a caller-supplied string into a `cypher(...)` call
   or other dynamic SQL without going through `validate_safe_identifier()`
   or `pick_dollar_tag()`."*

## Files to change

| File | Change |
|---|---|
| `src/util/sql_safety.h` (new) | Declare `validate_safe_identifier()`, `pick_dollar_tag()` |
| `src/util/sql_safety.cpp` (new) | Implement both, plus self-check `main()` |
| `src/catalog/graph_project.cpp` | Validate `name_str`, `age_graph_str` in `do_graph_project()`; validate each label in `translate_projection()` |
| `src/bridge/age_loader.cpp` | Validate `weight_property` in `reweight_from_property()`; replace all three `$$` sites with `pick_dollar_tag()` |
| `src/util/writeback.cpp` | Validate `property`/`age_graph` in `writeback_scores()` |
| `src/catalog/graph_index.cpp` | Validate `name_str`/`index_name` in `graph_index()`/`install_label_trigger()` |
| `src/util/partition_post.cpp` | Validate `property`; replace the `$$` site in `run_node_property_query()` with `pick_dollar_tag()` |
| `test/sql/99-security-hardening.sql` (new) | Regression tests for all five fixes |
| `CMakeLists.txt` | Add `src/util/sql_safety.cpp` to the build |

## Docs to update

- `docs/architecture.md` — new "Dynamic SQL/Cypher string safety" section.
- `AGENTS.md` — new non-negotiable constraint (see step 6 above).
