-- test/sql/06-algorithms.sql
-- Stage 6: all centrality, community, and path-finding algorithms.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);

-- Get a source node ID for path algorithms (highest PageRank)
SELECT "nodeId" AS src_id INTO TEMP source_node_tmp
FROM age_gds.pageRank_stream(graph_name => 'people')
ORDER BY score DESC LIMIT 1;

-- =========================================================================
-- Centrality
-- =========================================================================

SELECT count(*) FROM age_gds.betweenness_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.betweenness_stream(graph_name => 'people') WHERE score < 0;
-- expected: 0

SELECT count(*) FROM age_gds.closeness_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.degree_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.hits_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.hits_stream(graph_name => 'people') WHERE auth < 0 OR hub < 0;
-- expected: 0

SELECT count(*) FROM age_gds.harmonicCentrality_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.eigenvector_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.articleRank_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) >= 0 FROM age_gds.articulationPoints_stream(graph_name => 'people');
-- expected: t (no crash)

-- =========================================================================
-- Community Detection
-- =========================================================================

SELECT count(*) FROM age_gds.wcc_stream(graph_name => 'people');
-- expected: 4
SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(graph_name => 'people');
-- expected: 1

SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'people') WHERE "communityId" < 0;
-- expected: 0

SELECT count(*) FROM age_gds.kCore_stream(graph_name => 'people');
-- expected: 4
SELECT count(*) FROM age_gds.kCore_stream(graph_name => 'people') WHERE "coreNumber" < 0;
-- expected: 0

SELECT count(*) FROM age_gds.labelPropagation_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.leiden_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.scc_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.triangleCount_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.localClusteringCoefficient_stream(graph_name => 'people');
-- expected: 4

SELECT modularity FROM age_gds.modularity_stats(graph_name => 'people');
-- expected: 1 row

-- =========================================================================
-- Path Finding
-- =========================================================================

SELECT count(*) FROM age_gds.bfs_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp)
);
-- expected: 4

-- shortestPath_dijkstra_stream is now single-pair, path-returning (see
-- docs/implemented-plans/03-pathfinding-parity.md); use
-- allShortestPaths_dijkstra_stream below for the single-source-to-all case.
SELECT total_cost >= 0 FROM age_gds.shortestPath_dijkstra_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp),
    target_node => (SELECT "nodeId" FROM age_gds.pageRank_stream(graph_name => 'people') ORDER BY score ASC LIMIT 1)
);
-- expected: t

SELECT count(*) FROM age_gds.allShortestPaths_dijkstra_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp)
);
-- expected: >= 1

SELECT count(*) >= 0 FROM age_gds.deltaStepping_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp)
);
-- expected: t

SELECT count(*) FROM age_gds.spanningTree_stream(graph_name => 'people');
-- expected: 3 (n-1 spanning tree edges)

SELECT count(*) FROM age_gds.allShortestPaths_stream(graph_name => 'people');
-- expected: 16 (4*4 all pairs)

SELECT count(*) >= 0 FROM age_gds.bellmanFord_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp)
);
-- expected: t

SELECT count(*) >= 0 FROM age_gds.dfs_stream(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp)
);
-- expected: t

-- randomWalk_stream: source_node -> source_nodes bigint[] (see
-- docs/implemented-plans/03-pathfinding-parity.md).
SELECT count(*) >= 0 FROM age_gds.randomWalk_stream(
    graph_name    => 'people',
    source_nodes  => ARRAY[(SELECT src_id FROM source_node_tmp)],
    walk_length   => 10
);
-- expected: t

SELECT flow FROM age_gds.maxFlow_stats(
    graph_name  => 'people',
    source_node => (SELECT src_id FROM source_node_tmp),
    target_node => (SELECT "nodeId" FROM age_gds.pageRank_stream(graph_name => 'people') ORDER BY score ASC LIMIT 1)
);
-- expected: 1 row

-- =========================================================================
-- Cleanup
-- =========================================================================
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
