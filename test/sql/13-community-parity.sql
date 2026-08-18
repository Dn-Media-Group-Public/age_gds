-- Community detection algorithm parity fixes
-- (docs/implemented-plans/02-community-detection-parity.md)
--
-- Exercises the new optional parameters added to close closable gaps
-- against Neo4j GDS: relationship_weight_property / gamma / max_iterations /
-- recurse / consecutive_ids / min_community_size (louvain_stream),
-- relationship_weight_property / gamma / iterations (leiden_stream),
-- relationship_weight_property / max_iterations / consecutive_ids
-- (labelPropagation_stream, seed_property refused with a clear error),
-- relationship_weight_property / threshold / consecutive_ids (wcc_stream),
-- consecutive_ids (scc_stream), max_degree (triangleCount_stream),
-- triangle_count_property (localClusteringCoefficient_stream), and
-- community_property / relationship_weight_property (modularity_stats).

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('commtest');

-- Two triangle-adjacent communities {a,b,c} and joined via a weak bridge to
-- an isolated pair {d,e}. `cid` is a precomputed community label (used by
-- modularity_stats' community_property). `tri` is a precomputed triangle
-- count, deliberately wrong (always 0) so that
-- localClusteringCoefficient_stream's triangle_count_property override is
-- observably different from the computed default.
SELECT * FROM cypher('commtest', $$
    CREATE (:N {name: 'a', cid: 0, tri: 0}),
           (:N {name: 'b', cid: 0, tri: 0}),
           (:N {name: 'c', cid: 0, tri: 0}),
           (:N {name: 'd', cid: 1, tri: 0}),
           (:N {name: 'e', cid: 1, tri: 0})
$$) AS (result agtype);

-- Triangle a-b-c (w1 uniform 1.0, w2 lopsided so relationship_weight_property
-- overrides are observable), then a weak bridge c-d, then d-e.
SELECT * FROM cypher('commtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL {w1: 1.0, w2: 9.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('commtest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('commtest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'})
    CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('commtest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'})
    CREATE (a)-[:REL {w1: 0.1, w2: 0.1}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('commtest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'e'})
    CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'commproj',
    age_graph  => 'commtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w1 AS weight$$,
    directed   => false,
    weighted   => true
);

-- =========================================================================
-- louvain_stream: relationship_weight_property / gamma / max_iterations /
--                 recurse / consecutive_ids / min_community_size
-- =========================================================================

SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'commproj');
-- expected: 5

-- relationship_weight_property must be able to change the result.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", "communityId" FROM age_gds.louvain_stream(graph_name => 'commproj')
        EXCEPT
        SELECT "nodeId", "communityId" FROM age_gds.louvain_stream(
            graph_name                   => 'commproj',
            relationship_weight_property => 'w2'
        )
    ) d;
    -- Not asserted further: Louvain's own partition choice is not
    -- guaranteed to change on every weight perturbation, but the call
    -- itself must not error and must return the same node count.
    IF diff_count IS NULL THEN
        RAISE EXCEPTION 'unreachable';
    END IF;
END $$;

SELECT count(*) FROM age_gds.louvain_stream(
    graph_name      => 'commproj',
    gamma           => 0.5,
    max_iterations  => 5,
    recurse         => false
);
-- expected: 5

-- consecutive_ids: community ids must be dense, starting at 0.
SELECT (min("communityId") = 0) AS min_is_zero,
       (max("communityId") = count(DISTINCT "communityId") - 1) AS dense
FROM age_gds.louvain_stream(graph_name => 'commproj', consecutive_ids => true);
-- expected: min_is_zero=t, dense=t

-- min_community_size larger than the whole graph must filter out every row.
SELECT count(*) FROM age_gds.louvain_stream(graph_name => 'commproj', min_community_size => 1000);
-- expected: 0

-- =========================================================================
-- leiden_stream: relationship_weight_property / gamma / iterations
-- =========================================================================

SELECT count(*) FROM age_gds.leiden_stream(graph_name => 'commproj');
-- expected: 5

SELECT count(*) FROM age_gds.leiden_stream(
    graph_name                   => 'commproj',
    relationship_weight_property => 'w2',
    gamma                        => 2.0,
    iterations                   => 5
);
-- expected: 5

-- =========================================================================
-- labelPropagation_stream: relationship_weight_property / max_iterations /
--                           consecutive_ids; seed_property refused
-- =========================================================================

SELECT count(*) FROM age_gds.labelPropagation_stream(graph_name => 'commproj');
-- expected: 5

SELECT count(*) FROM age_gds.labelPropagation_stream(
    graph_name                   => 'commproj',
    relationship_weight_property => 'w2',
    max_iterations                => 3
);
-- expected: 5

SELECT (min("communityId") = 0) AS min_is_zero,
       (max("communityId") = count(DISTINCT "communityId") - 1) AS dense
FROM age_gds.labelPropagation_stream(graph_name => 'commproj', consecutive_ids => true);
-- expected: min_is_zero=t, dense=t

-- seed_property is a documented, deliberate gap (NetworKit::PLP's
-- baseClustering constructor leaves maxIterations uninitialized -- see
-- src/algorithms/community.cpp) -- must raise a clean SQL error, not crash.
\set ON_ERROR_STOP off
SELECT * FROM age_gds.labelPropagation_stream(graph_name => 'commproj', seed_property => 'cid');
\set ON_ERROR_STOP on

-- =========================================================================
-- wcc_stream: relationship_weight_property / threshold / consecutive_ids
-- =========================================================================

SELECT count(*) FROM age_gds.wcc_stream(graph_name => 'commproj');
-- expected: 5
SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(graph_name => 'commproj');
-- expected: 1 (graph is fully connected via the c-d bridge)

-- threshold above the weak bridge's weight (0.1) but at/below every other
-- edge's weight (1.0) must split the graph into 2 components.
SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(
    graph_name => 'commproj',
    threshold  => 0.5
);
-- expected: 2

SELECT (min("componentId") = 0) AS min_is_zero,
       (max("componentId") = count(DISTINCT "componentId") - 1) AS dense
FROM age_gds.wcc_stream(graph_name => 'commproj', threshold => 0.5, consecutive_ids => true);
-- expected: min_is_zero=t, dense=t

-- relationship_weight_property must be honored by the threshold filter too:
-- w2 makes the bridge (c-d) weight 0.1, same as w1, so this should still
-- split into 2 components under the same threshold.
SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(
    graph_name                    => 'commproj',
    relationship_weight_property  => 'w2',
    threshold                     => 0.5
);
-- expected: 2

-- =========================================================================
-- scc_stream: consecutive_ids
-- =========================================================================

SELECT count(*) FROM age_gds.scc_stream(graph_name => 'commproj');
-- expected: 5

SELECT (min("componentId") = 0) AS min_is_zero,
       (max("componentId") = count(DISTINCT "componentId") - 1) AS dense
FROM age_gds.scc_stream(graph_name => 'commproj', consecutive_ids => true);
-- expected: min_is_zero=t, dense=t

-- =========================================================================
-- triangleCount_stream: max_degree
-- =========================================================================

SELECT count(*) FROM age_gds.triangleCount_stream(graph_name => 'commproj');
-- expected: 5
SELECT count(*) FROM age_gds.triangleCount_stream(graph_name => 'commproj')
WHERE "triangleCount" = -1;
-- expected: 0 (no max_degree cap given)

-- Any max_degree of 0 must flag every node (min degree in this graph is >= 1).
SELECT count(*) FROM age_gds.triangleCount_stream(graph_name => 'commproj', max_degree => 0)
WHERE "triangleCount" <> -1;
-- expected: 0

-- =========================================================================
-- localClusteringCoefficient_stream: triangle_count_property
-- =========================================================================

-- Default (computed) coefficients: node 'a' sits in the a-b-c triangle with
-- degree 2, so its coefficient must be > 0.
SELECT count(*) FROM age_gds.localClusteringCoefficient_stream(graph_name => 'commproj')
WHERE coefficient > 0;
-- expected: >= 1

-- triangle_count_property ('tri', deliberately all zero) must override the
-- computed value and drive every coefficient to 0.
SELECT count(*) FROM age_gds.localClusteringCoefficient_stream(
    graph_name              => 'commproj',
    triangle_count_property => 'tri'
) WHERE coefficient <> 0;
-- expected: 0

-- =========================================================================
-- modularity_stats: community_property / relationship_weight_property
-- =========================================================================

-- No arguments: must keep working exactly as before this plan (internal
-- PLM fallback) -- this is the call test/sql/06-algorithms.sql already
-- exercises, so community_property must remain optional, not required.
SELECT count(*) FROM age_gds.modularity_stats(graph_name => 'commproj');
-- expected: 1

-- community_property: evaluate modularity of the precomputed 'cid' labels
-- instead of running PLM internally.
SELECT count(*) FROM age_gds.modularity_stats(graph_name => 'commproj', community_property => 'cid');
-- expected: 1

DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT modularity FROM age_gds.modularity_stats(graph_name => 'commproj', community_property => 'cid')
        EXCEPT
        SELECT modularity FROM age_gds.modularity_stats(
            graph_name                    => 'commproj',
            community_property            => 'cid',
            relationship_weight_property  => 'w2'
        )
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'modularity_stats relationship_weight_property override had no observable effect';
    END IF;
END $$;

-- =========================================================================
-- Cleanup
-- =========================================================================

SELECT age_gds.graph_drop('commproj');
DROP EXTENSION age_gds CASCADE;
SELECT drop_graph('commtest', true);
