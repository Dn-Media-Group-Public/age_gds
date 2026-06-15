# Stage 3 — AGE→NetworKit Bridge

## Scope

Replace the Stage 2 stub loader with a real AGE graph loader. Execute
user-supplied Cypher node and edge queries against an AGE graph, build a
`nk::Graph`, and write it to the `.bin` file via the existing serialization
code. Implement `NodeIdMap` (graphid ↔ nk node index).

Builds on Stage 2. `graph_project` now accepts real Cypher queries and a
real AGE graph name.

## Files to Create / Modify

```
src/bridge/
    age_loader.cpp          # execute Cypher via AGE SPI, build NetworKit::Graph
    age_loader.h            # LoadedGraph struct + age_load_graph declaration
    node_id_map.h           # NodeIdMap: graphid ↔ nk::node bidirectional map
src/catalog/
    graph_project.cpp       # replace stub call with age_loader call
CMakeLists.txt              # add src/bridge/age_loader.cpp to sources;
                            # NetworKit and OpenMP already linked from Stage 1
```

**`CMakeLists.txt` already links NetworKit from Stage 1. Do not remove those
lines.** Only add the new source file:

```cmake
add_library(age_gds MODULE
    ...
    src/bridge/age_loader.cpp   # ← add this line
)

target_include_directories(age_gds PRIVATE
    ...
    ${CMAKE_SOURCE_DIR}/src/bridge  # ← add this line
)
```

## NodeIdMap

```cpp
// src/bridge/node_id_map.h
#pragma once

// nk::node comes from NetworKit — do not redefine it locally.
#include <networkit/Globals.hpp>   // defines nk::node (= uint64_t in NetworKit namespace)

#include <cstdint>
#include <unordered_map>
#include <stdexcept>

struct NodeIdMap {
    std::unordered_map<int64_t,  nk::node> graphid_to_nk;
    std::unordered_map<nk::node, int64_t>  nk_to_graphid;

    nk::node intern(int64_t gid) {
        auto [it, inserted] = graphid_to_nk.emplace(gid, (nk::node) graphid_to_nk.size());
        if (inserted) nk_to_graphid[it->second] = gid;
        return it->second;
    }

    int64_t resolve(nk::node n) const { return nk_to_graphid.at(n); }
};
```

The `nk::node` type comes from `<networkit/Globals.hpp>`. **Do not write
`namespace nk { using node = uint64_t; }` or any equivalent local alias.**

## AGE Loader Contract

```cpp
// src/bridge/age_loader.h
#pragma once

// Real NetworKit types — not stubs.
#include <networkit/graph/Graph.hpp>   // NetworKit::Graph
#include <networkit/Globals.hpp>       // nk::node

#include "node_id_map.h"

struct LoadedGraph {
    NetworKit::Graph    graph;
    NodeIdMap           id_map;
};

LoadedGraph age_load_graph(
    const char *age_graph_name,   // AGE graph name (SET graph_path = ...)
    const char *node_query,       // Cypher: must SELECT id(n) AS id
    const char *edge_query,       // Cypher: must SELECT id(a) AS src,
                                  //                   id(b) AS dst
                                  //         optional: weight AS weight
    bool        directed,
    bool        weighted
);
```

`age_loader.cpp` must include:

```cpp
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
```

Edges are added to the `NetworKit::Graph` via `graph.addEdge(src_nk, dst_nk)`
(or `graph.addEdge(src_nk, dst_nk, weight)` for weighted graphs).
Do not accumulate edges in a plain vector and skip the `nk::Graph`.

Execution via AGE's SPI interface:
1. `SPI_connect()`
2. Set `search_path` to include AGE's catalog schema
3. Execute `SELECT * FROM cypher('age_graph_name', $$ node_query $$) AS (id agtype)`
4. Iterate rows; call `id_map.intern(graphid)` for each
5. Execute edge query similarly; add edges to `nk::Graph`
6. `SPI_finish()`

Node query must return column `id` (agtype graphid).
Edge query must return columns `src`, `dst` (agtype graphids), and
optionally `weight` (agtype float).

## Updated `graph_project` Signature

```sql
SELECT age_gds.graph_project(
    name       => 'myGraph',
    age_graph  => 'my_age_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$,
    directed   => false,
    weighted   => false
);
```

## Docker Test Environment

Container needs AGE extension active. Start with:

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
```

Seed test data before running tests:

```sql
CREATE EXTENSION age;
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
```

## Tests

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Project from real AGE graph
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst, r.weight AS weight$$
);

-- Catalog row has correct counts
SELECT name, node_count, edge_count
FROM age_gds.graph_list();
-- expected: people | 4 | 5

-- Verify .bin file is non-trivially sized
-- docker exec age_gds_test wc -c <file_path>
-- expected: 64 + 4*8 + 5*24 = 64 + 32 + 120 = 216 bytes

-- Node IDs in file map to real AGE graphids (spot-check via xxd or
-- a small verification SQL function if implemented)

-- Empty node query returns zero-node graph without crash
SELECT age_gds.graph_project(
    name       => 'empty',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:NoSuchLabel) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:NoSuchLabel)-[r]->(b) RETURN id(a) AS src, id(b) AS dst$$
);
SELECT name, node_count, edge_count FROM age_gds.graph_list()
WHERE name = 'empty';
-- expected: empty | 0 | 0

-- Invalid AGE graph name raises error
SELECT age_gds.graph_project(
    name       => 'bad',
    age_graph  => 'nonexistent_graph',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst$$
);
-- must raise ERROR

-- Cleanup
SELECT age_gds.graph_drop('people');
SELECT age_gds.graph_drop('empty');
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] Extension builds cleanly with AGE bridge sources
- [ ] **NetworKit symbols present in the compiled `.so`**:
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```
      If this prints 0, NetworKit is not linked. Fix the build before running
      any SQL tests.
- [ ] `graph_project` with real Cypher queries writes correct node and edge
      counts to catalog
- [ ] `.bin` node table contains valid AGE graphids (spot-check)
- [ ] Empty-label projection produces zero-count graph without crash or error
- [ ] Invalid AGE graph name raises a clear PostgreSQL ERROR
- [ ] Rollback of `graph_project` leaves no file and no catalog row
      (regression from Stage 2)
