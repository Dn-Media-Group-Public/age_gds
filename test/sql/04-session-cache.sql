LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Setup: project a graph from the seeded test_graph
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);

-- Cache starts empty
SELECT entry_count, total_bytes FROM age_gds.cache_stats();
-- expected: 0 | 0

-- Load graph into cache (first call: cache miss → deserialize from disk)
SELECT age_gds.cache_load('people');

-- After first load: entry_count = 1
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 1

-- Second load (cache hit): entry_count unchanged
SELECT age_gds.cache_load('people');
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 1

-- Cleanup
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
