-- Path-finding & flow algorithm parity fixes (docs/implemented-plans/03-pathfinding-parity.md)
--
-- Exercises: bfs_stream/dfs_stream max_depth + target_nodes,
-- shortestPath_dijkstra_stream's new single-pair path shape,
-- allShortestPaths_dijkstra_stream, bellmanFord_stream/_stats negative-cycle
-- detection, randomWalk_stream's source_nodes/walks_per_node/P-Q params,
-- spanningTree_stream's objective + source_node validation,
-- shortestPath_astar_stream, maxFlow_stats multi-source/sink +
-- capacity_property + node_capacity_property, and minCostMaxFlow_stats.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Dedicated fixture graph, independent of the shared test_graph fixture
-- (which later test files, e.g. 10-snapshot-version-cache.sql, mutate --
-- including deleting nodes -- across the whole suite run). Alice-[1.0]->Bob-
-- [2.0]->Carol-[0.5]->Dave, Alice-[1.5]->Carol.
SELECT create_graph('pftest');
SELECT * FROM cypher('pftest', $$
    CREATE (:Person {name: 'Alice'}), (:Person {name: 'Bob'}),
           (:Person {name: 'Carol'}), (:Person {name: 'Dave'})
$$) AS (result agtype);
SELECT * FROM cypher('pftest', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'})
    CREATE (a)-[:KNOWS {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('pftest', $$
    MATCH (a:Person {name:'Bob'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 2.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('pftest', $$
    MATCH (a:Person {name:'Carol'}), (b:Person {name:'Dave'})
    CREATE (a)-[:KNOWS {weight: 0.5}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('pftest', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 1.5}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'pftest',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

-- id(n) is a Cypher function, only callable inside cypher(); cast its
-- agtype text representation (a bare integer) to bigint for use as a plain
-- SQL parameter, same convention as node_query/edge_query's "AS id"/"AS src"
-- projections above.
SELECT (id::text)::bigint AS alice_id INTO TEMP alice_tmp FROM cypher('pftest', $$
    MATCH (n:Person {name:'Alice'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS bob_id INTO TEMP bob_tmp FROM cypher('pftest', $$
    MATCH (n:Person {name:'Bob'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS carol_id INTO TEMP carol_tmp FROM cypher('pftest', $$
    MATCH (n:Person {name:'Carol'}) RETURN id(n) AS id
$$) AS (id agtype);
SELECT (id::text)::bigint AS dave_id INTO TEMP dave_tmp FROM cypher('pftest', $$
    MATCH (n:Person {name:'Dave'}) RETURN id(n) AS id
$$) AS (id agtype);

-- =========================================================================
-- bfs_stream: max_depth + target_nodes
-- =========================================================================

-- Alice's neighborhood is bfs-depth 0 (self), 1 (Bob, Carol), 2 (Dave).
SELECT count(*) FROM age_gds.bfs_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp),
    max_depth   => 1
);
-- expected: 4 (one row per graph node -- Alice depth 0, Bob+Carol depth 1,
-- and Dave with a NULL distance since he's beyond max_depth)

SELECT count(*) FILTER (WHERE distance IS NOT NULL) FROM age_gds.bfs_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp),
    max_depth   => 1
);
-- expected: 3

SELECT "nodeId", distance FROM age_gds.bfs_stream(
    graph_name    => 'people',
    source_node   => (SELECT alice_id FROM alice_tmp),
    target_nodes  => ARRAY[(SELECT dave_id FROM dave_tmp)]
) ORDER BY "nodeId";
-- expected: 1 row, Dave at distance 2

-- =========================================================================
-- dfs_stream: max_depth + target_nodes
-- =========================================================================

SELECT count(*) FROM age_gds.dfs_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp),
    max_depth   => 1
);
-- expected: <= 3 (Alice + depth-1 neighbors only)

SELECT count(*) FROM age_gds.dfs_stream(
    graph_name    => 'people',
    source_node   => (SELECT alice_id FROM alice_tmp),
    target_nodes  => ARRAY[(SELECT dave_id FROM dave_tmp)]
);
-- expected: 1

-- =========================================================================
-- shortestPath_dijkstra_stream: single-pair path output (breaking change)
-- =========================================================================

SELECT total_cost, node_ids, costs FROM age_gds.shortestPath_dijkstra_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp),
    target_node => (SELECT dave_id FROM dave_tmp)
);
-- expected: total_cost=1.5+0.5=2.0 (Alice->Carol->Dave beats
-- Alice->Bob->Carol->Dave's 1.0+2.0+0.5=3.5), node_ids=[alice,carol,dave]

-- Unreachable target (reverse direction on a directed graph) -> a single
-- all-NULL row, not a crash / not zero rows.
SELECT total_cost IS NULL AS unreachable FROM age_gds.shortestPath_dijkstra_stream(
    graph_name  => 'people',
    source_node => (SELECT dave_id FROM dave_tmp),
    target_node => (SELECT alice_id FROM alice_tmp)
);
-- expected: t

-- =========================================================================
-- allShortestPaths_dijkstra_stream: single-source-to-all with paths
-- =========================================================================

SELECT count(*) FROM age_gds.allShortestPaths_dijkstra_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp)
);
-- expected: 4 (one row per node, including Alice itself at cost 0)

SELECT count(*) FROM age_gds.allShortestPaths_dijkstra_stream(
    graph_name    => 'people',
    source_node   => (SELECT alice_id FROM alice_tmp),
    target_nodes  => ARRAY[(SELECT dave_id FROM dave_tmp), (SELECT bob_id FROM bob_tmp)]
);
-- expected: 2

-- =========================================================================
-- bellmanFord_stream / bellmanFord_stats: negative-cycle detection
-- =========================================================================

SELECT create_graph('bftest');
SELECT * FROM cypher('bftest', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'})
$$) AS (result agtype);
SELECT * FROM cypher('bftest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('bftest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'})
    CREATE (a)-[:REL {weight: -3.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('bftest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'})
    CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'bfproj',
    age_graph  => 'bftest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS a_id INTO TEMP bf_a_tmp FROM cypher('bftest', $$MATCH (n:N {name:'a'}) RETURN id(n) AS id$$) AS (id agtype);

-- a->b->c->a sums to 1 - 3 + 1 = -1: a genuine negative cycle.
SELECT bool_and("isNegativeCycle") FROM age_gds.bellmanFord_stream(
    graph_name  => 'bfproj',
    source_node => (SELECT a_id FROM bf_a_tmp)
);
-- expected: t

SELECT "containsNegativeCycle" FROM age_gds.bellmanFord_stats(
    graph_name  => 'bfproj',
    source_node => (SELECT a_id FROM bf_a_tmp)
);
-- expected: t

-- The (non-negative-cycle) 'people' graph must report no negative cycle.
SELECT bool_or("isNegativeCycle") FROM age_gds.bellmanFord_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp)
);
-- expected: f

SELECT age_gds.graph_drop('bfproj');
SELECT drop_graph('bftest', true);

-- =========================================================================
-- randomWalk_stream: source_nodes / walks_per_node / P,Q / breaking change
-- =========================================================================

-- 3 walks from Alice only -> 3 distinct walk_id groups, every walk starts
-- at Alice.
SELECT count(DISTINCT walk_id) FROM age_gds.randomWalk_stream(
    graph_name     => 'people',
    source_nodes   => ARRAY[(SELECT alice_id FROM alice_tmp)],
    walk_length    => 5,
    walks_per_node => 3
);
-- expected: 3

DO $$
DECLARE
    bad_starts int;
BEGIN
    SELECT count(*) INTO bad_starts FROM (
        SELECT walk_id, min("nodeId") AS first_seen FROM (
            SELECT walk_id, "nodeId", row_number() OVER (PARTITION BY walk_id) AS rn
            FROM age_gds.randomWalk_stream(
                graph_name     => 'people',
                source_nodes   => ARRAY[(SELECT alice_id FROM alice_tmp)],
                walk_length    => 5,
                walks_per_node => 3
            )
        ) t WHERE rn = 1 GROUP BY walk_id
    ) s WHERE first_seen <> (SELECT alice_id FROM alice_tmp);
    IF bad_starts <> 0 THEN
        RAISE EXCEPTION 'randomWalk_stream: a walk did not start at the requested source_nodes entry';
    END IF;
END $$;

-- source_nodes => NULL means "every node" (Neo4j default): must include
-- walks starting at nodes other than Alice too.
SELECT count(*) > 0 FROM age_gds.randomWalk_stream(
    graph_name     => 'people',
    walk_length    => 3,
    walks_per_node => 2
) WHERE "nodeId" = (SELECT dave_id FROM dave_tmp);
-- expected: t (Dave is reachable as *some* walk's start or interior node)

-- =========================================================================
-- spanningTree_stream: objective + source_node validation
-- =========================================================================

SELECT count(*) FROM age_gds.spanningTree_stream(graph_name => 'people', objective => 'minimum');
-- expected: 3

SELECT count(*) FROM age_gds.spanningTree_stream(graph_name => 'people', objective => 'maximum');
-- expected: 3

-- objective must actually change which edges come back, not just be
-- accepted and ignored: on this fixture (edges 1.0, 2.0, 0.5, 1.5), the
-- minimum spanning tree is {0.5, 1.0, 1.5} = 3.0 and the maximum is
-- {2.0, 1.5, 0.5} = 4.0 -- genuinely different totals. This also guards
-- against the 'minimum' branch accidentally reporting the negated
-- edgeValues instead of pg.graph.weight(u,v) (the sum would go negative).
DO $$
DECLARE
    min_sum float8;
    max_sum float8;
BEGIN
    SELECT sum(cost) INTO min_sum FROM age_gds.spanningTree_stream(graph_name => 'people', objective => 'minimum');
    SELECT sum(cost) INTO max_sum FROM age_gds.spanningTree_stream(graph_name => 'people', objective => 'maximum');
    IF min_sum IS DISTINCT FROM 3.0 OR max_sum IS DISTINCT FROM 4.0 OR NOT (min_sum < max_sum) THEN
        RAISE EXCEPTION 'spanningTree_stream objective had no observable effect (min_sum=%, max_sum=%)', min_sum, max_sum;
    END IF;
END $$;

\set ON_ERROR_STOP off
SELECT * FROM age_gds.spanningTree_stream(graph_name => 'people', objective => 'bogus');
\set ON_ERROR_STOP on

SELECT count(*) FROM age_gds.spanningTree_stream(
    graph_name  => 'people',
    source_node => (SELECT alice_id FROM alice_tmp)
);
-- expected: 3 (validation-only; result unaffected)

\set ON_ERROR_STOP off
SELECT * FROM age_gds.spanningTree_stream(graph_name => 'people', source_node => -999999);
\set ON_ERROR_STOP on

-- =========================================================================
-- shortestPath_astar_stream
-- =========================================================================

SELECT create_graph('geotest');
SELECT * FROM cypher('geotest', $$
    CREATE (:City {name: 'x', lat: 0.0,  lon: 0.0}),
           (:City {name: 'y', lat: 0.0,  lon: 1.0}),
           (:City {name: 'z', lat: 0.0,  lon: 2.0})
$$) AS (result agtype);
SELECT * FROM cypher('geotest', $$
    MATCH (a:City {name:'x'}), (b:City {name:'y'})
    CREATE (a)-[:ROAD {weight: 111.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('geotest', $$
    MATCH (a:City {name:'y'}), (b:City {name:'z'})
    CREATE (a)-[:ROAD {weight: 111.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'geoproj',
    age_graph  => 'geotest',
    node_query => $$MATCH (n:City) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:City)-[r:ROAD]->(b:City) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS x_id INTO TEMP geo_x_tmp FROM cypher('geotest', $$MATCH (n:City {name:'x'}) RETURN id(n) AS id$$) AS (id agtype);
SELECT (id::text)::bigint AS z_id INTO TEMP geo_z_tmp FROM cypher('geotest', $$MATCH (n:City {name:'z'}) RETURN id(n) AS id$$) AS (id agtype);

SELECT total_cost, array_length(node_ids, 1) FROM age_gds.shortestPath_astar_stream(
    graph_name          => 'geoproj',
    source_node         => (SELECT x_id FROM geo_x_tmp),
    target_node         => (SELECT z_id FROM geo_z_tmp),
    latitude_property   => 'lat',
    longitude_property  => 'lon'
);
-- expected: total_cost=222.0, 3 nodes in path (x, y, z)

SELECT age_gds.graph_drop('geoproj');
SELECT drop_graph('geotest', true);

-- =========================================================================
-- maxFlow_stats: multi-source/sink, capacity_property, node_capacity_property
-- =========================================================================

SELECT create_graph('flowtest');
SELECT * FROM cypher('flowtest', $$
    CREATE (:N {name: 's1'}), (:N {name: 's2'}),
           (:N {name: 'mid'}),
           (:N {name: 't1'}), (:N {name: 't2'})
$$) AS (result agtype);
SELECT * FROM cypher('flowtest', $$
    MATCH (a:N {name:'s1'}), (b:N {name:'mid'})
    CREATE (a)-[:PIPE {cap: 5.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('flowtest', $$
    MATCH (a:N {name:'s2'}), (b:N {name:'mid'})
    CREATE (a)-[:PIPE {cap: 5.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('flowtest', $$
    MATCH (a:N {name:'mid'}), (b:N {name:'t1'})
    CREATE (a)-[:PIPE {cap: 6.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('flowtest', $$
    MATCH (a:N {name:'mid'}), (b:N {name:'t2'})
    CREATE (a)-[:PIPE {cap: 6.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'flowproj',
    age_graph  => 'flowtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:PIPE]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS s1_id INTO TEMP fs1_tmp FROM cypher('flowtest', $$MATCH (n:N {name:'s1'}) RETURN id(n) AS id$$) AS (id agtype);
SELECT (id::text)::bigint AS s2_id INTO TEMP fs2_tmp FROM cypher('flowtest', $$MATCH (n:N {name:'s2'}) RETURN id(n) AS id$$) AS (id agtype);
SELECT (id::text)::bigint AS t1_id INTO TEMP ft1_tmp FROM cypher('flowtest', $$MATCH (n:N {name:'t1'}) RETURN id(n) AS id$$) AS (id agtype);
SELECT (id::text)::bigint AS t2_id INTO TEMP ft2_tmp FROM cypher('flowtest', $$MATCH (n:N {name:'t2'}) RETURN id(n) AS id$$) AS (id agtype);
SELECT (id::text)::bigint AS mid_id INTO TEMP fmid_tmp FROM cypher('flowtest', $$MATCH (n:N {name:'mid'}) RETURN id(n) AS id$$) AS (id agtype);

-- capacity_property (cap) instead of the graph's own (uniform 1.0) weight:
-- max flow from s1 to t1 alone is capped at 5.0 (s1->mid) or 6.0
-- (mid->t1), i.e. 5.0.
SELECT flow FROM age_gds.maxFlow_stats(
    graph_name        => 'flowproj',
    source_node       => (SELECT s1_id FROM fs1_tmp),
    target_node       => (SELECT t1_id FROM ft1_tmp),
    capacity_property => 'cap'
);
-- expected: 5.0

-- Multi-source/multi-sink: s1+s2 (5+5=10 capacity in) vs t1+t2 (6+6=12
-- capacity out), bottlenecked by mid's total incident capacity -> 10.0.
SELECT flow FROM age_gds.maxFlow_stats(
    graph_name        => 'flowproj',
    capacity_property => 'cap',
    source_nodes      => ARRAY[(SELECT s1_id FROM fs1_tmp), (SELECT s2_id FROM fs2_tmp)],
    target_nodes      => ARRAY[(SELECT t1_id FROM ft1_tmp), (SELECT t2_id FROM ft2_tmp)]
);
-- expected: 10.0

-- node_capacity_property: capping 'mid' itself at 3.0 must bring the
-- multi-source/sink flow down to 3.0 regardless of edge capacities.
SELECT * FROM cypher('flowtest', $$
    MATCH (n:N {name:'mid'}) SET n.node_cap = 3.0
$$) AS (result agtype);

SELECT flow FROM age_gds.maxFlow_stats(
    graph_name             => 'flowproj',
    capacity_property      => 'cap',
    source_nodes           => ARRAY[(SELECT s1_id FROM fs1_tmp), (SELECT s2_id FROM fs2_tmp)],
    target_nodes           => ARRAY[(SELECT t1_id FROM ft1_tmp), (SELECT t2_id FROM ft2_tmp)],
    node_capacity_property => 'node_cap'
);
-- expected: 3.0

-- =========================================================================
-- minCostMaxFlow_stats
-- =========================================================================

SELECT * FROM cypher('flowtest', $$
    MATCH (a:N)-[r:PIPE]->(b:N) SET r.cost = 1.0
$$) AS (result agtype);

-- s1->mid->t1 has capacity min(5,6)=5 and 2 hops at cost 1.0 each, so the
-- max flow (5 units) at minimum cost is 5*1.0 + 5*1.0 = 10.0 exactly --
-- pinning the exact value (not just >= 0) catches cost/capacity swapped.
SELECT abs(total_cost - 10.0) < 1e-6 FROM age_gds.minCostMaxFlow_stats(
    graph_name         => 'flowproj',
    capacity_property  => 'cap',
    cost_property      => 'cost',
    source_nodes       => ARRAY[(SELECT s1_id FROM fs1_tmp)],
    target_nodes       => ARRAY[(SELECT t1_id FROM ft1_tmp)]
);
-- expected: t (5 units * 2 hops * cost 1.0 = 10.0)

\set ON_ERROR_STOP off
SELECT * FROM age_gds.minCostMaxFlow_stats(graph_name => 'flowproj');
\set ON_ERROR_STOP on

SELECT age_gds.graph_drop('flowproj');
SELECT drop_graph('flowtest', true);

-- =========================================================================
-- Cleanup
-- =========================================================================

SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds CASCADE;
SELECT drop_graph('pftest', true);
