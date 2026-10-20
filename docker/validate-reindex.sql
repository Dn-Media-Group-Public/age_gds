-- docker/validate-reindex.sql -- driven by docker/validate-reindex.sh.
-- Random, duplicate-free (no parallel edges, no self-loops) AGE graph; mutate it
-- with direct SQL; after each step compare the index content (counts, per-node
-- degree, per-node weighted degree) of an incremental projection and a live
-- index against ground truth computed in plain SQL from the AGE label tables.
-- Works on the baseline build and the new build (explicit Cypher queries only).
\set ON_ERROR_STOP on
\set QUIET on
CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;
SELECT setseed(0.42);
SELECT create_graph('valgraph');
SELECT create_vlabel('valgraph', 'VA'::cstring), create_vlabel('valgraph', 'VB'::cstring),
       create_elabel('valgraph', 'EA'::cstring), create_elabel('valgraph', 'EB'::cstring);

CREATE TABLE vids (n serial PRIMARY KEY, id graphid);
CREATE TABLE results (label text, proj text, ok boolean, detail text);

CREATE PROCEDURE refresh_ids() LANGUAGE plpgsql AS $$
BEGIN
    TRUNCATE vids RESTART IDENTITY;
    INSERT INTO vids (id) SELECT id FROM valgraph._ag_label_vertex;
END $$;

-- Add up to cnt new random edges between existing vertices; never a duplicate
-- pair in either direction, never a self-loop. Integer weights 1..9.
CREATE PROCEDURE add_edges(cnt int, lbl text) LANGUAGE plpgsql AS $$
DECLARE nmax int;
BEGIN
    SELECT max(n) INTO nmax FROM vids;
    EXECUTE format($q$
        INSERT INTO valgraph.%I (start_id, end_id, properties)
        SELECT s.id, e.id, ('{"weight": ' || r.w || ', "cost": ' || r.w || '}')::agtype
        FROM (SELECT DISTINCT ON (a, b) a, b, w FROM (
                SELECT least(x, y) a, greatest(x, y) b, w FROM (
                  SELECT 1 + floor(random()*%s)::int + 0*g x, 1 + floor(random()*%s)::int + 0*g y,
                         1 + floor(random()*9)::int + 0*g w
                  FROM generate_series(1,%s) g) q WHERE x <> y) q2 ORDER BY a, b) r
        JOIN vids s ON s.n = r.a JOIN vids e ON e.n = r.b
        WHERE NOT EXISTS (SELECT 1 FROM valgraph._ag_label_edge x
                          WHERE (x.start_id = s.id AND x.end_id = e.id)
                             OR (x.start_id = e.id AND x.end_id = s.id))$q$,
        lbl, nmax, nmax, cnt);
END $$;

CREATE PROCEDURE add_vertices(cnt int) LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO valgraph."VA" (properties) SELECT '{}'::agtype FROM generate_series(1, cnt / 2);
    INSERT INTO valgraph."VB" (properties) SELECT '{}'::agtype FROM generate_series(1, cnt - cnt / 2);
    CALL refresh_ids();
END $$;

CREATE PROCEDURE del_vertices(cnt int) LANGUAGE plpgsql AS $$
BEGIN
    CREATE TEMP TABLE dv ON COMMIT DROP AS
        SELECT id FROM valgraph._ag_label_vertex ORDER BY random() LIMIT cnt;
    DELETE FROM valgraph._ag_label_edge WHERE start_id IN (SELECT id FROM dv) OR end_id IN (SELECT id FROM dv);
    DELETE FROM valgraph._ag_label_vertex WHERE id IN (SELECT id FROM dv);
    CALL refresh_ids();
END $$;

CREATE PROCEDURE del_edges(cnt int) LANGUAGE plpgsql AS $$
BEGIN
    CREATE TEMP TABLE de ON COMMIT DROP AS
        SELECT id FROM valgraph._ag_label_edge ORDER BY random() LIMIT cnt;
    DELETE FROM valgraph._ag_label_edge WHERE id IN (SELECT id FROM de);
END $$;

CREATE PROCEDURE reweight_edges(cnt int) LANGUAGE plpgsql AS $$
BEGIN
    CREATE TEMP TABLE re ON COMMIT DROP AS
        SELECT id FROM valgraph._ag_label_edge ORDER BY random() LIMIT cnt;
    UPDATE valgraph._ag_label_edge e SET properties =
        ('{"weight": ' || r.w || ', "cost": ' || r.w || '}')::agtype
    FROM (SELECT id, 1 + floor(random()*9)::int w FROM re) r
    WHERE e.id = r.id;
END $$;

-- Ground truth from the AGE tables vs. the index content as seen by algorithms.
CREATE PROCEDURE check_proj(lbl text, p text) LANGUAGE plpgsql AS $$
DECLARE dups bigint; loops bigint; tn bigint; tedges bigint; gn bigint; ge bigint;
        d1 bigint; d2 bigint; w1 bigint; w2 bigint; ok boolean; det text;
BEGIN
    SELECT count(*) - count(DISTINCT (least(s,e), greatest(s,e))), count(*) FILTER (WHERE s = e)
      INTO dups, loops
      FROM (SELECT (start_id::text)::bigint s, (end_id::text)::bigint e FROM valgraph._ag_label_edge) x;

    CREATE TEMP TABLE tv ON COMMIT DROP AS SELECT (id::text)::bigint id FROM valgraph._ag_label_vertex;
    CREATE TEMP TABLE te ON COMMIT DROP AS
        SELECT (start_id::text)::bigint s, (end_id::text)::bigint e,
               coalesce(ag_catalog.agtype_access_operator(properties, '"weight"'::agtype)::text::float8, 1) w
        FROM valgraph._ag_label_edge
        WHERE (start_id::text)::bigint IN (SELECT id FROM tv) AND (end_id::text)::bigint IN (SELECT id FROM tv);
    CREATE TEMP TABLE td ON COMMIT DROP AS
        SELECT tv.id, count(x.id) deg, coalesce(sum(x.w), 0) wdeg
        FROM tv LEFT JOIN (SELECT s id, w FROM te UNION ALL SELECT e, w FROM te) x USING (id)
        GROUP BY tv.id;
    SELECT count(*) INTO tn FROM tv;
    SELECT count(*) INTO tedges FROM te;
    SELECT node_count, edge_count INTO gn, ge FROM age_gds.graph_list() WHERE name = p;

    CREATE TEMP TABLE sd ON COMMIT DROP AS
        SELECT "nodeId" id, score FROM age_gds.degree_stream(graph_name => p);
    CREATE TEMP TABLE sw ON COMMIT DROP AS
        SELECT "nodeId" id, score FROM age_gds.degree_stream(graph_name => p,
                                                              relationship_weight_property => 'cost');
    SELECT count(*) INTO d1 FROM (SELECT id, deg::float8 FROM td EXCEPT SELECT id, score FROM sd) q;
    SELECT count(*) INTO d2 FROM (SELECT id, score FROM sd EXCEPT SELECT id, deg::float8 FROM td) q;
    SELECT count(*) INTO w1 FROM (SELECT id, wdeg::float8 FROM td EXCEPT SELECT id, score FROM sw) q;
    SELECT count(*) INTO w2 FROM (SELECT id, score FROM sw EXCEPT SELECT id, wdeg::float8 FROM td) q;

    ok := dups = 0 AND loops = 0 AND (p = 'live' OR (gn = tn AND ge = tedges)) AND d1 + d2 = 0 AND w1 + w2 = 0;
    det := format('graph: %s nodes/%s edges dup=%s loops=%s | catalog %s/%s | degree diff %s+%s, weighted diff %s+%s',
                  tn, tedges, dups, loops, gn, ge, d1, d2, w1, w2);
    INSERT INTO results VALUES (lbl, p, ok, det);
    RAISE NOTICE '% % %: %', CASE WHEN ok THEN 'PASS' ELSE 'FAIL' END, p, lbl, det;
    DROP TABLE tv, te, td, sd, sw;
END $$;

CALL add_vertices(2000);
CALL add_edges(5000, 'EA');
CALL add_edges(2000, 'EB');

SELECT age_gds.graph_project('inc', 'valgraph',
    'MATCH (n) RETURN id(n) AS id',
    'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
    false, true, true);
SELECT age_gds.graph_index('live', 'valgraph', false, true);
CALL check_proj('initial', 'inc');
CALL check_proj('initial', 'live');

CREATE PROCEDURE step(lbl text) LANGUAGE plpgsql AS $$
BEGIN
    COMMIT;
    CALL check_proj(lbl || ' (trigger, before reindex)', 'live');
    PERFORM age_gds.graph_reindex('inc');
    PERFORM age_gds.graph_reindex('live');
    COMMIT;
    CALL check_proj(lbl || ' (reindex)', 'inc');
    CALL check_proj(lbl || ' (compact)', 'live');
END $$;

CALL add_vertices(300);  CALL add_edges(600, 'EA');            CALL step('add vertices+edges');
CALL del_vertices(150);                                         CALL step('delete vertices');
CALL del_edges(200);                                            CALL step('delete edges');
CALL reweight_edges(200);                                       CALL step('reweight edges');
CALL add_vertices(100);  CALL add_edges(400, 'EB');            CALL step('add after tombstones');
CALL del_vertices(80); CALL add_vertices(80); CALL add_edges(300, 'EA'); CALL del_edges(100);
                                                                CALL step('mixed');

SELECT age_gds.graph_reindex('inc', true);
SELECT age_gds.graph_reindex('live', true);
CALL check_proj('force rebuild', 'inc');
CALL check_proj('force rebuild', 'live');
SELECT age_gds.graph_reindex('inc');
CALL check_proj('idempotent', 'inc');

\echo === SUMMARY
SELECT ok, count(*) FROM results GROUP BY ok ORDER BY ok;
\echo === FAILURES
SELECT proj, label, detail FROM results WHERE NOT ok;
