-- test/sql/23-diff-reindex-slot-map.sql
-- Plan: faster-diff-reindex, item 1. diff-and-apply graph_reindex addresses
-- nodes by slot through a graphid -> slot map. After a node is tombstoned,
-- NetworKit ids are compacted past it (nk != slot), so the map must come from
-- the slot file, not from NodeIdMap. Every check RAISEs on mismatch.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('slot_graph');

SELECT * FROM cypher('slot_graph', $$
    CREATE (:N {i:1}), (:N {i:2}), (:N {i:3}), (:N {i:4}), (:N {i:5}), (:N {i:6})
$$) AS (r agtype);

DO $do$
BEGIN
    FOR k IN 1..5 LOOP
        EXECUTE format($q$
            SELECT * FROM cypher('slot_graph', $$
                MATCH (a:N {i:%s}), (b:N {i:%s}) CREATE (a)-[:E {weight: %s.0}]->(b)
            $$) AS (r agtype) $q$, k, k + 1, k);
    END LOOP;
END
$do$;

-- Signature of a projection: counts, BFS reach, component count, weighted path.
CREATE FUNCTION pg_temp.sig(p text, src bigint, dst bigint) RETURNS text
LANGUAGE sql AS $$
    SELECT (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = p)
        || ' bfs=' || (SELECT count(*) FROM age_gds.bfs_stream(graph_name => p, source_node => src))
        || ' wcc=' || (SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(graph_name => p))
        || ' cost=' || (SELECT total_cost FROM age_gds.shortestPath_dijkstra_stream(
                            graph_name => p, source_node => src, target_node => dst))
$$;

CREATE FUNCTION pg_temp.vid(i int) RETURNS bigint LANGUAGE plpgsql AS $fn$
DECLARE r bigint;
BEGIN
    EXECUTE format($q$
        SELECT (v::text)::bigint FROM cypher('slot_graph', $$
            MATCH (n:N {i:%s}) RETURN id(n) $$) AS (v agtype) $q$, i) INTO r;
    RETURN r;
END $fn$;

CREATE FUNCTION pg_temp.check_equiv(label text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
    PERFORM age_gds.graph_drop('slot_fresh') WHERE age_gds.graph_exists('slot_fresh');
    PERFORM age_gds.graph_project('slot_fresh', 'slot_graph',
        'MATCH (n) RETURN id(n) AS id',
        'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
        false, true, false);
    a := pg_temp.sig('slot_inc',   pg_temp.vid(1), pg_temp.vid(6));
    b := pg_temp.sig('slot_fresh', pg_temp.vid(1), pg_temp.vid(6));
    IF a IS DISTINCT FROM b THEN
        RAISE EXCEPTION '% : incremental [%] <> fresh [%]', label, a, b;
    END IF;
    RAISE NOTICE '% ok: %', label, a;
END $$;

-- 1. incremental, non-live projection
SELECT age_gds.graph_project('slot_inc', 'slot_graph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    directed => false, weighted => true, incremental => true);
SELECT pg_temp.check_equiv('initial');

-- 2. delete a middle vertex (not the last slot) and its edges: tombstones
DO $$
DECLARE v graphid;
BEGIN
    SELECT id INTO v FROM slot_graph."N" WHERE properties::text = '{"i": 3}';
    DELETE FROM slot_graph."E" WHERE start_id = v OR end_id = v;
    DELETE FROM slot_graph."N" WHERE id = v;
END $$;
SELECT age_gds.graph_reindex('slot_inc');
-- 6 -> 5 vertices; the chain is now cut, so BFS/dijkstra from 1 to 6 differ,
-- compare against a fresh projection using endpoints that still exist.
DO $$
DECLARE a text; b text;
BEGIN
    PERFORM age_gds.graph_project('slot_fresh2', 'slot_graph',
        'MATCH (n) RETURN id(n) AS id',
        'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
        false, true, false);
    IF (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'slot_inc')
       IS DISTINCT FROM
       (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'slot_fresh2') THEN
        RAISE EXCEPTION 'after tombstone: counts differ';
    END IF;
    PERFORM age_gds.graph_drop('slot_fresh2');
END $$;

-- 3. new vertices and edges whose endpoints lie before AND after the tombstone;
--    this reindex deserializes a file with tombstones (nk != slot).
SELECT * FROM cypher('slot_graph', $$ CREATE (:N {i:7}), (:N {i:8}) $$) AS (r agtype);
SELECT * FROM cypher('slot_graph', $$
    MATCH (a:N {i:2}), (b:N {i:7}) CREATE (a)-[:E {weight: 1.0}]->(b) $$) AS (r agtype);
SELECT * FROM cypher('slot_graph', $$
    MATCH (a:N {i:7}), (b:N {i:5}) CREATE (a)-[:E {weight: 1.0}]->(b) $$) AS (r agtype);
SELECT * FROM cypher('slot_graph', $$
    MATCH (a:N {i:8}), (b:N {i:1}) CREATE (a)-[:E {weight: 1.0}]->(b) $$) AS (r agtype);
SELECT * FROM cypher('slot_graph', $$
    MATCH (a:N {i:6}), (b:N {i:8}) CREATE (a)-[:E {weight: 1.0}]->(b) $$) AS (r agtype);
SELECT age_gds.graph_reindex('slot_inc');
SELECT pg_temp.check_equiv('after add with tombstones');

-- 4. edge weight change on an existing edge propagates
SELECT * FROM cypher('slot_graph', $$
    MATCH (a:N {i:7})-[e:E]->(b:N {i:5}) SET e.weight = 9.0 $$) AS (r agtype);
SELECT age_gds.graph_reindex('slot_inc');
SELECT pg_temp.check_equiv('after weight change');

-- 5. a second pass over the (now more tombstoned) files stays consistent
SELECT age_gds.graph_reindex('slot_inc');
SELECT pg_temp.check_equiv('idempotent reindex');

SELECT age_gds.graph_drop('slot_inc');
SELECT age_gds.graph_drop('slot_fresh');
SELECT drop_graph('slot_graph', true);
