-- Cypher-callable centrality overloads
-- (docs/implemented-plans/cypher-callable-interface.md, phase 3).
--
-- Note: AGE's Cypher function-name resolution is case-sensitive (no SQL
-- identifier folding) -- calls below use the all-lowercase spelling even
-- though the SQL entry points accept any case. See
-- docs/architecture.md's "Cypher-callable interface" section.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Triangle: a-b, b-c, c-a (undirected), weight 1.0 each.
SELECT create_graph('ccctest');
SELECT * FROM cypher('ccctest', $$
    CREATE (:N {name: 'a'}), (:N {name: 'b'}), (:N {name: 'c'})
$$) AS (result agtype);
SELECT * FROM cypher('ccctest', $$
    MATCH (a:N {name:'a'}), (b:N {name:'b'}) CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccctest', $$
    MATCH (a:N {name:'b'}), (b:N {name:'c'}) CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);
SELECT * FROM cypher('ccctest', $$
    MATCH (a:N {name:'c'}), (b:N {name:'a'}) CREATE (a)-[:REL {weight: 1.0}]->(b)
$$) AS (result agtype);

SELECT age_gds.graph_project(
    name       => 'cccproj',
    age_graph  => 'ccctest',
    node_query => $$MATCH (n:N) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:N)-[r:REL]->(b:N) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => true
);

-- pageRank_stream: array of {nodeId, score}.
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.pagerank_stream('cccproj', {dampingFactor: 0.85, maxIterations: 20}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- pageRank_stats: single map, dot access.
SELECT (("computeMillis"::text)::bigint) >= 0 FROM cypher('ccctest', $$
    RETURN age_gds.pagerank_stats('cccproj', {}).computeMillis
$$) AS ("computeMillis" agtype);
-- expected: t

-- betweenness_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.betweenness_stream('cccproj', {}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- closeness_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.closeness_stream('cccproj', {useWassermanFaust: true}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- harmonicCentrality_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.harmoniccentrality_stream('cccproj', {}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- degree_stream: every node in a triangle has degree 2.
SELECT bool_and((score::text)::float8 = 2.0) FROM cypher('ccctest', $$
    UNWIND age_gds.degree_stream('cccproj', {orientation: 'UNDIRECTED'}) AS row
    RETURN row.score
$$) AS (score agtype);
-- expected: t

-- eigenvector_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.eigenvector_stream('cccproj', {}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- hits_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.hits_stream('cccproj', {}) AS row
    RETURN row.nodeId, row.auth, row.hub
$$) AS ("nodeId" agtype, auth agtype, hub agtype);
-- expected: 3

-- articleRank_stream
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.articlerank_stream('cccproj', {}) AS row
    RETURN row.nodeId, row.score
$$) AS ("nodeId" agtype, score agtype);
-- expected: 3

-- articulationPoints_stream: a triangle has none.
SELECT count(*) FROM cypher('ccctest', $$
    UNWIND age_gds.articulationpoints_stream('cccproj', {}) AS row
    RETURN row.nodeId
$$) AS ("nodeId" agtype);
-- expected: 0

-- pageRank_write: single map {nodePropertiesWritten}, then verify via MATCH.
SELECT (("nodePropertiesWritten"::text)::bigint) FROM cypher('ccctest', $$
    RETURN age_gds.pagerank_write('cccproj', {writeProperty: 'pr'}).nodePropertiesWritten
$$) AS ("nodePropertiesWritten" agtype);
-- expected: 3

SELECT count(*) FROM cypher('ccctest', $$
    MATCH (n:N) WHERE n.pr IS NOT NULL RETURN n
$$) AS (n agtype);
-- expected: 3

-- betweenness_write
SELECT (("nodePropertiesWritten"::text)::bigint) FROM cypher('ccctest', $$
    RETURN age_gds.betweenness_write('cccproj', {writeProperty: 'bw'}).nodePropertiesWritten
$$) AS ("nodePropertiesWritten" agtype);
-- expected: 3

SELECT age_gds.graph_drop('cccproj');
SELECT drop_graph('ccctest', true);

DROP EXTENSION age_gds CASCADE;
