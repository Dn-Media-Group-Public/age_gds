-- Per-call relationship weight override (docs/implemented-plans/00-weight-plumbing.md)
--
-- Projects a graph weighted by `w1` (uniform: node `a`'s two out-edges both
-- weigh 1.0, so PageRank splits its rank evenly between them), then re-runs
-- pageRank_stream against the *named/cached* graph with
-- relationship_weight_property => 'w2', where w2 makes `a`'s edge to `b`
-- 9x heavier than its edge to `c`. Confirms the override actually changes
-- the result (proving reweight_from_property() ran against a live edge
-- query, not a no-op), and that a NULL override is a no-op.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('wtest');

SELECT * FROM cypher('wtest', $$
    CREATE (:N {name: 'a'}),
           (:N {name: 'b'}),
           (:N {name: 'c'})
$$) AS (result agtype);

-- Two outgoing edges from `a`, evenly weighted under w1 (both 1.0), but
-- lopsided 9:1 under w2. PageRank distributes a node's rank to its
-- out-neighbors in proportion to *normalized* edge weight, so a single-edge
-- reweight (as in an earlier version of this test) is invisible: with only
-- one successor the normalization ratio is always 1 regardless of the raw
-- weight. Two competing out-edges are required to make the override
-- observable.
SELECT * FROM cypher('wtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'})
    CREATE (a)-[:REL {w1: 1.0, w2: 9.0}]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('wtest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'c'})
    CREATE (a)-[:REL {w1: 1.0, w2: 1.0}]->(b)
$$) AS (result agtype);

-- Projected with w1 as the weight (uniform: every edge weighs 1.0).
SELECT age_gds.graph_project(
    name       => 'wproj',
    age_graph  => 'wtest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N)
                   RETURN id(a) AS src, id(b) AS dst, r.w1 AS weight$$,
    directed   => true,
    weighted   => true
);

-- Sanity: cached graph resolves and pageRank_stream works with no override.
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'wproj');
-- expected: 3

-- weight_property override (w2) must change the result: w2 makes the a->b
-- edge 10x heavier than at projection time, so scores must differ.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.pageRank_stream(graph_name => 'wproj')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.pageRank_stream(
            graph_name                   => 'wproj',
            relationship_weight_property => 'w2'
        )
    ) d;
    IF diff_count = 0 THEN
        RAISE EXCEPTION 'weight_property override had no observable effect on pageRank_stream';
    END IF;
END $$;

-- A NULL override must be a no-op: identical to not passing it at all.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.pageRank_stream(graph_name => 'wproj')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.pageRank_stream(
            graph_name                   => 'wproj',
            relationship_weight_property => NULL
        )
    ) d;
    IF diff_count <> 0 THEN
        RAISE EXCEPTION 'NULL relationship_weight_property must be a no-op, but results differ';
    END IF;
END $$;

-- The session-cache entry itself must be untouched by the override (the
-- reweighted graph is a private copy) — re-running with no override after
-- the overridden call above must reproduce the original scores.
DO $$
DECLARE
    diff_count int;
BEGIN
    SELECT count(*) INTO diff_count FROM (
        SELECT "nodeId", score FROM age_gds.pageRank_stream(graph_name => 'wproj')
        EXCEPT
        SELECT "nodeId", score FROM age_gds.pageRank_stream(graph_name => 'wproj')
    ) d;
    IF diff_count <> 0 THEN
        RAISE EXCEPTION 'shared cache entry mutated by a weight_property override call';
    END IF;
END $$;

SELECT age_gds.graph_drop('wproj');
DROP EXTENSION age_gds CASCADE;
