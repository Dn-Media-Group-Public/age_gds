-- test/sql/10-snapshot-version-cache.sql
-- Tests for plan 10: cross-backend cache invalidation for flat (snapshot)
-- projections, and optional incremental storage + diff-based reindex for
-- graph_project.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- -------------------------------------------------------------------------
-- 1. Flat projection: cross-backend cache invalidation via content_version
-- -------------------------------------------------------------------------
SELECT age_gds.graph_project(
    name       => 'flat1',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => true
);

SELECT name, incremental, live FROM age_gds.graph_list() WHERE name = 'flat1';
-- expected: flat1 | f | f

-- Warm this backend's session cache
SELECT age_gds.cache_load('flat1');
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'flat1');
-- expected: 4

-- Add a new Person via Cypher directly to the underlying AGE graph
SELECT * FROM cypher('test_graph', $$
    CREATE (:Person {name: 'Frank', age: 40})
$$) AS (result agtype);

-- Reindex from a SEPARATE backend (shells out to a second psql connection).
-- This backend's session cache is untouched by that reindex's
-- session_cache_invalidate() call — the only way it can notice is the
-- content_version check on next use.
\! psql -X -q -d testdb -c "LOAD 'age'; SET search_path = ag_catalog, \"\$user\", public; SELECT age_gds.graph_reindex('flat1');"

-- Same session, no explicit cache_load/invalidate: content_version mismatch
-- must be detected and the file reloaded automatically.
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'flat1');
-- expected: 5

-- flat projections have no incremental mode: force := false must error
\set ON_ERROR_STOP off
SELECT age_gds.graph_reindex('flat1', force := false);
\set ON_ERROR_STOP on

-- force := true is accepted (full rebuild, same as default)
SELECT age_gds.graph_reindex('flat1', force := true);
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'flat1');
-- expected: 5

SELECT age_gds.graph_drop('flat1');

-- -------------------------------------------------------------------------
-- 2. graph_project(incremental := true): diff-based reindex
-- -------------------------------------------------------------------------
SELECT age_gds.graph_project(
    name        => 'inc1',
    age_graph   => 'test_graph',
    node_query  => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query  => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                    RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed    => false,
    weighted    => true,
    incremental => true
);

SELECT name, incremental, live FROM age_gds.graph_list() WHERE name = 'inc1';
-- expected: inc1 | t | f

-- 5 persons at this point (Alice, Bob, Carol, Dave, Frank)
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'inc1');
-- expected: 5

-- Mutate underlying AGE data: add a node, remove a node, change an edge weight
SELECT * FROM cypher('test_graph', $$
    CREATE (:Person {name: 'George', age: 50})
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (n:Person {name: 'Dave'}) DETACH DELETE n
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person {name:'Alice'})-[r:KNOWS]->(b:Person {name:'Bob'})
    SET r.weight = 5.0
$$) AS (result agtype);

-- Default reindex: diff-and-apply (no `force`)
SELECT age_gds.graph_reindex('inc1');

SELECT node_count, edge_count FROM age_gds.graph_list() WHERE name = 'inc1';
-- expected: 5 | 3   (George added, Dave removed, Alice-Bob weight updated in place)

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'inc1');
-- expected: 5

-- force := true: full rebuild into fresh slot files must match the diff result
SELECT age_gds.graph_reindex('inc1', force := true);

SELECT node_count, edge_count FROM age_gds.graph_list() WHERE name = 'inc1';
-- expected: 5 | 3

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'inc1');
-- expected: 5

-- Two consecutive force := true rebuilds with an intervening mutation: each
-- rewrite resets version_counter to a fixed constant internally
-- (index_storage_write_initial), so this specifically exercises that the
-- cache still detects the second rewrite (version must be forced strictly
-- past what was last observed, not just left at the reset constant).
SELECT * FROM cypher('test_graph', $$
    CREATE (:Person {name: 'Helen', age: 45})
$$) AS (result agtype);

SELECT age_gds.graph_reindex('inc1', force := true);
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'inc1');
-- expected: 6 (Helen picked up by the second force rebuild)

SELECT age_gds.graph_drop('inc1');

-- -------------------------------------------------------------------------
-- Cleanup
-- -------------------------------------------------------------------------
SELECT * FROM cypher('test_graph', $$
    MATCH (n:Person) WHERE n.name IN ['Frank', 'George', 'Helen'] DETACH DELETE n
$$) AS (result agtype);

DROP EXTENSION age_gds CASCADE;
