LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Project from real AGE graph
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);

-- Catalog row has correct counts
SELECT name, node_count, edge_count FROM age_gds.graph_list();
-- expected: people | 4 | 4

-- Empty node/edge query returns zero-node graph without crash
-- (edge query also returns 3 columns, weight is a literal since no rows match)
SELECT age_gds.graph_project(
    name       => 'empty',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:NoSuchLabel) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:NoSuchLabel)-[r]->(b)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);
SELECT name, node_count, edge_count FROM age_gds.graph_list()
WHERE name = 'empty';
-- expected: empty | 0 | 0

-- Invalid AGE graph name raises error
\set ON_ERROR_STOP off
SELECT age_gds.graph_project(
    name       => 'bad',
    age_graph  => 'nonexistent_graph',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
);
-- expected: ERROR: AGE graph "nonexistent_graph" does not exist
\set ON_ERROR_STOP on

-- Cleanup
SELECT age_gds.graph_drop('people');
SELECT age_gds.graph_drop('empty');
DROP EXTENSION age_gds;
