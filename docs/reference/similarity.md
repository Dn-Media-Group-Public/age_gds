# Similarity

See also: [Plan: Similarity & Link-Prediction Parity Fixes](../implemented-plans/04-similarity-linkprediction-parity.md)

---

### `age_gds.nodeSimilarity_stream`

**SQL**

```sql
age_gds.nodeSimilarity_stream(
    graph_name                    text     DEFAULT NULL,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    similarity_cutoff             float8   DEFAULT 1e-42,
    degree_cutoff                 int      DEFAULT 1,
    top_k                         int      DEFAULT 10,
    top_n                         int      DEFAULT 0,
    relationship_weight_property  text     DEFAULT NULL,
    upper_degree_cutoff           bigint   DEFAULT 9223372036854775807,
    bottom_k                      int      DEFAULT 10,
    bottom_n                      int      DEFAULT 0,
    source_node_filter            bigint[] DEFAULT NULL,
    target_node_filter            bigint[] DEFAULT NULL,
    use_components                boolean  DEFAULT false,
    similarity_metric             text     DEFAULT 'JACCARD'
) RETURNS TABLE(node1 bigint, node2 bigint, similarity float8)
```

Similarity for node pairs, computed per source node's **bounded top-K /
bottom-K neighbor lists** rather than the full `O(n²)` pairwise matrix (see
`docs/architecture.md`'s "Similarity: bounded top-K/bottom-K heaps"
section for why). `similarity` is in `[0.0, 1.0]` for `JACCARD`/`OVERLAP`/
`COSINE`.

- `similarity_cutoff`: post-filter -- a scored pair is discarded if
  `similarity < similarity_cutoff`. Default `1e-42` effectively means
  "greater than zero".
- `degree_cutoff` / `upper_degree_cutoff`: pre-filter -- a node is only
  considered as a candidate if its degree is within
  `[degree_cutoff, upper_degree_cutoff]` (inclusive). Default excludes only
  isolated (degree-0) nodes.
- `top_k` / `top_n`: for each source node, keep only its `top_k` highest-scoring
  partners (bounded per-node max-heap, `O(n·top_k)` memory instead of
  materializing every pair); `top_n` (`0` = unlimited) then caps the *total*
  number of top-K rows returned across all nodes, globally, by score.
- `bottom_k` / `bottom_n`: same pattern, inverted -- keeps each node's
  `bottom_k` *lowest*-scoring partners (this is Neo4j's Filtered Node
  Similarity feature, folded into this same function rather than a separate
  `_filtered` function). **A default call returns up to `top_k + bottom_k`
  (default `20`) rows per source node** -- the highest- and lowest-scoring
  partners, deduplicated against each other when a node has few enough
  candidates that the two sets would otherwise overlap. Pass `bottom_k => 0`
  to get only the `top_k` behavior most callers expect.
- `source_node_filter` / `target_node_filter` (`bigint[]`, AGE node ids):
  when either is given, only pairs with `node1` drawn from
  `source_node_filter` (or all degree-eligible nodes, if omitted) and
  `node2` drawn from `target_node_filter` (or all degree-eligible nodes, if
  omitted) are considered; unlike the unfiltered mode, this makes each row
  directional (a pair might only appear as `(a, b)`, not also `(b, a)`).
  Raises an error if a listed node id is not in the graph.
- `use_components`: when `true`, runs `NetworKit::ConnectedComponents`
  first and only compares pairs within the same component.
- `similarity_metric`: `'JACCARD'` (default, `|A∩B|/|A∪B|`), `'OVERLAP'`
  (`|A∩B|/min(|A|,|B|)`), or `'COSINE'` (`|A∩B|/sqrt(|A|·|B|)`) over 0/1
  neighbor-membership vectors. Any other value raises an error.
- `relationship_weight_property`: computes **weighted Jaccard**
  (`sum(min(w_u,w_v)) / sum(max(w_u,w_v))` over the union of `u`'s and `v`'s
  neighbor edge weights) instead of the unweighted class -- closed-form
  arithmetic over the graph's edge weights, since NetworKit's `JaccardIndex`
  has no weighted variant. Only valid with `similarity_metric = 'JACCARD'`
  (raises an error otherwise); also raises an error if the resulting graph
  is not weighted (see the common "Relationship weight override" section
  above -- for anonymous graphs, `relationship_weight_property` is a no-op
  by design, so make sure `edge_query`/`weighted` already produce a
  weighted graph).
- Any directed input graph is converted to an undirected view first
  (`NetworKit::GraphTools::toUndirected`), uniformly for every
  `similarity_metric` -- `NetworKit::LinkPredictor::run()` (used for
  unweighted `JACCARD`) unconditionally rejects directed graphs, so this
  keeps behavior consistent across metrics instead of varying by which one
  happens to be requested.

**NetworKit classes:** `NetworKit::JaccardIndex` (unweighted `JACCARD`),
`NetworKit::NeighborhoodUtility` (`OVERLAP`/`COSINE`/weighted `JACCARD` --
the same helper `JaccardIndex` uses internally), `NetworKit::ConnectedComponents`
(`use_components`), `NetworKit::GraphTools::toUndirected` (directed input).

**Cypher**

```cypher
UNWIND age_gds.nodesimilarity_stream('proj', {topK: 10, similarityMetric: 'JACCARD'}) AS row
RETURN row.node1, row.node2, row.similarity
```

- `graphName`: named or anonymous graph, per the [Graph source](README.md#graph-source-all-algorithm-functions)
  convention. **Case-sensitive call**: use the all-lowercase spelling
  `nodesimilarity_stream` inside `cypher()` (see
  [docs/architecture.md](../architecture.md)'s "Cypher-callable interface"
  section).
- `config` keys: `similarityCutoff`, `degreeCutoff`, `topK`, `topN`,
  `relationshipWeightProperty`, `upperDegreeCutoff`, `bottomK`, `bottomN`,
  `sourceNodeFilter`, `targetNodeFilter`, `useComponents`, `similarityMetric`.
- Output: array of `{node1, node2, similarity}`.

---

### `age_gds.knn_stream`

**SQL**

```sql
age_gds.knn_stream(
    graph_name         text   DEFAULT NULL,
    top_k              int    DEFAULT 10,
    age_graph          text   DEFAULT NULL,
    node_query         text   DEFAULT NULL,
    edge_query         text   DEFAULT NULL,
    similarity_cutoff  float8 DEFAULT 0
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, similarity float8, rank int)
```

K-nearest neighbors by Jaccard similarity. For each node returns up to `top_k`
most similar neighbors (score `>= similarity_cutoff`) sorted descending by
similarity. `rank` is 1-based. `top_k` default changed from `5` to `10` to
match Neo4j's `gds.knn.stream` default.

This remains a **topology-Jaccard substitution** for Neo4j's actual
`gds.knn.stream` semantics (K-nearest-neighbors over **node property
vectors**, not graph topology) -- a genuine NetworKit gap (no
property-vector / approximate-NN class); see
`docs/neo4j-gds-parity-external-libs.md` for the adopted-library plan to
close it (hnswlib), which is a separate, additive function, not a
modification of this one.

**NetworKit class:** `NetworKit::JaccardIndex`

**Cypher**

```cypher
UNWIND age_gds.knn_stream('proj', {topK: 10}) AS row
RETURN row.sourceNodeId, row.targetNodeId, row.similarity, row.rank
```

- `graphName`: named or anonymous graph.
- `config` keys: `topK`, `similarityCutoff`.
- Output: array of `{sourceNodeId, targetNodeId, similarity, rank}`.

---

See also: [docs/reference/README.md](README.md)
