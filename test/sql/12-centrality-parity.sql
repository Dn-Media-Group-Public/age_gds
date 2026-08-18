-- Centrality algorithm parity fixes (docs/implemented-plans/01-centrality-parity.md)
--
-- Exercises the new optional parameters added to close ✅/⚠️-closable gaps
-- against Neo4j GDS: scaler (pageRank/eigenvector/articleRank), betweenness
-- sampling (ApproxBetweenness), closeness use_wasserman_faust, degree
-- orientation + weighted degree, articleRank damping/tolerance, and
-- articulationPoints' resulting_component_* columns.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- A second numeric relationship property, distinct from `weight`, so the
-- relationship_weight_property override tests below exercise an actual
-- substitution (overriding with the *same* property name the projection
-- already uses is a degenerate no-op case handled separately).
SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person)-[r:KNOWS]->(b:Person)
    SET r.boost = r.weight * 10.0
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

-- NetworKit::ApproxBetweenness estimates the diameter as a first step, and
-- that estimator only supports undirected graphs ("connected components of
-- directed graphs cannot be computed" — a NetworKit limitation, not
-- something to work around with hand-written code). Use a separate
-- undirected projection for the sampling tests below.
SELECT age_gds.graph_project(
    name       => 'people_undirected',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => true
);

-- =========================================================================
-- pageRank_stream: scaler
-- =========================================================================

-- MinMax scaler must map the score range onto [0, 1].
SELECT
    abs(min(score) - 0.0) < 0.0001 AS min_is_zero,
    abs(max(score) - 1.0) < 0.0001 AS max_is_one
FROM age_gds.pageRank_stream(graph_name => 'people', scaler => 'MinMax');
-- expected: min_is_zero=t, max_is_one=t

-- An unknown scaler name is a SQL-level error, not a crash.
\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(graph_name => 'people', scaler => 'NotAScaler');
\set ON_ERROR_STOP on

-- =========================================================================
-- betweenness_stream: sampling (ApproxBetweenness substitution)
-- =========================================================================

SELECT count(*) FROM age_gds.betweenness_stream(
    graph_name    => 'people_undirected',
    sampling_size => 10,
    sampling_seed => 42
);
-- expected: 3 (matches the current node count of the shared test_graph
-- fixture, which prior test files in this suite mutate)

SELECT count(*) FROM age_gds.betweenness_stream(
    graph_name    => 'people_undirected',
    sampling_size => 10
) WHERE score < 0;
-- expected: 0

\set ON_ERROR_STOP off
SELECT * FROM age_gds.betweenness_stream(graph_name => 'people_undirected', sampling_size => -1);
\set ON_ERROR_STOP on

-- =========================================================================
-- closeness_stream: use_wasserman_faust
-- =========================================================================

SELECT count(*) FROM age_gds.closeness_stream(graph_name => 'people_undirected', use_wasserman_faust => true);
-- expected: 3

-- =========================================================================
-- degree_stream: orientation + weighted degree
-- =========================================================================

SELECT count(*) FROM age_gds.degree_stream(graph_name => 'people', orientation => 'REVERSE');
-- expected: 3

SELECT count(*) FROM age_gds.degree_stream(graph_name => 'people', orientation => 'UNDIRECTED');
-- expected: 3

-- NATURAL (out-degree) and REVERSE (in-degree) must differ on this directed
-- graph (Alice has out-degree 2 / in-degree 0).
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.degree_stream(graph_name => 'people', orientation => 'NATURAL')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.degree_stream(graph_name => 'people', orientation => 'REVERSE')
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'NATURAL and REVERSE orientation should differ on a directed graph';
    END IF;
END $$;

-- Weighted degree (relationship_weight_property set) must differ from the
-- unweighted case, since edge weights here are not uniform.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.degree_stream(graph_name => 'people')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.degree_stream(
            graph_name                   => 'people',
            relationship_weight_property => 'boost'
        )
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'weighted degree should differ from unweighted degree_stream';
    END IF;
END $$;

\set ON_ERROR_STOP off
SELECT * FROM age_gds.degree_stream(graph_name => 'people', orientation => 'BOGUS');
\set ON_ERROR_STOP on

-- Overriding with the exact property name the projection's own edge_query
-- already used for `weight` is a degenerate case for the regex-diff
-- substitution in reweight_from_property() (the substituted query is
-- byte-identical to the original, so it cannot tell "already this value"
-- apart from "no AS weight projection found"). It must surface as a clean
-- SQL error, not crash the backend (see src/util/weight_override.cpp).
\set ON_ERROR_STOP off
SELECT * FROM age_gds.pageRank_stream(graph_name => 'people', relationship_weight_property => 'weight');
\set ON_ERROR_STOP on

-- =========================================================================
-- eigenvector_stream: tolerance + scaler
-- =========================================================================
--
-- NetworKit::EigenvectorCentrality's power iteration has no damping term,
-- so on a directed *acyclic* graph the propagated vector's magnitude can
-- decay to (near) zero, tripping an internal NetworKit assertion
-- (`!Aux::NumericTools::equal(length, 1e-16)`) — a pre-existing limitation
-- of the vendored class on non-irreducible directed graphs, not something
-- to special-case in age_gds. Test against the undirected projection,
-- which is well-conditioned for this algorithm.

SELECT count(*) FROM age_gds.eigenvector_stream(graph_name => 'people_undirected', tolerance => 1e-5);
-- expected: 3

SELECT count(*) FROM age_gds.eigenvector_stream(graph_name => 'people_undirected', scaler => 'Max')
WHERE abs(score) > 1.0001;
-- expected: 0

-- =========================================================================
-- articleRank_stream: damping_factor / tolerance / scaler
-- =========================================================================

-- Changing damping_factor must change the result relative to the default.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.articleRank_stream(graph_name => 'people')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.articleRank_stream(graph_name => 'people', damping_factor => 0.5)
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'articleRank_stream damping_factor override had no observable effect';
    END IF;
END $$;

SELECT count(*) FROM age_gds.articleRank_stream(graph_name => 'people', scaler => 'StdScore');
-- expected: 3

-- =========================================================================
-- articulationPoints_stream: resulting_component_* columns
-- =========================================================================

SELECT create_graph('artest');

SELECT * FROM cypher('artest', $$
    CREATE (:N {name: 'a'}),
           (:N {name: 'b'}),
           (:N {name: 'c'}),
           (:N {name: 'd'})
$$) AS (result agtype);

-- Path a-b-c-d (undirected): b and c are both articulation points.
SELECT * FROM cypher('artest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('artest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('artest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'arproj',
    age_graph  => 'artest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => false
);

SELECT count(*) FROM age_gds.articulationPoints_stream(graph_name => 'arproj');
-- expected: 2 (b and c)

-- Every reported articulation point splits the path into exactly 2
-- resulting components of sizes 1 and 2/3, or 2/2 depending on which cut
-- vertex — in all cases resultingComponentCount = 2, min >= 1, max <= 3.
SELECT count(*) FROM age_gds.articulationPoints_stream(graph_name => 'arproj')
WHERE "resultingComponentCount" <> 2 OR "resultingComponentMin" < 1 OR "resultingComponentMax" > 3;
-- expected: 0

SELECT age_gds.graph_drop('arproj');
SELECT age_gds.graph_drop('people_undirected');
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
SELECT drop_graph('artest', true);
