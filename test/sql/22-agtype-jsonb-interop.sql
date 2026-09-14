-- agtype <-> jsonb SQL interop
-- (docs/implemented-plans/agtype-jsonb-sql-interop.md).
--
-- Plain SQL only -- no Cypher overload: jsonb has no representation in
-- Cypher's type system (Cypher property values are natively agtype).

LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

SELECT create_graph('ajitest');
SELECT * FROM cypher('ajitest', $$
    CREATE (:Person {name: 'a', age: 30, tags: ['x', 'y'], address: {city: 'Oslo'}})
$$) AS (result agtype);

-- forward cast: agtype -> jsonb, nested map/array round-trips structurally
SELECT (properties::jsonb)->>'name' FROM ajitest."Person";
-- expected: a

SELECT (properties::jsonb)->'tags' FROM ajitest."Person";
-- expected: ["x", "y"]

SELECT (properties::jsonb)->'address'->>'city' FROM ajitest."Person";
-- expected: Oslo

-- explicit function form (same as the cast)
SELECT age_gds.agtype_to_jsonb(properties)->>'age' FROM ajitest."Person";
-- expected: 30

-- reverse cast: jsonb -> agtype
SELECT age_gds.jsonb_to_agtype('{"a": 1, "b": [1, 2, 3]}'::jsonb);
-- expected: {"a": 1, "b": [1, 2, 3]}

-- round-trip agtype -> jsonb -> agtype is lossless
SELECT properties = (properties::jsonb::ag_catalog.agtype) FROM ajitest."Person";
-- expected: t

-- STRICT: NULL in, NULL out, no error
SELECT age_gds.agtype_to_jsonb(NULL::ag_catalog.agtype) IS NULL;
-- expected: t

SELECT age_gds.jsonb_to_agtype(NULL::jsonb) IS NULL;
-- expected: t

-- a full vertex value (not its properties map) carries AGE's type suffix
-- (::vertex), which is not valid JSON -- this is expected to error, not a
-- bug. Cast properties/properties(n), not the vertex itself.
\set ON_ERROR_STOP off
SELECT * FROM cypher('ajitest', $$
    MATCH (n:Person) RETURN n::jsonb
$$) AS (result jsonb);
-- expected: ERROR: invalid input syntax for type json
\set ON_ERROR_STOP on
