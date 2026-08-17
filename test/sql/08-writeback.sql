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

SELECT age_gds.pageRank_write(
    graph_name     => 'people',
    write_property => 'pagerank'
);

SELECT * FROM cypher('test_graph', $$
    MATCH (n:Person) RETURN n.name, n.pagerank
$$) AS t(name agtype, pagerank agtype)
ORDER BY pagerank::float DESC;
-- expected: 4 rows, all non-NULL pagerank

SELECT age_gds.betweenness_write(
    graph_name     => 'people',
    write_property => 'betweenness'
);
SELECT count(*) FROM cypher('test_graph', $$
    MATCH (n:Person) WHERE n.betweenness IS NOT NULL RETURN n
$$) AS t(v agtype);
-- expected: 4

-- Write twice (overwrite, no error)
SELECT age_gds.pageRank_write(
    graph_name     => 'people',
    write_property => 'pagerank'
);

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
