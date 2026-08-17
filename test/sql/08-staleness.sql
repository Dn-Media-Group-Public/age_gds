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

SELECT age_gds.graph_is_stale('people');
-- expected: false or NULL

SELECT name, "memoryUsage" FROM age_gds.graph_list();
-- expected: row with memoryUsage > 0

SELECT age_gds.graph_drop('people');
\set ON_ERROR_STOP off
SELECT age_gds.graph_is_stale('people');
-- must raise ERROR
\set ON_ERROR_STOP on

DROP EXTENSION age_gds CASCADE;
