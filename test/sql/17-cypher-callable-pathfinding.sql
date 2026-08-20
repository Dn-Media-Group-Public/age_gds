-- Cypher-callable pathfinding overloads
-- (docs/implemented-plans/cypher-callable-interface.md, phase 2).
--
-- Exercises every age_gds.<name>(graphName agtype, config agtype)
-- overload added to src/algorithms/pathfinding.cpp, called as a scalar
-- expression inside cypher() and consumed via UNWIND (AGE's Cypher has no
-- CALL...YIELD). Each test/sql/*.sql file is a fresh psql session, so
-- fixtures are self-contained here.
--
-- psql does not interpolate :variables inside dollar-quoted strings, so
-- AGE graphids (only known at runtime) can't be spliced into a cypher()
-- body the way plain SQL literals can. Instead, build the query text with
-- format(%s, ...) -- a normal string, no dollar-quote protection -- and
-- run it with \gexec.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- =========================================================================
-- Fixture: Alice-[1.0]->Bob-[2.0]->Carol-[0.5]->Dave, Alice-[1.5]->Carol
-- (same shape as 14-pathfinding-parity.sql's 'people' graph).
-- =========================================================================

SELECT create_graph('cctest');
SELECT * FROM cypher('cctest', $$
    CREATE (:Person {name: 'Alice'}), (:Person {name: 'Bob'}),
           (:Person {name: 'Carol'}), (:Person {name: 'Dave'})
$$) AS (result agtype);
SELECT * FROM cypher('cctest', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'})
    CREATE (a)-[:KNOWS {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cctest', $$
    MATCH (a:Person {name:'Bob'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 2.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cctest', $$
    MATCH (a:Person {name:'Carol'}), (b:Person {name:'Dave'})
    CREATE (a)-[:KNOWS {weight: 0.5}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cctest', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 1.5}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'ccpeople',
    age_graph  => 'cctest',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS alice_id FROM cypher('cctest', $$MATCH (n:Person {name:'Alice'}) RETURN id(n) AS id$$) AS (id agtype) \gset
SELECT (id::text)::bigint AS bob_id   FROM cypher('cctest', $$MATCH (n:Person {name:'Bob'})   RETURN id(n) AS id$$) AS (id agtype) \gset
SELECT (id::text)::bigint AS dave_id  FROM cypher('cctest', $$MATCH (n:Person {name:'Dave'})  RETURN id(n) AS id$$) AS (id agtype) \gset

-- bfs_stream: no path field, one row {sourceNode, nodeIds}.
SELECT format($q$
SELECT * FROM cypher('cctest', $c$
    UNWIND age_gds.bfs_stream('ccpeople', {sourceNode: %s}) AS row
    RETURN row.sourceNode, row.nodeIds
$c$) AS (sourceNode agtype, nodeIds agtype)
$q$, :alice_id) \gexec
-- expected: 1 row, sourceNode=alice, nodeIds contains all 4 people

-- bfs_stream targetNodes: early termination, nodeIds is the reduced
-- visited list (not per-target distances -- that stays SQL-only).
SELECT format($q$
SELECT jsonb_array_length((nodeIds::text)::jsonb) AS n_visited FROM cypher('cctest', $c$
    UNWIND age_gds.bfs_stream('ccpeople', {sourceNode: %s, targetNodes: [%s]}) AS row
    RETURN row.nodeIds
$c$) AS (nodeIds agtype)
$q$, :alice_id, :dave_id) \gexec
-- expected: 4 (Alice, Bob, Carol, Dave all visited before Dave -- the last
-- node in BFS order -- triggers early stop)

-- dfs_stream: array of {nodeId, depth} maps.
SELECT format($q$
SELECT count(*) FROM cypher('cctest', $c$
    UNWIND age_gds.dfs_stream('ccpeople', {sourceNode: %s}) AS row
    RETURN row.nodeId, row.depth
$c$) AS (nodeId agtype, depth agtype)
$q$, :alice_id) \gexec
-- expected: 4

-- shortestPath_dijkstra_stream: one-element array, {totalCost, nodeIds, costs}.
SELECT format($q$
SELECT "totalCost" FROM cypher('cctest', $c$
    UNWIND age_gds.shortestpath_dijkstra_stream('ccpeople',
        {sourceNode: %s, targetNode: %s}) AS row
    RETURN row.totalCost
$c$) AS ("totalCost" agtype)
$q$, :alice_id, :dave_id) \gexec
-- expected: 2.0 (Alice->Carol->Dave: 1.5+0.5, beats 1.0+2.0+0.5=3.5)

-- allShortestPaths_dijkstra_stream: array of {targetNodeId, totalCost, ...}.
SELECT format($q$
SELECT count(*) FROM cypher('cctest', $c$
    UNWIND age_gds.allshortestpaths_dijkstra_stream('ccpeople',
        {sourceNode: %s}) AS row
    RETURN row.targetNodeId, row.totalCost
$c$) AS ("targetNodeId" agtype, "totalCost" agtype)
$q$, :alice_id) \gexec
-- expected: 4

-- deltaStepping_stream (Dijkstra fallback): array of {nodeId, distance}.
SELECT format($q$
SELECT count(*) FROM cypher('cctest', $c$
    UNWIND age_gds.deltastepping_stream('ccpeople', {sourceNode: %s}) AS row
    RETURN row.nodeId, row.distance
$c$) AS ("nodeId" agtype, distance agtype)
$q$, :alice_id) \gexec
-- expected: 4

-- bellmanFord_stream / bellmanFord_stats: no negative cycle on this graph.
SELECT format($q$
SELECT bool_or(("isNegativeCycle"::text)::boolean) FROM cypher('cctest', $c$
    UNWIND age_gds.bellmanford_stream('ccpeople', {sourceNode: %s}) AS row
    RETURN row.isNegativeCycle
$c$) AS ("isNegativeCycle" agtype)
$q$, :alice_id) \gexec
-- expected: f

-- *_stats/*_write return a single map (not an array) -- accessed directly
-- via dot notation, no UNWIND.
SELECT format($q$
SELECT ("containsNegativeCycle"::text)::boolean FROM cypher('cctest', $c$
    RETURN age_gds.bellmanford_stats('ccpeople', {sourceNode: %s}).containsNegativeCycle
$c$) AS ("containsNegativeCycle" agtype)
$q$, :alice_id) \gexec
-- expected: f

-- randomWalk_stream: array of {walkId, nodeId}, every walk starts at Alice.
SELECT format($q$
SELECT count(DISTINCT ("walkId"::text)) FROM cypher('cctest', $c$
    UNWIND age_gds.randomwalk_stream('ccpeople',
        {sourceNodes: [%s], walkLength: 5, walksPerNode: 3}) AS row
    RETURN row.walkId, row.nodeId
$c$) AS ("walkId" agtype, "nodeId" agtype)
$q$, :alice_id) \gexec
-- expected: 3

-- spanningTree_stream: array of {src, dst, cost}.
SELECT count(*) FROM cypher('cctest', $$
    UNWIND age_gds.spanningtree_stream('ccpeople', {}) AS row
    RETURN row.src, row.dst, row.cost
$$) AS (src agtype, dst agtype, cost agtype);
-- expected: 3

-- allShortestPaths_stream: array of {sourceNodeId, targetNodeId, distance}.
SELECT count(*) FROM cypher('cctest', $$
    UNWIND age_gds.allshortestpaths_stream('ccpeople', {}) AS row
    RETURN row.sourceNodeId, row.targetNodeId, row.distance
$$) AS ("sourceNodeId" agtype, "targetNodeId" agtype, distance agtype);
-- expected: 16 (4x4, includes unreachable pairs as null distance)

-- maxFlow_stats: single map {flow, computeMillis}.
SELECT format($q$
SELECT (flow::text)::float8 > 0 FROM cypher('cctest', $c$
    RETURN age_gds.maxflow_stats('ccpeople', {sourceNode: %s, targetNode: %s}).flow
$c$) AS (flow agtype)
$q$, :alice_id, :dave_id) \gexec
-- expected: t

SELECT age_gds.graph_drop('ccpeople');
SELECT drop_graph('cctest', true);

-- =========================================================================
-- DAG fixture: topologicalSort_stream, longestPath_stream
-- =========================================================================

SELECT create_graph('ccdag');
SELECT * FROM cypher('ccdag', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'})
$$) AS (result agtype);
SELECT * FROM cypher('ccdag', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'}) CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccdag', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'}) CREATE (a)-[:REL {weight: 2.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'ccdagproj',
    age_graph  => 'ccdag',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT count(*) FROM cypher('ccdag', $$
    UNWIND age_gds.topologicalsort_stream('ccdagproj', {}) AS row
    RETURN row.nodeId, row.sortIndex, row.maxDistance
$$) AS ("nodeId" agtype, "sortIndex" agtype, "maxDistance" agtype);
-- expected: 3

SELECT max((distance::text)::float8) FROM cypher('ccdag', $$
    UNWIND age_gds.longestpath_stream('ccdagproj', {}) AS row
    RETURN row.nodeId, row.distance
$$) AS ("nodeId" agtype, distance agtype);
-- expected: 3.0 (a->b->c: 1.0+2.0)

SELECT age_gds.graph_drop('ccdagproj');
SELECT drop_graph('ccdag', true);

-- =========================================================================
-- Geo fixture: shortestPath_astar_stream
-- =========================================================================

SELECT create_graph('ccgeo');
SELECT * FROM cypher('ccgeo', $$
    CREATE (:City {name: 'x', lat: 0.0, lon: 0.0}),
           (:City {name: 'y', lat: 0.0, lon: 1.0}),
           (:City {name: 'z', lat: 0.0, lon: 2.0})
$$) AS (result agtype);
SELECT * FROM cypher('ccgeo', $$
    MATCH (a:City {name:'x'}), (b:City {name:'y'}) CREATE (a)-[:ROAD {weight: 111.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccgeo', $$
    MATCH (a:City {name:'y'}), (b:City {name:'z'}) CREATE (a)-[:ROAD {weight: 111.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'ccgeoproj',
    age_graph  => 'ccgeo',
    node_query => $$MATCH (n:City) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:City)-[r:ROAD]->(b:City) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS x_id FROM cypher('ccgeo', $$MATCH (n:City {name:'x'}) RETURN id(n) AS id$$) AS (id agtype) \gset
SELECT (id::text)::bigint AS z_id FROM cypher('ccgeo', $$MATCH (n:City {name:'z'}) RETURN id(n) AS id$$) AS (id agtype) \gset

SELECT format($q$
SELECT ("totalCost"::text)::float8 FROM cypher('ccgeo', $c$
    UNWIND age_gds.shortestpath_astar_stream('ccgeoproj',
        {sourceNode: %s, targetNode: %s,
         latitudeProperty: 'lat', longitudeProperty: 'lon'}) AS row
    RETURN row.totalCost
$c$) AS ("totalCost" agtype)
$q$, :x_id, :z_id) \gexec
-- expected: 222.0

SELECT age_gds.graph_drop('ccgeoproj');
SELECT drop_graph('ccgeo', true);

-- =========================================================================
-- Flow fixture: minCostMaxFlow_stats
-- =========================================================================

SELECT create_graph('ccflow');
SELECT * FROM cypher('ccflow', $$
    CREATE (:N {name: 's'}), (:N {name: 'mid'}), (:N {name: 't'})
$$) AS (result agtype);
SELECT * FROM cypher('ccflow', $$
    MATCH (a:N {name:'s'}), (b:N {name:'mid'}) CREATE (a)-[:PIPE {cap: 5.0, cost: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccflow', $$
    MATCH (a:N {name:'mid'}), (b:N {name:'t'}) CREATE (a)-[:PIPE {cap: 6.0, cost: 1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'ccflowproj',
    age_graph  => 'ccflow',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:PIPE]->(b:N) RETURN id(a) AS src, id(b) AS dst, 1.0 AS weight$$,
    directed   => true,
    weighted   => true
);

SELECT (id::text)::bigint AS s_id FROM cypher('ccflow', $$MATCH (n:N {name:'s'}) RETURN id(n) AS id$$) AS (id agtype) \gset
SELECT (id::text)::bigint AS t_id FROM cypher('ccflow', $$MATCH (n:N {name:'t'}) RETURN id(n) AS id$$) AS (id agtype) \gset

SELECT format($q$
SELECT abs(("totalCost"::text)::float8 - 10.0) < 1e-6 FROM cypher('ccflow', $c$
    RETURN age_gds.mincostmaxflow_stats('ccflowproj',
        {capacityProperty: 'cap', costProperty: 'cost',
         sourceNodes: [%s], targetNodes: [%s]}).totalCost
$c$) AS ("totalCost" agtype)
$q$, :s_id, :t_id) \gexec
-- expected: t (5 units * 2 hops * cost 1.0 = 10.0)

SELECT age_gds.graph_drop('ccflowproj');
SELECT drop_graph('ccflow', true);

DROP EXTENSION age_gds CASCADE;
