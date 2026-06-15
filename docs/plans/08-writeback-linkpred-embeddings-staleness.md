# Stage 8 — Write-Back, Link Prediction, Embeddings, Staleness

## Scope

Final stage. Four independent additions:

1. **Write-back** (`_write` variants): store algorithm results as properties
   on AGE nodes.
2. **Link prediction and similarity**: `nodeSimilarity`, `knn`, and
   `linkprediction.*` functions.
3. **Node embeddings**: `node2vec`.
4. **Staleness helper**: `graph_is_stale`, memory estimation,
   `graph_list` memory column.

Builds on Stage 7. All algorithm infrastructure is in place.

---

## 8.1 Write-Back

### Files

```
src/util/writeback.cpp      # write algorithm result back to AGE node properties
```

### Contract

Each algorithm gains a `_write` variant:

```sql
SELECT age_gds.pageRank_write(
    graph_name     => 'myGraph',
    write_property => 'pagerank',
    damping_factor => 0.85
);
-- → void
-- Sets property 'pagerank' on each AGE node in the source graph
```

Mechanism in `writeback.cpp`:
1. Run algorithm to get `(nk_node → score)` map.
2. For each nk_node, resolve `graphid` via `NodeIdMap`.
3. Execute Cypher via SPI:
   ```cypher
   MATCH (n) WHERE id(n) = $graphid
   SET n.<write_property> = $value
   ```
   Use a prepared SPI plan per batch, not one call per node.

Write-back is always against the source `age_graph` recorded in the catalog
(not configurable separately).

---

## 8.2 Link Prediction and Similarity

### Files

```
src/algorithms/
    similarity.cpp          # nodeSimilarity, knn
    linkprediction.cpp      # adamicAdar, commonNeighbors,
                            #      preferentialAttachment, resourceAllocation
```

### SQL Interface

```sql
-- nodeSimilarity: Jaccard index between node pairs
SELECT * FROM age_gds.nodeSimilarity_stream(graph_name => 'myGraph');
-- → (node1 bigint, node2 bigint, similarity float8)

-- knn
SELECT * FROM age_gds.knn_stream(
    graph_name => 'myGraph',
    top_k      => 5
);
-- → (sourceNodeId bigint, targetNodeId bigint, similarity float8, rank int)

-- Link prediction (all return one row per node pair)
SELECT * FROM age_gds.linkprediction_adamicAdar_stream(graph_name => 'myGraph');
SELECT * FROM age_gds.linkprediction_commonNeighbors_stream(graph_name => 'myGraph');
SELECT * FROM age_gds.linkprediction_preferentialAttachment_stream(graph_name => 'myGraph');
SELECT * FROM age_gds.linkprediction_resourceAllocation_stream(graph_name => 'myGraph');
-- → (node1 bigint, node2 bigint, score float8)
```

NetworKit classes: `NetworKit::LinkPredictor` subclasses from
`<networkit/linkprediction/>`:
- `NetworKit::JaccardIndex` → `nodeSimilarity`
- `NetworKit::AdamicAdarIndex` → `linkprediction_adamicAdar`
- `NetworKit::CommonNeighborsIndex` → `linkprediction_commonNeighbors`
- `NetworKit::PreferentialAttachmentIndex` → `linkprediction_preferentialAttachment`
- `NetworKit::ResourceAllocationIndex` → `linkprediction_resourceAllocation`

Do not implement any of these scoring functions from scratch.

---

## 8.3 Node Embeddings

### Files

```
src/algorithms/
    embedding.cpp           # node2vec
```

### SQL Interface

```sql
SELECT * FROM age_gds.node2vec_stream(
    graph_name    => 'myGraph',
    embedding_dim => 64,
    walk_length   => 80,
    walks_per_node => 10,
    p             => 1.0,
    q             => 1.0
);
-- → (nodeId bigint, embedding float8[])
```

NetworKit class: `NetworKit::Node2Vec` from `<networkit/embedding/Node2Vec.hpp>`.

Do not implement random walks or SVD yourself. Pass parameters directly to
`NetworKit::Node2Vec` and extract the embedding matrix from the result.

---

## 8.4 Staleness, Memory Estimation, graph_list Enhancement

### Files

```
src/catalog/
    graph_project.cpp       # extend graph_list with memoryUsage column
                            # implement graph_is_stale
```

### graph_is_stale

```sql
SELECT age_gds.graph_is_stale('myGraph');
-- → boolean
```

Compares `age_gds.graphs.created_at` against
`pg_stat_user_tables.n_mod_since_analyze` (or `last_autoanalyze`) for the
AGE label tables referenced by the projection's `age_graph`. Not a
guarantee; a useful signal.

### graph_list Enhancement

```sql
SELECT * FROM age_gds.graph_list();
-- → adds column: memoryUsage bigint  (estimated bytes for nk::Graph in RAM)
```

Estimated as: `node_count * 8 + edge_count * 20` (rough; matches `.bin`
on-disk size).

---

## Docker Test Environment

Same container and seed data as previous stages.

## Tests

### Write-back

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
);

-- Write PageRank scores back to AGE nodes
SELECT age_gds.pageRank_write(
    graph_name     => 'people',
    write_property => 'pagerank'
);
-- must complete without error

-- Verify property set on nodes
SELECT * FROM cypher('test_graph', $$
    MATCH (n:Person) RETURN n.name, n.pagerank
$$) AS t(name agtype, pagerank agtype)
ORDER BY pagerank::float DESC;
-- expected: 4 rows, all pagerank values non-NULL and > 0

-- Write betweenness
SELECT age_gds.betweenness_write(
    graph_name     => 'people',
    write_property => 'betweenness'
);
SELECT * FROM cypher('test_graph', $$
    MATCH (n:Person) RETURN n.name, n.betweenness
$$) AS t(name agtype, betweenness agtype);
-- expected: 4 rows, no NULL betweenness values

-- write_property collision: writing twice overwrites (no error)
SELECT age_gds.pageRank_write(
    graph_name     => 'people',
    write_property => 'pagerank'
);
-- must complete without error
```

### Link Prediction and Similarity

```sql
-- nodeSimilarity
SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people');
-- expected: > 0 pairs
SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people')
WHERE similarity < 0 OR similarity > 1;
-- expected: 0

-- knn
SELECT count(*) FROM age_gds.knn_stream(
    graph_name => 'people',
    top_k      => 2
);
-- expected: <= 4 * 2 = 8 rows (may be fewer if graph is small)

-- adamicAdar
SELECT count(*) FROM age_gds.linkprediction_adamicAdar_stream(graph_name => 'people');
-- expected: > 0 pairs, no NULLs

-- (repeat for: commonNeighbors, preferentialAttachment, resourceAllocation)
```

### Node Embeddings

```sql
-- node2vec
SELECT nodeId, array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name    => 'people',
    embedding_dim => 8,
    walk_length   => 10,
    walks_per_node => 5
);
-- expected: 4 rows, dim = 8 for each row, no NULL embeddings
```

### Staleness and graph_list

```sql
-- graph_is_stale: newly projected graph is not stale
SELECT age_gds.graph_is_stale('people');
-- expected: false (or NULL if pg_stat_user_tables has no data yet)

-- graph_list includes memoryUsage
SELECT name, "memoryUsage" FROM age_gds.graph_list();
-- expected: 'people' row with memoryUsage > 0

-- After dropping and checking nonexistent
SELECT age_gds.graph_drop('people');
SELECT age_gds.graph_is_stale('people');
-- must raise ERROR (graph does not exist)

-- Cleanup
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] **NetworKit symbols present** (must not regress):
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```

### Write-back
- [ ] `pageRank_write` sets property on all nodes in AGE graph
- [ ] Written values match scores from `pageRank_stream` (same graph)
- [ ] Writing same property twice overwrites without error
- [ ] At least one other algorithm has a `_write` variant tested
      (betweenness recommended)

### Link prediction and similarity
- [ ] `nodeSimilarity_stream` returns pairs with scores in `[0, 1]`
- [ ] `knn_stream` returns at most `top_k` rows per source node
- [ ] All four `linkprediction_*` functions return rows with no NULLs
- [ ] No crash on 4-node test graph for any of the above

### Node embeddings
- [ ] `node2vec_stream` returns one row per node
- [ ] Embedding length equals `embedding_dim` parameter
- [ ] No NULL embeddings

### Staleness and memory
- [ ] `graph_is_stale` returns false for a freshly projected graph
- [ ] `graph_is_stale` raises ERROR for a nonexistent graph
- [ ] `graph_list` includes a non-NULL `memoryUsage` column
