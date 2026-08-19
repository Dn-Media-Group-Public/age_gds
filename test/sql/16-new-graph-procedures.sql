-- New graph-theoretic procedures with no prior age_gds equivalent
-- (docs/implemented-plans/06-new-graph-procedures.md)
--
-- Exercises topologicalSort_stream, longestPath_stream, bridges_stream,
-- conductance_stats, and cliqueCounting_stats.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- =========================================================================
-- topologicalSort_stream / longestPath_stream: diamond DAG
--   a -> b (weight 1)
--   a -> c (weight 5)
--   b -> d (weight 1)
--   c -> d (weight 1)
-- =========================================================================

SELECT create_graph('dagtest');

SELECT * FROM cypher('dagtest', $$
    CREATE (:N {name: 'a'}),
           (:N {name: 'b'}),
           (:N {name: 'c'}),
           (:N {name: 'd'})
$$) AS (result agtype);

SELECT * FROM cypher('dagtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL {w: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('dagtest', $$
    MATCH (a:N {name:'a'}), (c:N {name:'c'})
    CREATE (a)-[:REL {w: 5.0}]->(c)
$$) AS (result agtype);
SELECT * FROM cypher('dagtest', $$
    MATCH (b:N {name:'b'}), (d:N {name:'d'})
    CREATE (b)-[:REL {w: 1.0}]->(d)
$$) AS (result agtype);
SELECT * FROM cypher('dagtest', $$
    MATCH (c:N {name:'c'}), (d:N {name:'d'})
    CREATE (c)-[:REL {w: 1.0}]->(d)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'dagproj',
    age_graph  => 'dagtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS a_id INTO TEMP dag_a_tmp FROM cypher('dagtest', $$
    MATCH (n:N {name:'a'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS d_id INTO TEMP dag_d_tmp FROM cypher('dagtest', $$
    MATCH (n:N {name:'d'}) RETURN id(n) AS id
$$) AS (id agtype);

-- topologicalSort_stream (default): 4 rows, sort_index dense 0..3, and
-- a source ('a') must come before its successors.
SELECT count(*) FROM age_gds.topologicalSort_stream(graph_name => 'dagproj');
-- expected: 4

SELECT (min("sortIndex") = 0) AS min_is_zero,
       (max("sortIndex") = 3) AS max_is_three
FROM age_gds.topologicalSort_stream(graph_name => 'dagproj');
-- expected: min_is_zero=t, max_is_three=t

-- Without compute_max_distance_from_source, maxDistance must be NULL.
SELECT count(*) FROM age_gds.topologicalSort_stream(graph_name => 'dagproj')
WHERE "maxDistance" IS NOT NULL;
-- expected: 0

-- With it: a (no predecessors) has maxDistance 0; d (2 hops from a via
-- either b or c) has maxDistance 2.
SELECT "maxDistance" FROM age_gds.topologicalSort_stream(
    graph_name                       => 'dagproj',
    compute_max_distance_from_source => true
) WHERE "nodeId" = (SELECT a_id FROM dag_a_tmp);
-- expected: 0

SELECT "maxDistance" FROM age_gds.topologicalSort_stream(
    graph_name                       => 'dagproj',
    compute_max_distance_from_source => true
) WHERE "nodeId" = (SELECT d_id FROM dag_d_tmp);
-- expected: 2

-- longestPath_stream: distance at 'a' is 0; distance at 'd' is
-- max(dist_b + 1, dist_c + 1) = max(1+1, 5+1) = 6 (the c-branch dominates).
SELECT distance FROM age_gds.longestPath_stream(graph_name => 'dagproj')
WHERE "nodeId" = (SELECT a_id FROM dag_a_tmp);
-- expected: 0

SELECT distance FROM age_gds.longestPath_stream(graph_name => 'dagproj')
WHERE "nodeId" = (SELECT d_id FROM dag_d_tmp);
-- expected: 6

-- A cycle must be refused with a clean SQL error, not a crash or a
-- silently wrong/partial order.
SELECT * FROM cypher('dagtest', $$
    MATCH (d:N {name:'d'}), (a:N {name:'a'})
    CREATE (d)-[:REL {w: 1.0}]->(a)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'cyclicproj',
    age_graph  => 'dagtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w AS weight$$,
    directed   => true,
    weighted   => true
);

\set ON_ERROR_STOP off
SELECT * FROM age_gds.topologicalSort_stream(graph_name => 'cyclicproj');
SELECT * FROM age_gds.longestPath_stream(graph_name => 'cyclicproj');
\set ON_ERROR_STOP on

-- An undirected graph must also be refused (TopologicalSort is only
-- defined for directed graphs).
SELECT age_gds.graph_project(
    name       => 'undirproj',
    age_graph  => 'dagtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w AS weight$$,
    directed   => false,
    weighted   => true
);

\set ON_ERROR_STOP off
SELECT * FROM age_gds.topologicalSort_stream(graph_name => 'undirproj');
\set ON_ERROR_STOP on

SELECT age_gds.graph_drop('dagproj');
SELECT age_gds.graph_drop('cyclicproj');
SELECT age_gds.graph_drop('undirproj');
SELECT drop_graph('dagtest', true);

-- =========================================================================
-- bridges_stream: path graph a-b-c-d (undirected) -- every edge is a
-- bridge (reuses the same fixture shape as articulationPoints_stream's
-- test in test/sql/12-centrality-parity.sql).
-- =========================================================================

SELECT create_graph('bridgetest');

SELECT * FROM cypher('bridgetest', $$
    CREATE (:N {name: 'a'}),
           (:N {name: 'b'}),
           (:N {name: 'c'}),
           (:N {name: 'd'})
$$) AS (result agtype);

SELECT * FROM cypher('bridgetest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('bridgetest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('bridgetest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'bridgeproj',
    age_graph  => 'bridgetest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => false
);

SELECT count(*) FROM age_gds.bridges_stream(graph_name => 'bridgeproj');
-- expected: 3 (every edge in a path graph is a bridge)

-- Adding an edge that closes a-b-c into a triangle removes those two edges
-- (a-b, b-c) from the bridge set, leaving only c-d.
SELECT * FROM cypher('bridgetest', $$
    MATCH (a:N {name:'a'}), (c:N {name:'c'})
    CREATE (a)-[:REL]->(c)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'bridgeproj2',
    age_graph  => 'bridgetest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => false
);

SELECT count(*) FROM age_gds.bridges_stream(graph_name => 'bridgeproj2');
-- expected: 1 (only c-d remains a bridge)

SELECT age_gds.graph_drop('bridgeproj');
SELECT age_gds.graph_drop('bridgeproj2');
SELECT drop_graph('bridgetest', true);

-- =========================================================================
-- conductance_stats: community_property restricted to {0,1}
-- =========================================================================

SELECT create_graph('condtest');

-- Triangle {a,b,c} (community 0) joined via a single bridge edge to pair
-- {d,e} (community 1).
SELECT * FROM cypher('condtest', $$
    CREATE (:N {name: 'a', part: 0}),
           (:N {name: 'b', part: 0}),
           (:N {name: 'c', part: 0}),
           (:N {name: 'd', part: 1}),
           (:N {name: 'e', part: 1})
$$) AS (result agtype);

SELECT * FROM cypher('condtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('condtest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('condtest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('condtest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('condtest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'e'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'condproj',
    age_graph  => 'condtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => false
);

-- Single bridge edge (c-d) cut between a 3-node community (degree sum 6)
-- and a 2-node community (degree sum 3, the smaller volume) -> conductance
-- = 1 / 3.
SELECT abs(conductance - (1.0/3.0)) < 0.0001 AS matches_expected
FROM age_gds.conductance_stats(graph_name => 'condproj', community_property => 'part');
-- expected: t

-- community_property is required.
\set ON_ERROR_STOP off
SELECT * FROM age_gds.conductance_stats(graph_name => 'condproj');
\set ON_ERROR_STOP on

-- Values outside {0,1} must be refused, not silently coerced.
SELECT * FROM cypher('condtest', $$
    MATCH (n:N {name:'a'}) SET n.part3 = 2
$$) AS (result agtype);
SELECT * FROM cypher('condtest', $$
    MATCH (n:N) WHERE n.name <> 'a' SET n.part3 = 0
$$) AS (result agtype);

\set ON_ERROR_STOP off
SELECT * FROM age_gds.conductance_stats(graph_name => 'condproj', community_property => 'part3');
\set ON_ERROR_STOP on

-- A property that is missing on some nodes must also be refused rather
-- than silently defaulting.
\set ON_ERROR_STOP off
SELECT * FROM age_gds.conductance_stats(graph_name => 'condproj', community_property => 'tri');
\set ON_ERROR_STOP on

-- Regression: a gapped node-id space (graph.upperNodeIdBound() >
-- graph.numberOfNodes(), from a deleted node on an incremental projection
-- whose slot is tombstoned rather than compacted) must not corrupt
-- Partition::upperBound(). NetworKit::Partition(vector) sets omega from
-- max_element(data), which is 0 (not 1) if any slot is still
-- NetworKit::none -- Conductance::getQuality() then indexes fixed-size
-- 2-element arrays by that (wrong) upperBound(), a heap OOB write for
-- ids other than literal 0/1 if not explicitly pinned back to 2.
SELECT * FROM cypher('condtest', $$
    CREATE (:N {name: 'gap', part: 0})
$$) AS (result agtype);

SELECT age_gds.graph_drop('condproj');
SELECT age_gds.graph_project(
    name        => 'condproj_inc',
    age_graph   => 'condtest',
    node_query  => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query  => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed    => false,
    incremental => true
);

SELECT * FROM cypher('condtest', $$
    MATCH (n:N {name: 'gap'}) DETACH DELETE n
$$) AS (result agtype);

SELECT age_gds.graph_reindex('condproj_inc');

SELECT abs(conductance - (1.0/3.0)) < 0.0001 AS matches_expected
FROM age_gds.conductance_stats(graph_name => 'condproj_inc', community_property => 'part');
-- expected: t (must not crash, and must still be exact despite the gap)

SELECT age_gds.graph_drop('condproj_inc');
SELECT drop_graph('condtest', true);

-- =========================================================================
-- cliqueCounting_stats: bucketed maximal-clique counts by size
-- =========================================================================

SELECT create_graph('cliquetest');

-- Triangle {a,b,c} (one maximal 3-clique) plus a disjoint edge d-e (one
-- maximal 2-clique).
SELECT * FROM cypher('cliquetest', $$
    CREATE (:N {name: 'a'}),
           (:N {name: 'b'}),
           (:N {name: 'c'}),
           (:N {name: 'd'}),
           (:N {name: 'e'})
$$) AS (result agtype);

SELECT * FROM cypher('cliquetest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cliquetest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cliquetest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cliquetest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'e'})
    CREATE (a)-[:REL]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'cliqueproj',
    age_graph  => 'cliquetest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => false
);

SELECT clique_size, count FROM age_gds.cliqueCounting_stats(graph_name => 'cliqueproj')
WHERE clique_size = 3;
-- expected: one row, count = 1 (the a-b-c triangle)

SELECT clique_size, count FROM age_gds.cliqueCounting_stats(graph_name => 'cliqueproj')
WHERE clique_size = 2;
-- expected: one row, count = 1 (the d-e edge)

SELECT sum(count) FROM age_gds.cliqueCounting_stats(graph_name => 'cliqueproj');
-- expected: 2 (total maximal clique count)

SELECT age_gds.graph_drop('cliqueproj');
SELECT drop_graph('cliquetest', true);

DROP EXTENSION age_gds CASCADE;
