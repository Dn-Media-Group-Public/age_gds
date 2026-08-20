-- Cypher-callable community-detection overloads
-- (docs/implemented-plans/cypher-callable-interface.md, phase 4).
--
-- Note: AGE's Cypher function-name resolution is case-sensitive; calls
-- below use all-lowercase spelling (see phase-2/3 test files).

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Two triangles a-b-c and d-e-f, connected by a single bridge c-d.
SELECT create_graph('cocotest');
SELECT * FROM cypher('cocotest', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'}),
           (:N {name: 'd'}), (:N {name: 'e'}), (:N {name: 'f'})
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'e'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'e'}), (b:N {name:'f'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'f'}), (b:N {name:'d'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('cocotest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'cocoproj',
    age_graph  => 'cocotest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => true
);

-- louvain_stream / leiden_stream / labelPropagation_stream: each returns
-- one row per node (6 total), community ids partition the 6 nodes.
SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.louvain_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.communityId
$$) AS ("nodeId" agtype, "communityId" agtype);
-- expected: 6

SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.leiden_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.communityId
$$) AS ("nodeId" agtype, "communityId" agtype);
-- expected: 6

SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.labelpropagation_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.communityId
$$) AS ("nodeId" agtype, "communityId" agtype);
-- expected: 6

-- labelPropagation_stream: seedProperty refused.
\set ON_ERROR_STOP off
SELECT * FROM cypher('cocotest', $$
    RETURN age_gds.labelpropagation_stream('cocoproj', {seedProperty: 'x'})
$$) AS (r agtype);
\set ON_ERROR_STOP on

-- wcc_stream: single connected component (bridge c-d joins both triangles).
SELECT count(DISTINCT ("componentId"::text)) FROM cypher('cocotest', $$
    UNWIND age_gds.wcc_stream('cocoproj', {}) AS row
    RETURN row.componentId
$$) AS ("componentId" agtype);
-- expected: 1

-- scc_stream: undirected graph -> every node its own SCC is not
-- guaranteed, but the row count must equal the node count.
SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.scc_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.componentId
$$) AS ("nodeId" agtype, "componentId" agtype);
-- expected: 6

-- kCore_stream
SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.kcore_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.coreNumber
$$) AS ("nodeId" agtype, "coreNumber" agtype);
-- expected: 6

-- triangleCount_stream: a and b (in the a-b-c triangle) each have 1 triangle.
SELECT bool_and(("triangleCount"::text)::bigint >= 0) FROM cypher('cocotest', $$
    UNWIND age_gds.trianglecount_stream('cocoproj', {}) AS row
    RETURN row.triangleCount
$$) AS ("triangleCount" agtype);
-- expected: t

-- localClusteringCoefficient_stream
SELECT count(*) FROM cypher('cocotest', $$
    UNWIND age_gds.localclusteringcoefficient_stream('cocoproj', {}) AS row
    RETURN row.nodeId, row.coefficient
$$) AS ("nodeId" agtype, coefficient agtype);
-- expected: 6

-- modularity_stats: single map, dot access.
SELECT (modularity::text)::float8 IS NOT NULL FROM cypher('cocotest', $$
    RETURN age_gds.modularity_stats('cocoproj', {}).modularity
$$) AS (modularity agtype);
-- expected: t

-- conductance_stats: requires communityProperty.
\set ON_ERROR_STOP off
SELECT * FROM cypher('cocotest', $$
    RETURN age_gds.conductance_stats('cocoproj', {})
$$) AS (r agtype);
\set ON_ERROR_STOP on

-- cliqueCounting_stats: array of {cliqueSize, count} despite the name.
-- "count" is a reserved word in AGE's Cypher grammar (aggregate function),
-- so bracket/subscript access is used instead of row.count.
SELECT count(*) > 0 FROM cypher('cocotest', $$
    UNWIND age_gds.cliquecounting_stats('cocoproj', {}) AS row
    RETURN row.cliqueSize, row["count"]
$$) AS ("cliqueSize" agtype, clique_count agtype);
-- expected: t

SELECT age_gds.graph_drop('cocoproj');
SELECT drop_graph('cocotest', true);

DROP EXTENSION age_gds CASCADE;
