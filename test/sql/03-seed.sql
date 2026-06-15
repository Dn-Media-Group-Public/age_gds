CREATE EXTENSION IF NOT EXISTS age;
LOAD 'age';
SET search_path = ag_catalog, "$user", public;

SELECT create_graph('test_graph');

SELECT * FROM cypher('test_graph', $$
    CREATE (:Person {name: 'Alice', age: 30}),
           (:Person {name: 'Bob',   age: 25}),
           (:Person {name: 'Carol', age: 35}),
           (:Person {name: 'Dave',  age: 28})
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Bob'})
    CREATE (a)-[:KNOWS {weight: 1.0}]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person {name:'Bob'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 2.0}]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person {name:'Carol'}), (b:Person {name:'Dave'})
    CREATE (a)-[:KNOWS {weight: 0.5}]->(b)
$$) AS (result agtype);

SELECT * FROM cypher('test_graph', $$
    MATCH (a:Person {name:'Alice'}), (b:Person {name:'Carol'})
    CREATE (a)-[:KNOWS {weight: 1.5}]->(b)
$$) AS (result agtype);
