# Stage 4 — Session LRU Cache and mmap Loader

## Scope

Per-session `nk::Graph` cache backed by `mmap` of `.bin` files. First call
to any algorithm (added in Stage 5) deserializes the projected graph from
disk and caches it in the backend's private heap. Subsequent calls in the
same session reuse the deserialized object without I/O. LRU eviction applies
when total cached size exceeds `age_gds.session_cache_mb`.

Builds on Stage 3. No new SQL functions visible to users; this stage is
pure infrastructure. Testing is done by observing timing differences and
adding a debug helper function.

## Files to Create / Modify

```
src/catalog/
    session_cache.cpp       # ProjectedGraph struct, LRU cache, mmap loader
    session_cache.h         # public API used by algorithm layer
```

## Data Structures

```cpp
// src/catalog/session_cache.h

// Real NetworKit type — not a stub or alias.
#include <networkit/graph/Graph.hpp>   // NetworKit::Graph
#include "bridge/node_id_map.h"

struct ProjectedGraph {
    std::string              name;
    NetworKit::Graph         graph;     // real nk::Graph, not a plain struct
    NodeIdMap                id_map;
    size_t                   approx_bytes;  // for LRU accounting
    std::chrono::steady_clock::time_point last_used;
};
```

### mmap Load Path

`session_cache_get`:
1. Check `std::unordered_map<string, ProjectedGraph>` — return if hit.
2. Look up `file_path` from `age_gds.graphs` catalog via SPI.
3. `open` + `mmap` the `.bin` file (read-only, `MAP_SHARED`).
4. Validate magic and version from header.
5. Read node count and edge count from header.
6. Create `NetworKit::Graph(node_count, directed, weighted)`.
7. For each edge in the edge table: call `graph.addEdge(src, dst, weight)`.
8. For each node in the node table: populate `NodeIdMap`.
9. `munmap`.
10. While `session_cache_bytes() + approx_bytes > session_cache_mb * 1024^2`:
    call `session_cache_evict_lru()`.
11. Insert into cache, return reference.

### LRU Eviction

Track `last_used` timestamp per entry. On eviction, destroy the
`ProjectedGraph` (destructor frees `nk::Graph` heap). Use
`on_proc_exit(session_cache_clear, 0)` registered in `_PG_init` to free
cache on backend exit.

## Debug SQL Helper (for testing only)

```sql
-- Returns current session cache stats
SELECT * FROM age_gds.cache_stats();
-- → (entry_count int, total_bytes bigint, limit_bytes bigint)
```

Implement `age_gds_cache_stats` in `session_cache.cpp`. Can be removed or
kept as a diagnostic tool after Stage 4.

## Docker Test Environment

Same container as Stage 3. The test below requires a projected graph from
Stage 3, so run Stage 3 seed + project steps first.

## Tests

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Setup: project a graph (reuse Stage 3 seed data)
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
);

-- Cache starts empty
SELECT entry_count, total_bytes FROM age_gds.cache_stats();
-- expected: 0 | 0

-- Trigger a cache load (use internal test hook or wait for Stage 5 algorithm)
-- For now: call a dedicated test-load function if implemented, OR
-- verify indirectly in Stage 5. Document which approach is taken.

-- After first load:
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 1

-- After second load of same graph (should be cache hit — entry_count unchanged):
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 1

-- LRU eviction under memory pressure:
-- Set limit very low, project a second graph, trigger load of both.
SET age_gds.session_cache_mb = 1;  -- or minimum valid value
-- (project + load second graph, observe first evicted)
-- cache_stats() entry_count must not exceed what fits in 1 MB

-- Cache cleared on new session:
-- \c postgres   (reconnect)
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 0

-- Cleanup
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
```

### Timing Smoke Test (bash)

```bash
# First call (cold — reads from disk):
time docker exec age_gds_test psql -U postgres -c \
    "SELECT age_gds.cache_stats()"

# Second call (warm — from cache):
# Both should succeed; warm call should be faster or equal
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] Extension builds cleanly with session cache sources
- [ ] **NetworKit symbols still present** (regression check):
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```
- [ ] `cache_stats()` returns `(0, 0, <limit>)` on a fresh session
- [ ] After loading a graph, `entry_count = 1` and `total_bytes > 0`
- [ ] Second load of same graph does not increase `entry_count` (cache hit)
- [ ] LRU eviction removes least-recently-used entry when over memory limit
- [ ] New session starts with empty cache (`entry_count = 0`)
- [ ] `mmap` load correctly reconstructs `NetworKit::Graph` (verified in
      Stage 5 when algorithms produce correct results)
