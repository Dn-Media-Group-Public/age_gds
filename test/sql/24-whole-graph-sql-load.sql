-- test/sql/24-whole-graph-sql-load.sql
-- Plan: faster-diff-reindex, item 3. A projection with both queries omitted is
-- the whole AGE graph, loaded with SQL from the label parents. It must equal
-- the explicit whole-graph Cypher projection. Every check RAISEs on mismatch.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('wg_graph');

-- A -2.5-> B -(no weight)-> C -('abc')-> D ; each edge also has a `cost`.
SELECT * FROM cypher('wg_graph', $$
    CREATE (a:P {i:1}), (b:P {i:2}), (c:Q {i:3, grp:'x'}), (d:Q {i:4, grp:'y'}),
           (a)-[:R {weight: 2.5, cost: 10.0}]->(b),
           (b)-[:R {cost: 20.0}]->(c),
           (c)-[:S {weight: 'abc', cost: 30.0}]->(d)
$$) AS (r agtype);

CREATE FUNCTION pg_temp.vid(i int) RETURNS bigint LANGUAGE plpgsql AS $fn$
DECLARE r bigint;
BEGIN
    EXECUTE format($q$
        SELECT (v::text)::bigint FROM cypher('wg_graph', $$
            MATCH (n {i:%s}) RETURN id(n) $$) AS (v agtype) $q$, i) INTO r;
    RETURN r;
END $fn$;

CREATE FUNCTION pg_temp.sig(p text, src bigint, dst bigint) RETURNS text
LANGUAGE sql AS $$
    SELECT (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = p)
        || ' bfs=' || (SELECT count(*) FROM age_gds.bfs_stream(graph_name => p, source_node => src))
        || ' wcc=' || (SELECT count(DISTINCT "componentId") FROM age_gds.wcc_stream(graph_name => p))
        || ' cost=' || coalesce((SELECT total_cost FROM age_gds.shortestPath_dijkstra_stream(
                            graph_name => p, source_node => src, target_node => dst))::text, 'none')
$$;

CREATE FUNCTION pg_temp.same(label text, a text, b text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    IF a IS DISTINCT FROM b THEN RAISE EXCEPTION '% : [%] <> [%]', label, a, b; END IF;
    RAISE NOTICE '% ok: %', label, a;
END $$;

-- 1+2. weighted = false / true: omitted queries == explicit whole-graph Cypher
DO $$
DECLARE w boolean; p1 text; p2 text;
BEGIN
    FOREACH w IN ARRAY ARRAY[false, true] LOOP
        PERFORM age_gds.graph_project('wg_cy', 'wg_graph',
            'MATCH (n) RETURN id(n) AS id',
            'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
            false, w, false);
        PERFORM age_gds.graph_project(name => 'wg_sql', age_graph => 'wg_graph',
            weighted => w);
        p1 := pg_temp.sig('wg_cy',  pg_temp.vid(1), pg_temp.vid(4));
        p2 := pg_temp.sig('wg_sql', pg_temp.vid(1), pg_temp.vid(4));
        PERFORM pg_temp.same('weighted=' || w, p1, p2);
        IF w AND p2 NOT LIKE '%cost=4.5' THEN
            RAISE EXCEPTION 'weighted cost should be 2.5+1+1 (missing/non-numeric => 1.0): %', p2;
        END IF;
        -- catalog stores NULL queries for the omitted form
        IF (SELECT node_query IS NOT NULL OR edge_query IS NOT NULL
              FROM age_gds.graphs WHERE name = 'wg_sql') THEN
            RAISE EXCEPTION 'omitted-query projection must store NULL queries';
        END IF;
        PERFORM age_gds.graph_drop('wg_cy');
        PERFORM age_gds.graph_drop('wg_sql');
    END LOOP;
END $$;

-- 3. per-call weight override and node-property loader on an omitted-query
--    projection match the Cypher-defined one
SELECT age_gds.graph_project('wg_cy', 'wg_graph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    false, false, false);
SELECT age_gds.graph_project('wg_sql', 'wg_graph');
DO $$
DECLARE a text; b text;
BEGIN
    SELECT total_cost::text INTO a FROM age_gds.shortestPath_dijkstra_stream(
        graph_name => 'wg_cy',  source_node => pg_temp.vid(1), target_node => pg_temp.vid(4),
        relationship_weight_property => 'cost');
    SELECT total_cost::text INTO b FROM age_gds.shortestPath_dijkstra_stream(
        graph_name => 'wg_sql', source_node => pg_temp.vid(1), target_node => pg_temp.vid(4),
        relationship_weight_property => 'cost');
    PERFORM pg_temp.same('weight override', a, b);
    IF b::float8 <> 60 THEN RAISE EXCEPTION 'cost override should be 10+20+30, got %', b; END IF;

    SELECT modularity::text INTO a FROM age_gds.modularity_stats(
        graph_name => 'wg_cy',  community_property => 'grp');
    SELECT modularity::text INTO b FROM age_gds.modularity_stats(
        graph_name => 'wg_sql', community_property => 'grp');
    PERFORM pg_temp.same('node property loader', a, b);
END $$;

-- 4. new vertex label + edge label after projection are picked up by reindex
SELECT age_gds.graph_drop('wg_sql');
SELECT age_gds.graph_project('wg_sql', 'wg_graph', incremental => true);
SELECT * FROM cypher('wg_graph', $$
    MATCH (d:Q {i:4}) CREATE (d)-[:NEWREL {weight: 1.0}]->(:NEWLABEL {i:5})
$$) AS (r agtype);
SELECT age_gds.graph_reindex('wg_sql');
SELECT age_gds.graph_drop('wg_cy');
SELECT age_gds.graph_project('wg_cy', 'wg_graph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    false, false, false);
SELECT pg_temp.same('new labels',
    pg_temp.sig('wg_cy',  pg_temp.vid(1), pg_temp.vid(5)),
    pg_temp.sig('wg_sql', pg_temp.vid(1), pg_temp.vid(5)));

-- 5. dangling edge (endpoint vertex row deleted with direct SQL) skipped alike
DO $$
DECLARE v graphid;
BEGIN
    SELECT id INTO v FROM wg_graph."NEWLABEL" LIMIT 1;
    DELETE FROM wg_graph."NEWLABEL" WHERE id = v;
END $$;
SELECT age_gds.graph_drop('wg_cy');
SELECT age_gds.graph_project('wg_cy', 'wg_graph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    false, false, false);
SELECT age_gds.graph_reindex('wg_sql');
SELECT pg_temp.same('dangling edge',
    (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'wg_cy'),
    (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'wg_sql'));

-- 6. exactly one query given is an error
DO $$
BEGIN
    BEGIN
        PERFORM age_gds.graph_project('wg_bad', 'wg_graph', 'MATCH (n) RETURN id(n) AS id', NULL);
        RAISE EXCEPTION 'expected an error when only node_query is given';
    EXCEPTION WHEN invalid_parameter_value THEN
        RAISE NOTICE 'one query only: error ok (%)', SQLERRM;
    END;
END $$;

-- 7. live index: whole-graph SQL load, unweighted and weighted, and force reload
SELECT age_gds.graph_drop('wg_cy');
SELECT age_gds.graph_project('wg_cy', 'wg_graph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    false, true, false);
SELECT age_gds.graph_index('wg_live_u', 'wg_graph');
SELECT age_gds.graph_index('wg_live_w', 'wg_graph', false, true);
SELECT age_gds.graph_reindex('wg_live_w', true);
SELECT pg_temp.same('live weighted + force reload',
    pg_temp.sig('wg_cy',     pg_temp.vid(1), pg_temp.vid(4)),
    pg_temp.sig('wg_live_w', pg_temp.vid(1), pg_temp.vid(4)));
SELECT pg_temp.same('live unweighted count',
    (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'wg_live_u'),
    (SELECT node_count || '/' || edge_count FROM age_gds.graph_list() WHERE name = 'wg_cy'));

SELECT age_gds.graph_drop('wg_cy');
SELECT age_gds.graph_drop('wg_sql');
SELECT age_gds.graph_drop('wg_live_u');
SELECT age_gds.graph_drop('wg_live_w');
SELECT drop_graph('wg_graph', true);
