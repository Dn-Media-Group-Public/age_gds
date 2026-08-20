-- Cypher-callable catalog overloads
-- (docs/implemented-plans/cypher-callable-interface.md, phase 6, final).
--
-- Note: AGE's Cypher function-name resolution is case-sensitive; calls
-- below use all-lowercase spelling (see phase-2..5 test files).
--
-- graph_list()/cache_stats() have no arguments in SQL, so their agtype
-- overload can't share the name (Postgres can't overload on return type
-- alone with an identical zero-arg signature) -- they use the
-- graph_list_cypher()/cache_stats_cypher() names documented in
-- sql/age_gds--1.0.sql.

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('ccatest');
SELECT * FROM cypher('ccatest', $$
    CREATE (:Person {name: 'a'}), (:Person {name: 'b'})
$$) AS (result agtype);
SELECT * FROM cypher('ccatest', $$
    MATCH (a:Person {name:'a'}), (b:Person {name:'b'})
    CREATE (a)-[:KNOWS]->(b)
$$) AS (result agtype);

-- graph_project: label/property projection translation. nodeProjection
-- {Person: {}} compiles to MATCH (n:Person) RETURN id(n) AS id;
-- relationshipProjection '*' compiles to the default all-edges query.
SELECT (("graphName")::text) FROM cypher('ccatest', $$
    RETURN age_gds.graph_project('ccaproj', {Person: {}}, '*', {ageGraph: 'ccatest'}).graphName
$$) AS ("graphName" agtype);
-- expected: "ccaproj"

SELECT count(*) FROM age_gds.graph_list() WHERE name = 'ccaproj';
-- expected: 1 (SQL entry point unaffected)

-- graph_project: unsupported 'properties' key is refused, not ignored.
\set ON_ERROR_STOP off
SELECT * FROM cypher('ccatest', $$
    RETURN age_gds.graph_project('ccabad', {Person: {properties: ['name']}}, '*', {ageGraph: 'ccatest'})
$$) AS (r agtype);
\set ON_ERROR_STOP on

-- graph_exists: bare scalar boolean.
SELECT (exists_val::text)::boolean FROM cypher('ccatest', $$
    RETURN age_gds.graph_exists('ccaproj')
$$) AS (exists_val agtype);
-- expected: t

SELECT (exists_val::text)::boolean FROM cypher('ccatest', $$
    RETURN age_gds.graph_exists('doesnotexist')
$$) AS (exists_val agtype);
-- expected: f

-- graph_is_stale: bare scalar boolean.
SELECT (stale::text)::boolean FROM cypher('ccatest', $$
    RETURN age_gds.graph_is_stale('ccaproj')
$$) AS (stale agtype);
-- expected: f (freshly projected)

-- cache_load: single map {graphName}.
SELECT (("graphName")::text) FROM cypher('ccatest', $$
    RETURN age_gds.cache_load('ccaproj').graphName
$$) AS ("graphName" agtype);
-- expected: "ccaproj"

-- cache_stats_cypher(): single map {entryCount, totalBytes, limitBytes}.
SELECT (("entryCount"::text)::int) >= 1 FROM cypher('ccatest', $$
    RETURN age_gds.cache_stats_cypher().entryCount
$$) AS ("entryCount" agtype);
-- expected: t

-- graph_list_cypher(): array of maps.
SELECT count(*) > 0 FROM cypher('ccatest', $$
    UNWIND age_gds.graph_list_cypher() AS row
    RETURN row.name, row.age_graph
$$) AS (name agtype, age_graph agtype);
-- expected: t

-- graph_reindex: flat (non-incremental) projection has no incremental
-- mode -- force := false is invalid, matching the SQL entry point.
\set ON_ERROR_STOP off
SELECT * FROM cypher('ccatest', $$
    RETURN age_gds.graph_reindex('ccaproj', {force: false})
$$) AS (r agtype);
\set ON_ERROR_STOP on

-- graph_drop: single map {graphName}.
SELECT (("graphName")::text) FROM cypher('ccatest', $$
    RETURN age_gds.graph_drop('ccaproj').graphName
$$) AS ("graphName" agtype);
-- expected: "ccaproj"

SELECT (exists_val::text)::boolean FROM cypher('ccatest', $$
    RETURN age_gds.graph_exists('ccaproj')
$$) AS (exists_val agtype);
-- expected: f (dropped)

SELECT drop_graph('ccatest', true);

DROP EXTENSION age_gds CASCADE;
