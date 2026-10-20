-- docker/bench-reindex.sql -- driven by docker/bench-reindex.sh (psql -v nv=.. -v ne=..).
-- Not part of run-tests.sh: too slow for every run.
\set ON_ERROR_STOP on
\timing on
CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;
SELECT create_graph('bench');
SELECT create_vlabel('bench', ('V' || i)::cstring) FROM generate_series(1, 6) i;
SELECT create_elabel('bench', ('E' || i)::cstring) FROM generate_series(1, 8) i;

SELECT set_config('bench.nv', :'nv', false), set_config('bench.ne', :'ne', false);

-- Seed in committed 50,000-row chunks (the label tables' id default supplies graphids).
DO $$
DECLARE n int := current_setting('bench.nv')::int; done int := 0; c int;
BEGIN
    WHILE done < n LOOP
        c := least(50000, n - done);
        EXECUTE format('INSERT INTO bench.%I (properties) SELECT %L::agtype FROM generate_series(1,%s)',
                       'V' || (1 + done % 6), '{"k": 1}', c);
        done := done + c;
        RAISE NOTICE 'vertices %/%', done, n;
        COMMIT;
    END LOOP;
END $$;
CREATE TABLE bench_ids (n serial PRIMARY KEY, id graphid);
INSERT INTO bench_ids (id) SELECT id FROM bench._ag_label_vertex;
ANALYZE bench_ids;

-- Edges between random existing vertices; numeric weight on most.
CREATE PROCEDURE bench_add_edges(cnt int) LANGUAGE plpgsql AS $$
DECLARE nmax int; done int := 0; c int;
BEGIN
    SELECT max(n) INTO nmax FROM bench_ids;
    WHILE done < cnt LOOP
        c := least(50000, cnt - done);
        EXECUTE format($q$INSERT INTO bench.%I (start_id, end_id, properties)
            SELECT s.id, e.id,
                   CASE WHEN r.w < 0.9 THEN '{"weight": 2.0}'::agtype ELSE '{}'::agtype END
            FROM generate_series(1,%s) g
            CROSS JOIN LATERAL (SELECT 1 + floor(random()*%s)::int + 0*g AS a,
                                       1 + floor(random()*%s)::int + 0*g AS b,
                                       random() + 0*g AS w) r
            JOIN bench_ids s ON s.n = r.a
            JOIN bench_ids e ON e.n = r.b$q$, 'E' || (1 + done % 8), c, nmax, nmax);
        done := done + c;
        COMMIT;
    END LOOP;
END $$;
CALL bench_add_edges(:ne);
ANALYZE;

\echo '== initial project (queries per :qmode)'
SELECT CASE WHEN :'qmode' = 'omitted'
  THEN age_gds.graph_project('bench_inc', 'bench', directed => false, weighted => :wt, incremental => true)
  ELSE age_gds.graph_project('bench_inc', 'bench',
        'MATCH (n) RETURN id(n) AS id',
        'MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight',
        false, :wt, true) END;
\echo '== no-change reindex'
SELECT age_gds.graph_reindex('bench_inc');

-- Delta of new edges (5% of each delta also adds a new vertex appended at the end).
CREATE PROCEDURE bench_delta(cnt int) LANGUAGE plpgsql AS $$
DECLARE nv int := greatest(cnt / 20, 1);
BEGIN
    INSERT INTO bench."V1" (properties) SELECT '{"k": 2}'::agtype FROM generate_series(1, nv);
    INSERT INTO bench_ids (id) SELECT v.id FROM bench."V1" v WHERE NOT EXISTS (SELECT 1 FROM bench_ids b WHERE b.id = v.id);
    COMMIT;
    CALL bench_add_edges(cnt);
END $$;

\echo '== reindex after delta of N new edges'
CALL bench_delta(100); SELECT age_gds.graph_reindex('bench_inc');
CALL bench_delta(400); SELECT age_gds.graph_reindex('bench_inc');
CALL bench_delta(1600); SELECT age_gds.graph_reindex('bench_inc');
CALL bench_delta(6400); SELECT age_gds.graph_reindex('bench_inc');
CALL bench_delta(25600); SELECT age_gds.graph_reindex('bench_inc');

\echo '== bulk delta: 50,000 new edges'
SET statement_timeout = '30min';
CALL bench_delta(50000); SELECT age_gds.graph_reindex('bench_inc');
RESET statement_timeout;

\echo '== correctness: full rebuild vs incrementally maintained'
SELECT name, node_count, edge_count FROM age_gds.graph_list() WHERE name = 'bench_inc';
SELECT age_gds.graph_reindex('bench_inc', true);
SELECT name, node_count, edge_count FROM age_gds.graph_list() WHERE name = 'bench_inc';
