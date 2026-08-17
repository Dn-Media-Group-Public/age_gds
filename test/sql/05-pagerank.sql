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

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'people');
-- expected: 4

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'people')
WHERE score < 0;
-- expected: 0

SELECT abs(sum(score) - 1.0) < 0.0001
FROM age_gds.pageRank_stream(graph_name => 'people');
-- expected: true

SELECT count(*) FROM age_gds.pageRank_stream(
    graph_name     => 'people',
    damping_factor => 0.5,
    max_iterations => 5
);
-- expected: 4

SELECT count(*) FROM age_gds.pageRank_stream(
    graph_name => 'people',
    config     => '{"dampingFactor": 0.9, "maxIterations": 10}'::jsonb
);
-- expected: 4

SELECT "ranIterations", "didConverge", "computeMillis", "minScore", "maxScore", "meanScore"
FROM age_gds.pageRank_stats(graph_name => 'people');

SELECT entry_count FROM age_gds.cache_stats();
-- expected: >= 1

\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(graph_name => 'no_such_graph');
\set ON_ERROR_STOP on

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
