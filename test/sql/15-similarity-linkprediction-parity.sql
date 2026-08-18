-- Similarity & link-prediction parity fixes
-- (docs/implemented-plans/04-similarity-linkprediction-parity.md)
--
-- Exercises: nodeSimilarity_stream's similarity_cutoff / degree_cutoff /
-- top_k / top_n / bottom_k / bottom_n / relationship_weight_property
-- (weighted Jaccard) / upper_degree_cutoff / source_node_filter /
-- target_node_filter / use_components / similarity_metric
-- (JACCARD|OVERLAP|COSINE), knn_stream's top_k default fix + similarity_cutoff,
-- and the four new linkprediction_* single-pair scalar functions
-- (relationship_query, direction gap).

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('simtest');

-- Two triangle-adjacent clusters {a,b,c} and {d,e,f}, joined by a single
-- bridge edge c-d, plus an isolated node g (degree 0, excluded by the
-- default degree_cutoff => 1), plus a *disconnected* third triangle
-- {h,i,j} (used to make use_components observable). w2 is lopsided vs w1's
-- uniform weight so weighted-Jaccard is observably different from
-- unweighted.
SELECT * FROM cypher('simtest', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'}),
           (:N {name: 'd'}), (:N {name: 'e'}), (:N {name: 'f'}),
           (:N {name: 'g'}),
           (:N {name: 'h'}), (:N {name: 'i'}), (:N {name: 'j'})
$$) AS (result agtype);

SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'h'}), (b:N {name:'i'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'i'}), (b:N {name:'j'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'h'}), (b:N {name:'j'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'c'}) CREATE (a)-[:REL {w1: 1.0, w2: 9.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'e'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'e'}), (b:N {name:'f'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('simtest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'f'}) CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'simtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w1 AS weight$$,
    directed   => false,
    weighted   => true
);

SELECT (id::text)::bigint AS a_id INTO TEMP a_tmp FROM cypher('simtest', $$
    MATCH (n:N {name:'a'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS g_id INTO TEMP g_tmp FROM cypher('simtest', $$
    MATCH (n:N {name:'g'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS d_id INTO TEMP d_tmp FROM cypher('simtest', $$
    MATCH (n:N {name:'d'}) RETURN id(n) AS id
$$) AS (id agtype);

-- =========================================================================
-- nodeSimilarity_stream: defaults (degree_cutoff => 1 excludes isolated g)
-- =========================================================================

SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people')
WHERE node1 = (SELECT g_id FROM g_tmp) OR node2 = (SELECT g_id FROM g_tmp);
-- expected: 0 (g is degree 0, excluded by default degree_cutoff => 1)

SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people')
WHERE similarity < 0 OR similarity > 1;
-- expected: 0

-- =========================================================================
-- nodeSimilarity_stream: top-K bound verification
-- =========================================================================

DO $$
DECLARE
    max_per_node int;
BEGIN
    SELECT max(cnt) INTO max_per_node FROM (
        SELECT node1, count(*) AS cnt
        FROM age_gds.nodeSimilarity_stream(graph_name => 'people', top_k => 1, bottom_k => 0)
        GROUP BY node1
    ) t;
    IF max_per_node > 1 THEN
        RAISE EXCEPTION 'nodeSimilarity_stream top_k => 1 bound violated: got % rows for a node', max_per_node;
    END IF;
END $$;

DO $$
DECLARE
    row_count int;
BEGIN
    SELECT count(*) INTO row_count
    FROM age_gds.nodeSimilarity_stream(graph_name => 'people', top_n => 2, bottom_k => 0);
    IF row_count > 2 THEN
        RAISE EXCEPTION 'nodeSimilarity_stream top_n => 2 global cap violated: got % rows', row_count;
    END IF;
END $$;

-- =========================================================================
-- nodeSimilarity_stream: similarity_cutoff
-- =========================================================================

DO $$
DECLARE
    row_count int;
BEGIN
    SELECT count(*) INTO row_count
    FROM age_gds.nodeSimilarity_stream(graph_name => 'people', similarity_cutoff => 2.0, bottom_k => 0);
    IF row_count <> 0 THEN
        RAISE EXCEPTION 'similarity_cutoff => 2.0 (above the maximum possible score) should exclude everything, got % rows', row_count;
    END IF;
END $$;

-- =========================================================================
-- nodeSimilarity_stream: relationship_weight_property (weighted Jaccard)
-- =========================================================================

DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT node1, node2, similarity FROM age_gds.nodeSimilarity_stream(graph_name => 'people', bottom_k => 0)
        EXCEPT
        SELECT node1, node2, similarity FROM age_gds.nodeSimilarity_stream(
            graph_name                    => 'people',
            relationship_weight_property  => 'w2',
            bottom_k                      => 0
        )
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'relationship_weight_property override had no observable effect on nodeSimilarity_stream';
    END IF;
END $$;

-- relationship_weight_property is only defined for JACCARD.
\set ON_ERROR_STOP off
SELECT * FROM age_gds.nodeSimilarity_stream(
    graph_name                    => 'people',
    relationship_weight_property  => 'w2',
    similarity_metric              => 'OVERLAP'
);
-- expected: ERROR
\set ON_ERROR_STOP on

-- =========================================================================
-- nodeSimilarity_stream: similarity_metric (OVERLAP / COSINE / bad value)
-- =========================================================================

SELECT count(*) FROM age_gds.nodeSimilarity_stream(
    graph_name => 'people', similarity_metric => 'OVERLAP', bottom_k => 0
) WHERE similarity < 0 OR similarity > 1;
-- expected: 0

SELECT count(*) FROM age_gds.nodeSimilarity_stream(
    graph_name => 'people', similarity_metric => 'COSINE', bottom_k => 0
) WHERE similarity < 0 OR similarity > 1;
-- expected: 0

\set ON_ERROR_STOP off
SELECT * FROM age_gds.nodeSimilarity_stream(graph_name => 'people', similarity_metric => 'NOT_A_METRIC');
-- expected: ERROR
\set ON_ERROR_STOP on

-- =========================================================================
-- nodeSimilarity_stream: degree_cutoff / upper_degree_cutoff
-- =========================================================================

-- degree_cutoff => 0 lets isolated node g back in as a candidate (though it
-- will never score > 0 against anything since it has no neighbors, so it
-- still never appears -- verifying the *filter*, not the score, requires
-- checking it's still absent even when the filter would otherwise admit it,
-- i.e. this documents that a degree-0 node contributes no similarity pairs
-- regardless of degree_cutoff).
SELECT count(*) FROM age_gds.nodeSimilarity_stream(graph_name => 'people', degree_cutoff => 0, bottom_k => 0)
WHERE node1 = (SELECT g_id FROM g_tmp) OR node2 = (SELECT g_id FROM g_tmp);
-- expected: 0

-- upper_degree_cutoff => 1 (b, c, d each have degree >= 2) excludes any
-- pair involving them.
DO $$
DECLARE
    bad_count int;
BEGIN
    SELECT count(*) INTO bad_count
    FROM age_gds.nodeSimilarity_stream(graph_name => 'people', upper_degree_cutoff => 1, bottom_k => 0)
    WHERE node1 = (SELECT d_id FROM d_tmp) OR node2 = (SELECT d_id FROM d_tmp);
    IF bad_count <> 0 THEN
        RAISE EXCEPTION 'upper_degree_cutoff => 1 should exclude node d (degree 3), got % rows', bad_count;
    END IF;
END $$;

-- =========================================================================
-- nodeSimilarity_stream: source_node_filter / target_node_filter
-- =========================================================================

DO $$
DECLARE
    bad_count int;
BEGIN
    SELECT count(*) INTO bad_count
    FROM age_gds.nodeSimilarity_stream(
        graph_name          => 'people',
        source_node_filter  => ARRAY[(SELECT a_id FROM a_tmp)],
        bottom_k            => 0
    )
    WHERE node1 <> (SELECT a_id FROM a_tmp);
    IF bad_count <> 0 THEN
        RAISE EXCEPTION 'source_node_filter restricted to node a produced % rows with a different node1', bad_count;
    END IF;
END $$;

-- =========================================================================
-- nodeSimilarity_stream: use_components
-- =========================================================================

-- {h,i,j} is a separate connected component, disjoint from {a..f}/{g}.
-- Cross-component pairs (e.g. a vs. h) share no neighbors, so they score
-- exactly 0 -- which the default similarity_cutoff (1e-42) already
-- excludes, making use_components unobservable at default settings.
-- similarity_cutoff => -1 lets zero-score pairs through so the
-- use_components filter is actually exercised: without it, cross-component
-- zero-score pairs are candidates (and can occupy a node's bottom-K slot);
-- with it, they're never even considered.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT node1, node2 FROM age_gds.nodeSimilarity_stream(
            graph_name => 'people', similarity_cutoff => -1, bottom_k => 0)
        EXCEPT
        SELECT node1, node2 FROM age_gds.nodeSimilarity_stream(
            graph_name => 'people', similarity_cutoff => -1,
            use_components => true, bottom_k => 0)
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'use_components => true did not exclude cross-component pairs';
    END IF;
END $$;

-- =========================================================================
-- nodeSimilarity_stream: default top_k + bottom_k bound
-- =========================================================================

-- Both default to 10, and (per docs/api.md) a default call can return up
-- to top_k + bottom_k = 20 rows per source node -- verify that bound, and
-- exercise the top/bottom dedup path (this fixture has few enough
-- candidates per node that dedup is actually needed).
DO $$
DECLARE
    max_per_node int;
BEGIN
    SELECT max(cnt) INTO max_per_node FROM (
        SELECT node1, count(*) AS cnt
        FROM age_gds.nodeSimilarity_stream(graph_name => 'people')
        GROUP BY node1
    ) t;
    IF max_per_node > 20 THEN
        RAISE EXCEPTION 'default top_k+bottom_k bound violated: got % rows for a node', max_per_node;
    END IF;
END $$;

-- =========================================================================
-- knn_stream: top_k default fix (5 -> 10) + similarity_cutoff
-- =========================================================================

DO $$
DECLARE
    max_per_node int;
BEGIN
    SELECT max(cnt) INTO max_per_node FROM (
        SELECT "sourceNodeId", count(*) AS cnt
        FROM age_gds.knn_stream(graph_name => 'people')
        GROUP BY "sourceNodeId"
    ) t;
    IF max_per_node > 10 THEN
        RAISE EXCEPTION 'knn_stream default top_k should be 10, got % rows for a node', max_per_node;
    END IF;
END $$;

DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "sourceNodeId", "targetNodeId" FROM age_gds.knn_stream(graph_name => 'people')
        EXCEPT
        SELECT "sourceNodeId", "targetNodeId" FROM age_gds.knn_stream(graph_name => 'people', similarity_cutoff => 2.0)
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'knn_stream similarity_cutoff => 2.0 (above max score) should exclude everything';
    END IF;
END $$;

-- =========================================================================
-- linkprediction_* single-pair scalar functions
-- =========================================================================

SELECT age_gds.linkprediction_adamicAdar(
    (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp), graph_name => 'people'
);
-- expected: not null, >= 0

SELECT age_gds.linkprediction_commonNeighbors(
    (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp), graph_name => 'people'
) AS common_neighbors;
-- expected: 1 (only c is a common neighbor of a and d)

SELECT age_gds.linkprediction_preferentialAttachment(
    (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp), graph_name => 'people'
);
-- expected: not null

SELECT age_gds.linkprediction_resourceAllocation(
    (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp), graph_name => 'people'
);
-- expected: not null, >= 0

-- direction other than 'BOTH' is a documented NetworKit gap, not silently
-- ignored.
\set ON_ERROR_STOP off
SELECT age_gds.linkprediction_adamicAdar(
    (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp), graph_name => 'people', direction => 'OUTGOING'
);
-- expected: ERROR
\set ON_ERROR_STOP on

-- node1/node2 are required.
\set ON_ERROR_STOP off
SELECT age_gds.linkprediction_adamicAdar(NULL, (SELECT d_id FROM d_tmp), graph_name => 'people');
-- expected: ERROR
\set ON_ERROR_STOP on

-- relationship_query re-projects with a restricted edge set: dropping the
-- c-d bridge (the only path between {a,b,c} and {d,e,f}) means a and d no
-- longer share a common neighbor.
DO $$
DECLARE
    restricted_common float8;
BEGIN
    SELECT age_gds.linkprediction_commonNeighbors(
        (SELECT a_id FROM a_tmp), (SELECT d_id FROM d_tmp),
        graph_name          => 'people',
        relationship_query  => $q$MATCH (a:N)-[r:REL]->(b:N)
                                  WHERE NOT (a.name = 'c' AND b.name = 'd')
                                    AND NOT (a.name = 'd' AND b.name = 'c')
                                  RETURN id(a) AS src, id(b) AS dst, r.w1 AS weight$q$
    ) INTO restricted_common;
    IF restricted_common <> 0 THEN
        RAISE EXCEPTION 'relationship_query dropping the c-d bridge should leave 0 common neighbors for a/d, got %', restricted_common;
    END IF;
END $$;

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
