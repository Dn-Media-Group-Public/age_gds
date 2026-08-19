LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- edge_query projects a placeholder "1.0 AS weight" (rather than
-- r.weight AS weight) so the relationship_weight_property => 'weight'
-- override test below actually observes a change (a substitution that
-- ends up textually identical to the stored edge_query is treated as "no
-- convention match" by reweight_from_property() -- see
-- src/bridge/age_loader.cpp).
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$
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

-- plan 05: embedding_dim default should now be 128 (Neo4j parity)
SELECT array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name     => 'people',
    walk_length    => 10,
    walks_per_node => 5
) LIMIT 1;
-- expected: 128

-- plan 05: window_size/iterations plumbing + relationship_weight_property.
-- (node2vec's output is stochastic, so this only proves the call completes
-- with the requested shape and doesn't crash/error on these params, not
-- that the trained vectors differ from an unweighted/default-window run --
-- unlike pageRank_stream's weight-override test (11-weight-override.sql),
-- there is no deterministic downstream metric to diff here.)
SELECT "nodeId", array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name                    => 'people',
    embedding_dim                 => 8,
    walk_length                   => 10,
    walks_per_node                => 5,
    relationship_weight_property  => 'weight',
    window_size                   => 3,
    iterations                    => 2
);
-- expected: 4 rows, dim = 8

-- plan 05 regression guard: window_size can now be caller-supplied and
-- may exceed walk_length (impossible before this plan, since winSize was
-- hardcoded to 10 while walk_length defaulted to 80). Confirmed by reading
-- networkit/cpp/embedding/LearnEmbedding.cpp's trainModel() that the
-- skip-gram context-window loop bounds-checks against the actual walk
-- length via `wordI + a < winSize` / `>= winSize + thisWalk.size()`
-- continue-guards, so this cannot read out of bounds -- this test just
-- guards against that assumption regressing.
SELECT "nodeId", array_length(embedding, 1) AS dim
FROM age_gds.node2vec_stream(
    graph_name     => 'people',
    embedding_dim  => 4,
    walk_length    => 2,
    walks_per_node => 3,
    window_size    => 50,
    iterations     => 2
);
-- expected: 4 rows, dim = 4

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
