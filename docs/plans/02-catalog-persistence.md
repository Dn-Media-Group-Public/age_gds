# Stage 2 — Catalog Persistence (Stub Loader)

## Scope

Binary `.bin` file format, catalog metadata table, and `graph_project` /
`graph_drop` / `graph_list` / `graph_exists` SQL functions. The AGE loader
is **not** implemented yet — `graph_project` uses a stub that builds a small
synthetic hardcoded `NetworKit::Graph` (3 nodes, 3 edges) and serializes it.
Goal: verify the full persistence contract (write → read → round-trip) before
touching AGE.

The stub must use a real `NetworKit::Graph` object, not a plain struct or
hand-written adjacency list. This keeps the serialization code honest and
ensures the build links against NetworKit from the start.

Builds on Stage 1.

## Files to Create

```
src/catalog/
    serialization.h/cpp     # .bin write (atomic via tmp+rename) and read
                            # write function takes (NetworKit::Graph&, NodeIdMap&)
                            # read function reconstructs NetworKit::Graph from .bin
    graph_project.cpp       # graph_project(), graph_drop(), graph_list(),
                            # graph_exists(); stub loader creates synthetic nk::Graph
sql/age_gds--1.0.sql        # extend: CREATE TABLE age_gds.graphs + SQL functions
```

### Required includes in serialization files

```cpp
#include <networkit/graph/Graph.hpp>   // NetworKit::Graph
#include <networkit/Globals.hpp>       // nk::node, nk::edgeweight
```

Do not define local type aliases for NetworKit types.

## Binary File Format

See `initial.md` — format is unchanged. Implement exactly as specified:

```
Header (64 bytes):
  magic:        uint64   0x4147455F4744530A
  version:      uint32
  flags:        uint32   bit0=directed, bit1=weighted
  node_count:   uint64
  edge_count:   uint64
  reserved:     bytes[24]

Node table (node_count × 8 bytes):
  graphid[]:    int64[]   AGE graphids mapped via NodeIdMap
                          (stub: use synthetic ids 1, 2, 3)

Edge table (edge_count × 24 bytes):
  src[]:        uint32    nk::node index (0-based)
  dst[]:        uint32    nk::node index (0-based)
  weight[]:     float64
```

The serialization write function signature must be:

```cpp
void gds_serialize(
    const char          *path,
    const NetworKit::Graph &graph,
    const NodeIdMap     &id_map,
    bool                 directed,
    bool                 weighted
);
```

It iterates `graph.forEdges(...)` and `graph.forNodes(...)` to extract
the data — do not maintain a separate edge list. Write atomically:
write to `<name>.bin.tmp`, then `rename(2)`.

## Catalog Table

```sql
CREATE TABLE age_gds.graphs (
    name          text PRIMARY KEY,
    age_graph     text        NOT NULL,
    node_query    text,
    edge_query    text,
    node_count    bigint,
    edge_count    bigint,
    directed      boolean     NOT NULL DEFAULT false,
    weighted      boolean     NOT NULL DEFAULT false,
    file_path     text        NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),
    projected_by  text
);
```

## SQL Interface

```sql
-- Project (stub: ignores queries, writes synthetic 3-node graph)
SELECT age_gds.graph_project(
    'myGraph',
    'ignored_node_query',
    'ignored_edge_query'
);
-- → void (raises on error)
```

The stub loader inside `graph_project.cpp` must:
1. Create a `NetworKit::Graph` with 3 nodes (directed=false, weighted=false).
2. Add edges: (0,1), (1,2), (0,2).
3. Create a `NodeIdMap` mapping nk indices 0,1,2 → synthetic graphids 1,2,3.
4. Call `gds_serialize(path, graph, id_map, false, false)`.

Do not write raw integers directly; use `graph.forEdges(...)` in `gds_serialize`.

`graph_project` and `graph_drop` must be transactional: if the transaction
rolls back, neither the file nor the catalog row persists. Use
`RegisterXactCallback` to unlink the temp file on abort if the rename has
not yet occurred.

## Docker Test Environment

Same container as Stage 1. Rebuild extension after adding new source files.

```bash
docker exec age_gds_test bash -c "
    cd /workspace &&
    cmake --build build -j$(nproc) &&
    cmake --install build
"
```

## Tests

```sql
\c postgres
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Project a graph (stub loader)
SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');

-- Verify catalog row
SELECT name, node_count, edge_count FROM age_gds.graph_list();
-- expected: g1 | 3 | 3

-- Verify file exists on disk
-- (run from bash, not psql):
--   docker exec age_gds_test ls -lh $(psql -U postgres -At \
--       -c "SELECT file_path FROM age_gds.graphs WHERE name='g1'")
-- expected: file present, size > 64 bytes

-- Exists
SELECT age_gds.graph_exists('g1');      -- must return true
SELECT age_gds.graph_exists('noexist'); -- must return false

-- Round-trip: drop and re-project
SELECT age_gds.graph_drop('g1');
SELECT age_gds.graph_exists('g1');      -- must return false
-- verify file gone:
-- docker exec age_gds_test test -f <file_path> && echo EXISTS || echo GONE
-- expected: GONE

SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');
SELECT age_gds.graph_exists('g1');      -- must return true

-- Transactional rollback test
BEGIN;
SELECT age_gds.graph_project('g2', 'stub_nodes', 'stub_edges');
ROLLBACK;
SELECT age_gds.graph_exists('g2');      -- must return false
-- verify no orphaned .bin.tmp file in data_dir

-- Duplicate name error
SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');
-- must raise ERROR (duplicate key)

-- Cleanup
SELECT age_gds.graph_drop('g1');
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] Extension builds cleanly with new catalog sources
- [ ] `graph_project` inserts catalog row and writes valid `.bin` file
- [ ] `.bin` header magic and node/edge counts are correct (verify with
      a small C reader or `xxd`)
- [ ] `graph_list` returns correct row
- [ ] `graph_exists` returns correct boolean
- [ ] `graph_drop` removes both catalog row and `.bin` file
- [ ] Rolled-back `graph_project` leaves no catalog row and no file
- [ ] Duplicate `graph_project` raises error
