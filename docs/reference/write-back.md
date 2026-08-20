# Write-back

Write-back functions compute an algorithm and then persist the scores back to
the corresponding AGE nodes as a Cypher property, using `SET n.<property> = <score>`.

---

### `age_gds.pageRank_write`

**SQL**

```sql
age_gds.pageRank_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'pagerank',
    damping_factor  float8  DEFAULT NULL,
    max_iterations  int     DEFAULT NULL,
    tolerance       float8  DEFAULT NULL,
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
```

Computes PageRank and writes each node's score to the AGE property named by
`write_property`. For named projections, the target AGE graph is resolved from
`age_gds.graphs.age_graph`.

**Cypher**

```cypher
RETURN age_gds.pageRank_write('proj', {writeProperty: 'pagerank', dampingFactor: 0.85})
```

- `graphName`: named-graph only, per the [Graph source](README.md#graph-source-all-algorithm-functions)
  convention.
- `config` keys: `writeProperty`, `dampingFactor`, `maxIterations`, `tolerance`.
- Output: map `{nodePropertiesWritten}`.

---

### `age_gds.betweenness_write`

**SQL**

```sql
age_gds.betweenness_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'betweenness',
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
```

Computes betweenness centrality and writes each node's score to the AGE
property named by `write_property`.

**Cypher**

```cypher
RETURN age_gds.betweenness_write('proj', {writeProperty: 'betweenness'})
```

- `graphName`: named-graph only.
- `config` keys: `writeProperty`.
- Output: map `{nodePropertiesWritten}`.

---

See also: [docs/reference/README.md](README.md)
