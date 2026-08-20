# Centrality algorithms

All centrality functions return one row per node in the graph.

---

### `age_gds.pageRank_stream`

**SQL**

```sql
age_gds.pageRank_stream(
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Computes PageRank scores. Delegates to `NetworKit::PageRank`.

**Parameters:**
- `damping_factor` — teleportation probability, default `0.85`.
- `max_iterations` — maximum power-iteration steps, default `20`.
- `tolerance` — convergence threshold, default `1e-7`.
- `config` — JSONB object; accepted keys: `"dampingFactor"`, `"maxIterations"`,
  `"tolerance"`. Named parameters override JSONB values when both are present.
- `relationship_weight_property` — see
  [Relationship weight override](README.md#relationship-weight-override-weight_property--relationship_weight_property)
  above. Only meaningful with `graph_name` (named/cached graphs); ignored (via
  `apply_weight_override`'s anonymous-mode no-op) when using `age_graph`.
- `scaler` — optional post-processing pass applied to the score vector after
  PageRank has run, matching Neo4j GDS's `scaler` convention: one of
  `'MinMax'`, `'Max'`, `'Mean'`, `'Log'`, `'StdScore'`. `NULL` (default) skips
  scaling. An unrecognized value raises `ERRCODE_INVALID_PARAMETER_VALUE`.
  Implemented once in `src/util/scaler.h`/`.cpp`, shared with
  `eigenvector_stream` and `articleRank_stream`.

Scores sum to approximately 1.0 (before any `scaler`). All scores are
non-negative before scaling; `Log`/`StdScore` scaling can produce negative
or zero values.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.pagerank_stream('proj', {dampingFactor: 0.85}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `pagerank_stream`.
- `config` keys: `dampingFactor`, `maxIterations`, `tolerance`,
  `relationshipWeightProperty`, `scaler`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.pageRank_stats`

**SQL**

```sql
age_gds.pageRank_stats(
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE(
    "ranIterations" int,
    "didConverge"   bool,
    "computeMillis" bigint,
    "minScore"      float8,
    "maxScore"      float8,
    "meanScore"     float8
)
```

Runs PageRank and returns a single summary row instead of per-node scores.
`didConverge` is `true` when `ranIterations < max_iterations`.

`relationship_weight_property` and `scaler` behave identically to
`pageRank_stream` (see above); `minScore`/`maxScore`/`meanScore` are computed
*after* `scaler` is applied.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
RETURN age_gds.pagerank_stats('proj', {dampingFactor: 0.85})
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `pagerank_stats`.
- `config` keys: same as `pageRank_stream`.
- Output: map `{ranIterations, didConverge, computeMillis, minScore,
  maxScore, meanScore}`.

---

### `age_gds.betweenness_stream`

**SQL**

```sql
age_gds.betweenness_stream(
    graph_name                    text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL,
    sampling_size                 int  DEFAULT NULL,
    sampling_seed                 int  DEFAULT NULL,
    relationship_weight_property  text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Betweenness centrality: fraction of shortest paths passing through each node.

**Parameters:**
- `sampling_size` — when set (must be a positive integer), switches from
  exact `NetworKit::Betweenness` to `NetworKit::ApproxBetweenness`, matching
  Neo4j GDS's approximate-betweenness `samplingSize` convention. Internally
  maps to `epsilon = 1 / sqrt(sampling_size)` with a fixed `delta = 0.1`.
  **Requires an undirected graph** — `ApproxBetweenness`'s diameter-estimation
  step does not support directed input and raises an error there (a
  NetworKit limitation, not an age_gds restriction).
- `sampling_seed` — only meaningful together with `sampling_size`; calls
  `Aux::Random::setSeed()` before running, for reproducible sampling.
- `relationship_weight_property` — see
  [Relationship weight override](README.md#relationship-weight-override-weight_property--relationship_weight_property)
  above.

**NetworKit class:** `NetworKit::Betweenness` (normalized=false) or, when
`sampling_size` is set, `NetworKit::ApproxBetweenness`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.betweenness_stream('proj', {samplingSize: 100}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph.
- `config` keys: `samplingSize`, `samplingSeed`, `relationshipWeightProperty`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.closeness_stream`

**SQL**

```sql
age_gds.closeness_stream(
    graph_name          text    DEFAULT NULL,
    age_graph           text    DEFAULT NULL,
    node_query          text    DEFAULT NULL,
    edge_query          text    DEFAULT NULL,
    use_wasserman_faust boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, score float8)
```

Closeness centrality. `use_wasserman_faust = false` (default) uses the
`GENERALIZED` variant, which handles disconnected graphs without dividing by
zero; `use_wasserman_faust = true` uses the `STANDARD` (Wasserman-Faust)
variant, matching Neo4j GDS's `useWassermanFaust` parameter.

**NetworKit class:** `NetworKit::Closeness` (variant=GENERALIZED or STANDARD)

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.closeness_stream('proj', {useWassermanFaust: false}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph.
- `config` keys: `useWassermanFaust`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.harmonicCentrality_stream`

**SQL**

```sql
age_gds.harmonicCentrality_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Harmonic centrality: sum of reciprocal distances. Works correctly on
disconnected graphs (unreachable pairs contribute 0).

**NetworKit class:** `NetworKit::HarmonicCloseness`

**Cypher**

```cypher
UNWIND age_gds.harmoniccentrality_stream('proj') AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `harmoniccentrality_stream`.
- `config` keys: none.
- Output: array of `{nodeId, score}`.

---

### `age_gds.degree_stream`

**SQL**

```sql
age_gds.degree_stream(
    graph_name                    text DEFAULT NULL,
    age_graph                     text DEFAULT NULL,
    node_query                    text DEFAULT NULL,
    edge_query                    text DEFAULT NULL,
    orientation                   text DEFAULT 'NATURAL',
    relationship_weight_property  text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Degree centrality (unnormalized node degree).

**Parameters:**
- `orientation` — one of `'NATURAL'` (out-degree, default), `'REVERSE'`
  (in-degree), or `'UNDIRECTED'` (in-degree + out-degree; on an already
  undirected graph this equals the plain degree). Matches Neo4j GDS's degree
  centrality `orientation` parameter. Any other value raises
  `ERRCODE_INVALID_PARAMETER_VALUE`.
- `relationship_weight_property` — see
  [Relationship weight override](README.md#relationship-weight-override-weight_property--relationship_weight_property)
  above. When set, weighted degree is computed directly via
  `Graph::weightedDegree()`/`weightedDegreeIn()` (no `DegreeCentrality`
  instance is needed for the weighted case — those are already O(1) per
  node on the graph object).

**NetworKit class:** `NetworKit::DegreeCentrality` (normalized=false), or
`Graph::weightedDegree()`/`weightedDegreeIn()` directly when
`relationship_weight_property` is set

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.degree_stream('proj', {orientation: 'NATURAL'}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph.
- `config` keys: `orientation`, `relationshipWeightProperty`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.eigenvector_stream`

**SQL**

```sql
age_gds.eigenvector_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

Eigenvector centrality: node importance proportional to the importance of its
neighbors.

**Parameters:**
- `tolerance` — convergence threshold passed to
  `NetworKit::EigenvectorCentrality`'s constructor, default `1e-7`.
- `relationship_weight_property` — see
  [Relationship weight override](README.md#relationship-weight-override-weight_property--relationship_weight_property)
  above.
- `scaler` — see `pageRank_stream`'s `scaler` parameter above; same allowed
  values, same shared implementation.

**Caveat:** `NetworKit::EigenvectorCentrality`'s power iteration has no
damping term. On a directed graph that is not irreducible (e.g. a directed
acyclic graph), the propagated score vector's magnitude can decay to zero,
which trips an internal NetworKit assertion. This is a limitation of the
vendored class on non-irreducible directed graphs, not an age_gds
restriction; prefer running this on undirected graphs, or directed graphs
known to be strongly connected.

**NetworKit class:** `NetworKit::EigenvectorCentrality`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.eigenvector_stream('proj', {tolerance: 1e-7}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph.
- `config` keys: `tolerance`, `relationshipWeightProperty`, `scaler`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.hits_stream`

**SQL**

```sql
age_gds.hits_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, auth float8, hub float8)
```

HITS (Hyperlink-Induced Topic Search) authority and hub scores.

**Substitution:** NetworKit has no HITS class. Both `auth` and `hub` return
the same `EigenvectorCentrality` score. For undirected graphs this is
equivalent; for directed graphs a proper HITS would use the transposed graph
for authority scores.

**NetworKit class:** `NetworKit::EigenvectorCentrality`

**Cypher**

```cypher
UNWIND age_gds.hits_stream('proj') AS row
RETURN row.nodeId, row.auth, row.hub
```

- `graphName`: named or anonymous graph.
- `config` keys: none.
- Output: array of `{nodeId, auth, hub}`.

---

### `age_gds.articleRank_stream`

**SQL**

```sql
age_gds.articleRank_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
```

ArticleRank centrality.

**Parameters:**
- `damping_factor` — default `0.85`.
- `tolerance` — default `1e-7` (previously hardcoded to `1e-6`, which did not
  match Neo4j GDS's `1e-7` default — fixed as part of
  [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)).
- `relationship_weight_property` — see
  [Relationship weight override](README.md#relationship-weight-override-weight_property--relationship_weight_property)
  above.
- `scaler` — see `pageRank_stream`'s `scaler` parameter above; same allowed
  values, same shared implementation.

`max_iterations` is not exposed (fixed internally at `20`) — matches
`pageRank_stream`'s default and NetworKit's `PageRank` has no separate
ArticleRank-specific iteration cap.

**Substitution:** NetworKit has no ArticleRank class. `NetworKit::PageRank`
is used, with the parameters above.

**NetworKit class:** `NetworKit::PageRank`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.articlerank_stream('proj', {dampingFactor: 0.85}) AS row
RETURN row.nodeId, row.score
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `articlerank_stream`.
- `config` keys: `dampingFactor`, `tolerance`, `relationshipWeightProperty`,
  `scaler`.
- Output: array of `{nodeId, score}`.

---

### `age_gds.articulationPoints_stream`

**SQL**

```sql
age_gds.articulationPoints_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(
    "nodeId"                  bigint,
    "resultingComponentCount" int,
    "resultingComponentMin"   int,
    "resultingComponentMax"   int
)
```

Returns the set of articulation points (cut vertices) — nodes whose removal
increases the number of connected components — one row per articulation
point (not one row per graph node).

**Columns:**
- `nodeId` — the articulation point's AGE graphid.
- `resultingComponentCount`/`resultingComponentMin`/`resultingComponentMax`
  — the number of connected components, and the smallest/largest component
  size, that result from removing this single node from the graph. Computed
  by running `NetworKit::ConnectedComponents` on a private copy of the graph
  (`Graph::removeNode()`) for each reported articulation point; matches
  Neo4j GDS's articulation-points procedure returning per-point
  `componentSizes`/`componentCount`.

**Substitution:** NetworKit has no dedicated ArticulationPoints class. The
point set is computed via `BiconnectedComponents`: a node is an articulation
point if and only if `getComponentsOfNode(u).size() > 1`.

**NetworKit classes:** `NetworKit::BiconnectedComponents`,
`NetworKit::ConnectedComponents`

See also: [Plan: Centrality Algorithm Parity Fixes](../implemented-plans/01-centrality-parity.md)

**Cypher**

```cypher
UNWIND age_gds.articulationpoints_stream('proj') AS row
RETURN row.nodeId, row.resultingComponentCount, row.resultingComponentMin, row.resultingComponentMax
```

- `graphName`: named or anonymous graph. **Case-sensitive call**: use
  `articulationpoints_stream`.
- `config` keys: none.
- Output: array of `{nodeId, resultingComponentCount, resultingComponentMin,
  resultingComponentMax}`.

---

See also: [docs/reference/README.md](README.md)
