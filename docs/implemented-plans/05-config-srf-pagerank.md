# Stage 5 — Config Parser, SRF Helper, and PageRank

## Scope

Infrastructure needed by every algorithm (config parser, SRF helper), then
the first working algorithm: `pageRank`. Exposes `pageRank_stream`,
`pageRank_stats`, and validates the end-to-end path from SQL call → cache
load → NetworKit → result rows.

**This stage must use `NetworKit::PageRank` from
`<networkit/centrality/PageRank.hpp>`. Do not implement a power-iteration
loop yourself.** The glue code calls NetworKit and streams results; it
contains no graph math.

Builds on Stage 4.

## Files to Create

```
src/util/
    config_parser.cpp       # named params + jsonb → typed config structs
    srf_helper.cpp          # set-returning function boilerplate
src/algorithms/
    centrality.cpp          # pageRank_stream, pageRank_stats (PageRank only)
```

## Config Parser

Handles two call styles; named parameters take precedence over JSON keys;
defaults apply last. Uses PostgreSQL's built-in `jsonb` infrastructure
(`JsonbIteratorNext`) — no external JSON library.

```cpp
// src/util/config_parser.h
struct PageRankConfig {
    double  damping_factor  = 0.85;
    int     max_iterations  = 20;
    double  tolerance       = 1e-7;
};

PageRankConfig parse_pagerank_config(
    Datum  config_jsonb,    // may be NULL
    double damping_factor,  // named param; NaN = not supplied
    int    max_iterations,  // -1 = not supplied
    double tolerance        // NaN = not supplied
);
```

## SRF Helper

Thin wrapper around `Tuplestorestate` / `ReturnSetInfo` for set-returning
functions that return multiple rows.

```cpp
// src/util/srf_helper.h
struct SRFState {
    Tuplestorestate *tuplestore;
    TupleDesc        tupdesc;
};

SRFState srf_begin(FunctionCallInfo fcinfo, int nattrs, Oid *types);
void     srf_emit(SRFState &s, Datum *values, bool *nulls);
Datum    srf_end(SRFState &s);
```

## SQL Interface

```sql
-- Stream: one row per node
SELECT * FROM age_gds.pageRank_stream(
    graph_name     => 'myGraph',
    damping_factor => 0.85,
    max_iterations => 20,
    tolerance      => 1e-7
);
-- → (nodeId bigint, score float8)

-- JSON config style
SELECT * FROM age_gds.pageRank_stream(
    graph_name => 'myGraph',
    config     => '{"dampingFactor": 0.85}'
);

-- Stats: aggregate summary only
SELECT * FROM age_gds.pageRank_stats(
    graph_name => 'myGraph'
);
-- → (ranIterations int, didConverge bool, computeMillis bigint,
--    minScore float8, maxScore float8, meanScore float8)
```

`pageRank_stream` returns AGE `graphid` values in `nodeId` (resolved via
`NodeIdMap.resolve(nk_node)`).

NetworKit class: `NetworKit::PageRank` from
`<networkit/centrality/PageRank.hpp>`.

Usage pattern (glue code only — no math):

```cpp
#include <networkit/centrality/PageRank.hpp>

// inside pageRank_stream:
const ProjectedGraph &pg = session_cache_get(graph_name);
NetworKit::PageRank pr(pg.graph, cfg.damping_factor, cfg.tolerance,
                       cfg.max_iterations);
pr.run();
for (nk::node u = 0; u < pg.graph.numberOfNodes(); ++u) {
    int64_t graphid = pg.id_map.resolve(u);
    double  score   = pr.score(u);
    // emit row (graphid, score) via SRF helper
}
```

## Docker Test Environment

Same container. Requires Stage 3 seed data (`test_graph` with Person nodes
and KNOWS edges) and a projected graph named `'people'`.

Seed + project setup (run once before tests):

```bash
docker exec age_gds_test psql -U postgres -f /workspace/test/setup_stage5.sql
```

`test/setup_stage5.sql` should create AGE graph, seed data, and call
`graph_project` (reuse Stage 3 seed SQL).

## Tests

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- (Assume 'people' projection already exists from Stage 3/4 setup)

-- pageRank_stream: basic correctness
SELECT nodeId, score
FROM age_gds.pageRank_stream(graph_name => 'people')
ORDER BY score DESC;
-- expected: 4 rows, all scores > 0, scores sum to approximately 1.0,
--           no NULLs, nodeIds are valid AGE graphids (non-zero int64)

-- Scores are non-negative
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'people')
WHERE score < 0;
-- expected: 0

-- Score sum ≈ 1.0 (within 1e-4)
SELECT abs(sum(score) - 1.0) < 0.0001
FROM age_gds.pageRank_stream(graph_name => 'people');
-- expected: true

-- Named parameter override
SELECT count(*) FROM age_gds.pageRank_stream(
    graph_name     => 'people',
    damping_factor => 0.5,
    max_iterations => 5
);
-- expected: 4 (same row count; just verifying no crash with custom params)

-- JSON config style
SELECT count(*) FROM age_gds.pageRank_stream(
    graph_name => 'people',
    config     => '{"dampingFactor": 0.9, "maxIterations": 10}'
);
-- expected: 4

-- Named param overrides JSON
SELECT count(*) FROM age_gds.pageRank_stream(
    graph_name     => 'people',
    damping_factor => 0.7,
    config         => '{"dampingFactor": 0.99}'
);
-- damping_factor must be 0.7 (named wins); just verify no crash

-- pageRank_stats: correct shape and types
SELECT ranIterations, didConverge, computeMillis, minScore, maxScore, meanScore
FROM age_gds.pageRank_stats(graph_name => 'people');
-- expected: single row; ranIterations > 0; computeMillis >= 0;
--           minScore <= meanScore <= maxScore; all non-NULL

-- Unknown graph raises error
SELECT * FROM age_gds.pageRank_stream(graph_name => 'no_such_graph');
-- must raise ERROR

-- Cache is populated after first call (Stage 4 regression)
SELECT entry_count FROM age_gds.cache_stats();
-- expected: >= 1

-- Cleanup
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] Extension builds with config parser, SRF helper, and centrality sources
- [ ] **NetworKit symbols present** (must not regress):
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```
- [ ] **`NetworKit::PageRank` symbol specifically present**:
      ```bash
      nm build/age_gds.so | c++filt | grep -c 'NetworKit::PageRank'
      # must print a number > 0
      ```
      If this is 0, the implementation is not using NetworKit PageRank.
- [ ] `pageRank_stream` returns one row per node with non-negative scores
- [ ] Score sum is approximately 1.0 (within 1e-4)
- [ ] `nodeId` values are valid AGE graphids (resolvable, non-zero)
- [ ] Named parameter overrides JSON config key for same field
- [ ] `pageRank_stats` returns a single row with correct shape and no NULLs
- [ ] Unknown graph name raises a clear ERROR
- [ ] Session cache is populated after first algorithm call (Stage 4
      cache hit path verified end-to-end)
