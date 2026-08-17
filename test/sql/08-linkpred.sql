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

SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people');
-- expected: > 0
SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people')
WHERE similarity < 0 OR similarity > 1;
-- expected: 0

SELECT count(*) FROM age_gds.knn_stream(graph_name => 'people', top_k => 2);
-- expected: <= 8

SELECT count(*) FROM age_gds.linkprediction_adamicAdar_stream(graph_name => 'people');
-- expected: > 0
SELECT count(*) FROM age_gds.linkprediction_commonNeighbors_stream(graph_name => 'people');
SELECT count(*) FROM age_gds.linkprediction_preferentialAttachment_stream(graph_name => 'people');
SELECT count(*) FROM age_gds.linkprediction_resourceAllocation_stream(graph_name => 'people');

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
