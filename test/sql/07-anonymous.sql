-- test/sql/07-anonymous.sql
-- Stage 7: anonymous (query-scoped) execution without persistent projection.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Anonymous pageRank_stream: row count
SELECT count(*) FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
-- expected: 4

-- Anonymous pageRank_stream: scores sum to 1
SELECT abs(sum(score) - 1.0) < 0.0001
FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
-- expected: t

-- Anonymous execution does NOT populate session cache
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 0

-- Anonymous pageRank_stats
SELECT "ranIterations", "didConverge"
FROM age_gds.pageRank_stats(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
-- expected: 1 row, no NULLs

-- Anonymous wcc_stream
SELECT count(*) FROM age_gds.wcc_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
-- expected: 4

-- Capture highest-PageRank nodeId for BFS
SELECT "nodeId" AS src_id INTO TEMP anon_src
FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
) ORDER BY score DESC LIMIT 1;

-- Anonymous bfs_stream
SELECT count(*) FROM age_gds.bfs_stream(
    age_graph   => 'test_graph',
    node_query  => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query  => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                    RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    source_node => (SELECT src_id FROM anon_src)
);
-- expected: 4

-- Error: both graph_name and age_graph
\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(
    graph_name => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
\set ON_ERROR_STOP on

-- Error: age_graph without node_query
\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
\set ON_ERROR_STOP on

-- Error: neither graph_name nor age_graph
\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(damping_factor => 0.85);
\set ON_ERROR_STOP on

-- Persistent path still works after anonymous calls
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'people');
-- expected: 4

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
