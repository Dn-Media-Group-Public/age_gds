-- Cypher-callable similarity/link-prediction/embedding overloads
-- (docs/implemented-plans/cypher-callable-interface.md, phase 5).
--
-- Note: AGE's Cypher function-name resolution is case-sensitive; calls
-- below use all-lowercase spelling (see phase-2/3/4 test files).

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Square a-b-c-d-a (undirected), every node degree 2.
SELECT create_graph('ccstest');
SELECT * FROM cypher('ccstest', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'}), (:N {name: 'd'})
$$) AS (result agtype);
SELECT * FROM cypher('ccstest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccstest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccstest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'d'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccstest', $$
    MATCH (a:N {name:'d'}), (b:N {name:'a'}) CREATE (a)-[:REL {weight:1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'ccsproj',
    age_graph  => 'ccstest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => true
);

SELECT (id::text)::bigint AS a_id FROM cypher('ccstest', $$MATCH (n:N {name:'a'}) RETURN id(n) AS id$$) AS (id agtype) \gset
SELECT (id::text)::bigint AS c_id FROM cypher('ccstest', $$MATCH (n:N {name:'c'}) RETURN id(n) AS id$$) AS (id agtype) \gset

-- nodeSimilarity_stream: array of {node1, node2, similarity}.
SELECT count(*) > 0 FROM cypher('ccstest', $$
    UNWIND age_gds.nodesimilarity_stream('ccsproj', {similarityCutoff: 0.0}) AS row
    RETURN row.node1, row.node2, row.similarity
$$) AS (node1 agtype, node2 agtype, similarity agtype);
-- expected: t

-- knn_stream: array of {sourceNodeId, targetNodeId, similarity, rank}.
SELECT count(*) > 0 FROM cypher('ccstest', $$
    UNWIND age_gds.knn_stream('ccsproj', {topK: 2}) AS row
    RETURN row.sourceNodeId, row.targetNodeId, row.similarity, row.rank
$$) AS ("sourceNodeId" agtype, "targetNodeId" agtype, similarity agtype, rank agtype);
-- expected: t

-- linkprediction_*_stream: array of {node1, node2, score}.
SELECT count(*) >= 0 FROM cypher('ccstest', $$
    UNWIND age_gds.linkprediction_adamicadar_stream('ccsproj', {}) AS row
    RETURN row.node1, row.node2, row.score
$$) AS (node1 agtype, node2 agtype, score agtype);
-- expected: t

SELECT count(*) >= 0 FROM cypher('ccstest', $$
    UNWIND age_gds.linkprediction_commonneighbors_stream('ccsproj', {}) AS row
    RETURN row.node1, row.node2, row.score
$$) AS (node1 agtype, node2 agtype, score agtype);
-- expected: t

SELECT count(*) >= 0 FROM cypher('ccstest', $$
    UNWIND age_gds.linkprediction_preferentialattachment_stream('ccsproj', {}) AS row
    RETURN row.node1, row.node2, row.score
$$) AS (node1 agtype, node2 agtype, score agtype);
-- expected: t

SELECT count(*) >= 0 FROM cypher('ccstest', $$
    UNWIND age_gds.linkprediction_resourceallocation_stream('ccsproj', {}) AS row
    RETURN row.node1, row.node2, row.score
$$) AS (node1 agtype, node2 agtype, score agtype);
-- expected: t

-- linkprediction_* single-pair scalar overloads: bare agtype number, no
-- UNWIND (already scalar). a and c are opposite corners of the square,
-- sharing exactly 2 common neighbors (b and d).
SELECT format($q$
SELECT (score::text)::float8 FROM cypher('ccstest', $c$
    RETURN age_gds.linkprediction_commonneighbors(%s, %s, 'ccsproj', {})
$c$) AS (score agtype)
$q$, :a_id, :c_id) \gexec
-- expected: 2

SELECT format($q$
SELECT (score::text)::float8 >= 0 FROM cypher('ccstest', $c$
    RETURN age_gds.linkprediction_adamicadar(%s, %s, 'ccsproj', {})
$c$) AS (score agtype)
$q$, :a_id, :c_id) \gexec
-- expected: t

SELECT format($q$
SELECT (score::text)::float8 >= 0 FROM cypher('ccstest', $c$
    RETURN age_gds.linkprediction_preferentialattachment(%s, %s, 'ccsproj', {})
$c$) AS (score agtype)
$q$, :a_id, :c_id) \gexec
-- expected: t

SELECT format($q$
SELECT (score::text)::float8 >= 0 FROM cypher('ccstest', $c$
    RETURN age_gds.linkprediction_resourceallocation(%s, %s, 'ccsproj', {})
$c$) AS (score agtype)
$q$, :a_id, :c_id) \gexec
-- expected: t

-- direction other than BOTH is refused.
\set ON_ERROR_STOP off
SELECT format($q$
SELECT * FROM cypher('ccstest', $c$
    RETURN age_gds.linkprediction_adamicadar(%s, %s, 'ccsproj', {direction: 'OUTGOING'})
$c$) AS (score agtype)
$q$, :a_id, :c_id) \gexec
\set ON_ERROR_STOP on

-- node2vec_stream: array of {nodeId, embedding}. Square graph has no
-- isolated nodes, so this should succeed.
SELECT count(*) FROM cypher('ccstest', $$
    UNWIND age_gds.node2vec_stream('ccsproj', {embeddingDim: 4, walkLength: 5, walksPerNode: 2}) AS row
    RETURN row.nodeId, row.embedding
$$) AS ("nodeId" agtype, embedding agtype);
-- expected: 4

SELECT age_gds.graph_drop('ccsproj');
SELECT drop_graph('ccstest', true);

DROP EXTENSION age_gds CASCADE;
