# Link prediction

`*_stream` functions score **all currently unconnected node pairs** using
`runAll()` and return one row per pair. The single-pair scalar functions
below (no `_stream` suffix, added by plan 04) instead score exactly one
`(node1, node2)` pair via the same predictor classes' base
`run(u, v)` method, matching Neo4j GDS's
`gds.alpha.linkprediction.<name>(node1, node2, {relationshipQuery,
direction})` spot-check shape. Both variants are kept -- the streaming form
is not deprecated.

See also: [Plan: Similarity & Link-Prediction Parity Fixes](../implemented-plans/04-similarity-linkprediction-parity.md)

---

### `age_gds.linkprediction_adamicAdar_stream`

**SQL**

```sql
age_gds.linkprediction_adamicAdar_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Adamic-Adar index: sum of `1 / log(deg(w))` for common neighbors `w`.

**NetworKit class:** `NetworKit::AdamicAdarIndex`

**Cypher**

```cypher
UNWIND age_gds.linkprediction_adamicadar_stream('proj') AS row
RETURN row.node1, row.node2, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use the
  all-lowercase spelling `linkprediction_adamicadar_stream`.
- `config` keys: none.
- Output: array of `{node1, node2, score}`.

---

### `age_gds.linkprediction_commonNeighbors_stream`

**SQL**

```sql
age_gds.linkprediction_commonNeighbors_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Common-neighbors count.

**NetworKit class:** `NetworKit::CommonNeighborsIndex`

**Cypher**

```cypher
UNWIND age_gds.linkprediction_commonneighbors_stream('proj') AS row
RETURN row.node1, row.node2, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `linkprediction_commonneighbors_stream`.
- `config` keys: none.
- Output: array of `{node1, node2, score}`.

---

### `age_gds.linkprediction_preferentialAttachment_stream`

**SQL**

```sql
age_gds.linkprediction_preferentialAttachment_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Preferential attachment: product of node degrees.

**NetworKit class:** `NetworKit::PreferentialAttachmentIndex`

**Cypher**

```cypher
UNWIND age_gds.linkprediction_preferentialattachment_stream('proj') AS row
RETURN row.node1, row.node2, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `linkprediction_preferentialattachment_stream`.
- `config` keys: none.
- Output: array of `{node1, node2, score}`.

---

### `age_gds.linkprediction_resourceAllocation_stream`

**SQL**

```sql
age_gds.linkprediction_resourceAllocation_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
```

Resource allocation index: sum of `1 / deg(w)` for common neighbors `w`.

**NetworKit class:** `NetworKit::ResourceAllocationIndex`

**Cypher**

```cypher
UNWIND age_gds.linkprediction_resourceallocation_stream('proj') AS row
RETURN row.node1, row.node2, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `linkprediction_resourceallocation_stream`.
- `config` keys: none.
- Output: array of `{node1, node2, score}`.

---

### `age_gds.linkprediction_adamicAdar` / `commonNeighbors` / `preferentialAttachment` / `resourceAllocation`

**SQL**

```sql
age_gds.linkprediction_adamicAdar(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
```
(same signature for `commonNeighbors`/`preferentialAttachment`/`resourceAllocation`)

Single-pair scalar score for exactly `(node1, node2)`. `graph_name` is
required -- these always run against an already-projected graph (there is no
`age_graph`/`node_query`/`edge_query` anonymous mode; use `relationship_query`
below for a per-call topology restriction instead).

- `relationship_query`: when given, re-projects the named graph's topology
  against this edge query instead of the one it was originally projected
  with -- this is an **actual different edge set**, not a weight
  substitution (unlike `relationship_weight_property` elsewhere), reusing
  the exact same anonymous-load mechanism `edge_query` provides elsewhere.
  Requires the graph's catalog row to have `age_graph`/`node_query`
  recorded (as usual for anything that needs to re-run a query against the
  projection's source graph).
- `direction`: `'BOTH'` (default), `'OUTGOING'`, or `'INCOMING'`.
  **Only `'BOTH'` is supported.** `NetworKit::LinkPredictor::run()`
  unconditionally throws `std::invalid_argument("Only undirected graphs
  accepted.")` whenever the graph is directed, and none of the four
  predictor classes offer a directed-aware alternative -- there is no
  NetworKit class to delegate an `OUTGOING`/`INCOMING`-restricted score to,
  and computing it by hand would mean hand-rolling directed neighbor-set
  arithmetic (forbidden by project rules). Passing `'OUTGOING'` or
  `'INCOMING'` raises a clear error rather than silently ignoring the
  parameter or computing an unsupported result; a directed input graph is
  still automatically converted to an undirected view for `'BOTH'`.

**NetworKit classes:** `NetworKit::AdamicAdarIndex` /
`NetworKit::CommonNeighborsIndex` / `NetworKit::PreferentialAttachmentIndex` /
`NetworKit::ResourceAllocationIndex` (base `LinkPredictor::run(u, v)`).

**Cypher**

For these four functions the Cypher form is a **plain scalar overload** — no
`UNWIND`/array packing, since the SQL form is already scalar:

```cypher
RETURN age_gds.linkprediction_adamicadar(123, 456, 'proj', {direction: 'BOTH'})
```

- Signature: `age_gds.<name>(node1 agtype, node2 agtype, graphName agtype,
  config agtype DEFAULT NULL)`. **Case-sensitive call**: use the
  all-lowercase spelling (`linkprediction_adamicadar`,
  `linkprediction_commonneighbors`, `linkprediction_preferentialattachment`,
  `linkprediction_resourceallocation`).
- `config` keys: `relationshipQuery`, `direction`.
- Output: bare number (not a map or array).

---

See also: [docs/reference/README.md](README.md)
