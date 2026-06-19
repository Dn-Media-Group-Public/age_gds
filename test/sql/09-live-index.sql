-- test/sql/09-live-index.sql
-- Tests for live graph index (plan 09).
--
-- NOTE: AGE 1.7.0 uses internal heap insertion for Cypher DML and therefore
-- does NOT fire PostgreSQL row-level triggers.  Sections 3-6 use direct SQL
-- INSERT/DELETE to exercise the trigger path.  Cypher mutations require a
-- manual graph_reindex to bring the live index up to date.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- -------------------------------------------------------------------------
-- 1. Basic creation and graph_list
-- -------------------------------------------------------------------------
SELECT age_gds.graph_index(
    name      => 'live_people',
    age_graph => 'test_graph'
);

-- Must appear in graph_list with live = true
SELECT name, node_count, edge_count, live
FROM age_gds.graph_list()
WHERE name = 'live_people';
-- expected: live_people | 4 | 4 | t

-- graph_exists works
SELECT age_gds.graph_exists('live_people');
-- expected: t

-- graph_is_stale always false for live
SELECT age_gds.graph_is_stale('live_people');
-- expected: f

-- -------------------------------------------------------------------------
-- 2. Algorithm call on live index
-- -------------------------------------------------------------------------
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 4  (one row per Person)

-- -------------------------------------------------------------------------
-- 3. Vertex INSERT — index captures via SQL trigger
--    (AGE 1.7.0 Cypher DML bypasses PG triggers; direct SQL INSERT fires them)
-- -------------------------------------------------------------------------
DO $$
DECLARE
    person_label_id int2;
    new_id graphid;
BEGIN
    SELECT al.id INTO person_label_id
    FROM ag_catalog.ag_label al
    JOIN ag_catalog.ag_graph ag ON al.graph = ag.graphid
    WHERE ag.name = 'test_graph' AND al.name = 'Person';

    new_id := ag_catalog._graphid(person_label_id,
                  nextval('test_graph."Person_id_seq"')::int8);
    INSERT INTO test_graph."Person" VALUES (new_id, '{"name":"Eve","age":22}');
END
$$;

-- Load fresh from index (version_counter has changed)
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 5

-- -------------------------------------------------------------------------
-- 4. Vertex DELETE — index removes via SQL trigger
-- -------------------------------------------------------------------------
DO $$
DECLARE
    max_id_int int8;
BEGIN
    SELECT max((id::text)::int8) INTO max_id_int FROM test_graph."Person";
    DELETE FROM test_graph."Person" WHERE (id::text)::int8 = max_id_int;
END
$$;

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 4

-- -------------------------------------------------------------------------
-- 5. Edge INSERT via SQL trigger
-- -------------------------------------------------------------------------
DO $$
DECLARE
    knows_label_id int2;
    alice_id graphid;
    dave_id  graphid;
    new_edge_id graphid;
BEGIN
    SELECT al.id INTO knows_label_id
    FROM ag_catalog.ag_label al
    JOIN ag_catalog.ag_graph ag ON al.graph = ag.graphid
    WHERE ag.name = 'test_graph' AND al.name = 'KNOWS';

    -- get Alice and Dave IDs (lowest and highest graphid among original persons)
    SELECT id INTO alice_id FROM test_graph."Person" ORDER BY (id::text)::int8 ASC  LIMIT 1;
    SELECT id INTO dave_id  FROM test_graph."Person" ORDER BY (id::text)::int8 DESC LIMIT 1;

    new_edge_id := ag_catalog._graphid(knows_label_id,
                       nextval('test_graph."KNOWS_id_seq"')::int8);
    INSERT INTO test_graph."KNOWS" VALUES (new_edge_id, alice_id, dave_id, '{"weight":0.7}');
END
$$;

-- Degree should reflect new edge; count still 4 persons
SELECT count(*) FROM age_gds.degree_stream(graph_name => 'live_people');
-- expected: 4

-- -------------------------------------------------------------------------
-- 6. graph_reindex (compact)
-- -------------------------------------------------------------------------
SELECT age_gds.graph_reindex('live_people');

SELECT name, node_count, edge_count, live
FROM age_gds.graph_list()
WHERE name = 'live_people';
-- catalog count unchanged; live still true

SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 4 (compaction preserves live nodes)

-- -------------------------------------------------------------------------
-- 7. New label added AFTER graph_index — AGE trigger bypass limitation
--    AGE 1.7.0 uses simple_table_tuple_insert for ALL internal mutations
--    (vertices AND ag_label rows), bypassing PostgreSQL row-level triggers.
--    Consequence: Cypher CREATE mutations are never captured by the live
--    index.  graph_reindex for a live index only compacts existing binary
--    files; it does NOT reload from AGE.  To incorporate Cypher-inserted
--    vertices into a new label, drop and recreate the index.
-- -------------------------------------------------------------------------

-- Create Animal label+vertex via Cypher (Dog bypasses all PG triggers)
SELECT * FROM cypher('test_graph', $$
    CREATE (:Animal {name: 'Dog'})
$$) AS (result agtype);

-- Live index does not capture Dog: count still 4
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 4

-- graph_reindex for a live index compacts files only — Dog still absent
SELECT age_gds.graph_reindex('live_people');
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 4

-- Drop and recreate: Animal label now exists, trigger installed, Dog loaded
SELECT age_gds.graph_drop('live_people');
SELECT age_gds.graph_index(name => 'live_people', age_graph => 'test_graph');
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'live_people');
-- expected: 5 (4 persons + Dog)

-- -------------------------------------------------------------------------
-- 8. graph_drop cleans up triggers and files
-- -------------------------------------------------------------------------
SELECT age_gds.graph_drop('live_people');

SELECT age_gds.graph_exists('live_people');
-- expected: f

-- index_triggers rows must be gone (cascade delete)
SELECT count(*) FROM age_gds.index_triggers WHERE graph_name = 'live_people';
-- expected: 0

-- -------------------------------------------------------------------------
-- 9. Error: graph_index basic call (no node_query / edge_query is fine)
-- -------------------------------------------------------------------------
\set ON_ERROR_STOP off
SELECT age_gds.graph_index(
    name       => 'bad',
    age_graph  => 'test_graph'
);
SELECT age_gds.graph_drop('bad');
\set ON_ERROR_STOP on

-- -------------------------------------------------------------------------
-- Cleanup
-- -------------------------------------------------------------------------
-- Remove animals we created
SELECT * FROM cypher('test_graph', $$
    MATCH (n:Animal) DELETE n
$$) AS (result agtype);

-- Remove extra KNOWS edge
DO $$
DECLARE
    max_edge_int int8;
BEGIN
    SELECT max((id::text)::int8) INTO max_edge_int FROM test_graph."KNOWS";
    DELETE FROM test_graph."KNOWS" WHERE (id::text)::int8 = max_edge_int;
END
$$;

DROP EXTENSION age_gds CASCADE;
