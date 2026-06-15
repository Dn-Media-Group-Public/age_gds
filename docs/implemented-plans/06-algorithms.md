# Stage 6 — Remaining Centrality, Community Detection, Path Finding

## Scope

All remaining algorithms from the initial plan, excluding write-back, link
prediction, embeddings, and staleness (those are Stage 8). Each algorithm
follows the same pattern as PageRank from Stage 5: `_stream` and `_stats`
variants, config parsed via `config_parser`, results via SRF helper.

Builds on Stage 5. The infrastructure is complete; this stage is
repetitive but mechanical.

**Every algorithm must delegate to the NetworKit class listed in the mapping
table below. Do not implement any graph algorithm from scratch.** The glue
code pattern is always:
1. Get `ProjectedGraph` from cache (or anonymous path).
2. Instantiate the NetworKit class with `pg.graph` and config parameters.
3. Call `.run()`.
4. Iterate results and emit rows via SRF helper, resolving nk indices to
   AGE graphids via `pg.id_map.resolve(u)`.

If a NetworKit class has a different constructor signature than expected,
adapt the glue code to match the real API. Do not reimplement the algorithm.

## Files to Modify

```
src/algorithms/
    centrality.cpp          # add: betweenness, closeness, harmonicCentrality,
                            #      degree, eigenvector, hits, articleRank,
                            #      articulationPoints
    community.cpp           # louvain, leiden, labelPropagation, wcc, scc,
                            #      kCore, triangleCount, localClusteringCoefficient,
                            #      modularity
    pathfinding.cpp         # shortestPath.dijkstra, allShortestPaths,
                            #      deltaStepping, bellmanFord, bfs, dfs,
                            #      randomWalk, spanningTree, maxFlow
```

## Algorithm → NetworKit Mapping

### Centrality

| SQL function | NetworKit class | Stream output | Stats output |
|---|---|---|---|
| `betweenness_stream` | `centrality.Betweenness` | `(nodeId, score)` | `(min,max,mean,computeMillis)` |
| `closeness_stream` | `centrality.Closeness` | `(nodeId, score)` | same |
| `harmonicCentrality_stream` | `centrality.HarmonicCloseness` | `(nodeId, score)` | same |
| `degree_stream` | `centrality.DegreeCentrality` | `(nodeId, score)` | same |
| `eigenvector_stream` | `centrality.EigenvectorCentrality` | `(nodeId, score)` | same |
| `hits_stream` | `centrality.HITS` | `(nodeId, auth, hub)` | `(computeMillis)` |
| `articleRank_stream` | custom PageRank variant | `(nodeId, score)` | same as pageRank |
| `articulationPoints_stream` | `components.BiconnectedComponents` | `(nodeId)` | `(count, computeMillis)` |

### Community Detection

| SQL function | NetworKit class | Stream output |
|---|---|---|
| `louvain_stream` | `community.PLM` | `(nodeId, communityId)` |
| `leiden_stream` | `community.ParallelLeiden` | `(nodeId, communityId)` |
| `labelPropagation_stream` | `community.LabelPropagation` | `(nodeId, communityId)` |
| `wcc_stream` | `components.ConnectedComponents` | `(nodeId, componentId)` |
| `scc_stream` | `components.StronglyConnectedComponents` | `(nodeId, componentId)` |
| `kCore_stream` | `centrality.CoreDecomposition` | `(nodeId, coreNumber)` |
| `triangleCount_stream` | `centrality.LocalClusteringCoefficient` | `(nodeId, triangleCount)` |
| `localClusteringCoefficient_stream` | `centrality.LocalClusteringCoefficient` | `(nodeId, coefficient)` |
| `modularity_stats` | `community.Modularity` | stats only: `(modularity float8)` |

### Path Finding

| SQL function | NetworKit class | Stream output |
|---|---|---|
| `shortestPath.dijkstra_stream` | `distance.Dijkstra` | `(sourceNodeId, targetNodeId, totalCost, nodeIds)` |
| `allShortestPaths_stream` | `distance.APSP` | `(sourceNodeId, targetNodeId, distance)` |
| `deltaStepping_stream` | `distance.SSSP` | `(nodeId, distance)` |
| `bellmanFord_stream` | `distance.BellmanFord` | `(nodeId, distance)` |
| `bfs_stream` | `traversal.BFS` | `(nodeId, distance)` |
| `dfs_stream` | `traversal.DFS` | `(nodeId, depth)` |
| `randomWalk_stream` | `sampling.RandomWalk` | `(nodeId)` |
| `spanningTree_stream` | `graph.SpanningForest` | `(src bigint, dst bigint, cost float8)` |
| `maxFlow_stats` | `flow.EdmondsKarp` | `(flow float8, computeMillis bigint)` |

Path finding functions that require source/target nodes accept `source_node`
and `target_node` as `bigint` AGE graphids; resolve via `NodeIdMap` before
passing to NetworKit.

## Docker Test Environment

Same container and seed data as Stage 5 (`test_graph` with 4 Person nodes,
5 KNOWS edges, projection `'people'`).

## Tests

Tests follow the same pattern for every algorithm. Minimum assertions per
algorithm:

1. Returns the correct number of rows (one per node, edge, or path as
   appropriate).
2. No NULL values in primary output columns.
3. Output values are in valid range (e.g. scores ≥ 0, community IDs ≥ 0).
4. `_stats` variant returns a single row with no NULLs.
5. No crash or ERROR on the standard 4-node test graph.

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- (Assume 'people' projection exists)

-- === Centrality ===

-- betweenness
SELECT count(*) FROM age_gds.betweenness_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.betweenness_stream(graph_name => 'people')
WHERE score < 0;
-- expected: 0

-- closeness
SELECT count(*) FROM age_gds.closeness_stream(graph_name => 'people');
-- expected: 4

-- degree
SELECT count(*) FROM age_gds.degree_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.degree_stream(graph_name => 'people')
WHERE score < 0;
-- expected: 0

-- hits
SELECT count(*) FROM age_gds.hits_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.hits_stream(graph_name => 'people')
WHERE auth < 0 OR hub < 0;
-- expected: 0

-- (repeat pattern for: harmonicCentrality, eigenvector, articleRank)

-- articulationPoints (returns only articulation point nodes, count may be < 4)
SELECT count(*) >= 0 FROM age_gds.articulationPoints_stream(graph_name => 'people');
-- expected: true (no crash)

-- === Community Detection ===

-- wcc: all nodes assigned to a component
SELECT count(*) FROM age_gds.wcc_stream(graph_name => 'people');
-- expected: 4
SELECT count(DISTINCT "componentId")
FROM age_gds.wcc_stream(graph_name => 'people');
-- expected: 1 (test graph is connected)

-- louvain
SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'people')
WHERE "communityId" < 0;
-- expected: 0

-- kCore
SELECT count(*) FROM age_gds.kCore_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.kCore_stream(graph_name => 'people')
WHERE "coreNumber" < 0;
-- expected: 0

-- (repeat pattern for: leiden, labelPropagation, scc, triangleCount,
--  localClusteringCoefficient)

-- modularity_stats (requires community partition — pass louvain result)
-- (may require a community_id parameter pointing to a prior result;
--  document exact interface when implementing)

-- === Path Finding ===

-- bfs from Alice (get Alice's nodeId first)
SELECT nodeId FROM age_gds.pageRank_stream(graph_name => 'people')
ORDER BY score DESC LIMIT 1
\gset top_node_
SELECT count(*) FROM age_gds.bfs_stream(
    graph_name  => 'people',
    source_node => :'top_node_nodeId'
);
-- expected: 4 (all nodes reachable in connected graph)

-- dijkstra
SELECT count(*) FROM age_gds.shortestPath_dijkstra_stream(
    graph_name  => 'people',
    source_node => :'top_node_nodeId'
);
-- expected: >= 1

-- deltaStepping / bellmanFord: no crash, returns rows
SELECT count(*) >= 0
FROM age_gds.deltaStepping_stream(
    graph_name  => 'people',
    source_node => :'top_node_nodeId'
);

-- spanningTree
SELECT count(*) FROM age_gds.spanningTree_stream(graph_name => 'people');
-- expected: node_count - 1 = 3

-- (repeat pattern for: dfs, randomWalk, allShortestPaths, maxFlow_stats)

-- Cleanup
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] **NetworKit symbols present** (must not regress):
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```

For every algorithm listed in this stage:

- [ ] `_stream` returns correct row count (one per node/edge/path)
- [ ] No NULL values in primary output columns
- [ ] Numeric outputs are in valid range (scores ≥ 0, IDs ≥ 0, etc.)
- [ ] `_stats` variant (where defined) returns a single row with no NULLs
- [ ] No crash or error on the 4-node test graph
- [ ] Path-finding functions that require `source_node` raise a clear ERROR
      when `source_node` is not supplied
