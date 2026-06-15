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

SELECT "nodeId", array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name     => 'people',
    embedding_dim  => 8,
    walk_length    => 10,
    walks_per_node => 5
);
-- expected: 4 rows, dim = 8

SELECT count(*) FROM age_gds.node2vec_stream(
    graph_name     => 'people',
    embedding_dim  => 8,
    walk_length    => 10,
    walks_per_node => 5
) WHERE embedding IS NULL;
-- expected: 0

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
