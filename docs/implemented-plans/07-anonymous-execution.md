# Stage 7 — Anonymous (Query-Scoped) Execution

## Scope

Query-scoped execution: run any algorithm directly against an AGE graph
without creating a persistent projection. Builds `NetworKit::Graph` in
memory, runs the algorithm, returns results, frees memory. No `.bin` file
written, no catalog row, no session cache entry.

This is a degenerate case of the persistent path: the same AGE loader from
Stage 3 is reused, but the resulting graph is discarded after the query
instead of serialized to disk. **No new algorithm code is added in this
stage** — only routing logic that threads the two input modes through the
existing NetworKit-backed algorithm functions.

Builds on Stage 6. No new algorithm logic; this stage threads a new code
path through existing components.

## Design

Each algorithm function already accepts a `graph_name` parameter that
triggers a cache lookup. Anonymous execution is triggered when instead
`age_graph` + `node_query` + `edge_query` are supplied and `graph_name` is
NULL (or absent).

Internal dispatch:

```cpp
// In each algorithm entry point:
const ProjectedGraph *pg;
ProjectedGraph        anon;   // stack/heap; not inserted into cache

if (graph_name != NULL) {
    pg = &session_cache_get(graph_name);   // persistent path
} else {
    // anonymous path
    anon = {
        .graph  = age_load_graph(age_graph, node_query, edge_query,
                                 directed, weighted).graph,
        .id_map = ...,
    };
    pg = &anon;
    // anon freed at end of function scope; no cache insertion, no file write
}
// run algorithm on pg->graph ...
```

`age_load_graph` is called directly; `session_cache_get` is bypassed
entirely.

## SQL Interface

All algorithm functions gain three optional parameters:

```sql
SELECT * FROM age_gds.pageRank_stream(
    age_graph      => 'test_graph',
    node_query     => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query     => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                       RETURN id(a) AS src, id(b) AS dst$$,
    damping_factor => 0.85
);
-- → (nodeId bigint, score float8)
-- No graph_name; anonymous execution
```

Rules:
- `graph_name` and `age_graph` are mutually exclusive; supplying both is
  an ERROR.
- `age_graph` requires both `node_query` and `edge_query`; omitting either
  is an ERROR.
- If neither `graph_name` nor `age_graph` is supplied, raise ERROR.

## Docker Test Environment

Same container and seed data as previous stages.

## Tests

```sql
LOAD 'age';
SET search_path = ag_catalog, "$user", public;
CREATE EXTENSION IF NOT EXISTS age_gds;

-- Anonymous pageRank_stream
SELECT nodeId, score
FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
)
ORDER BY score DESC;
-- expected: 4 rows, scores > 0, sum ≈ 1.0

-- Anonymous execution does NOT populate session cache
SELECT entry_count FROM age_gds.cache_stats();
-- expected: 0

-- Anonymous pageRank_stats
SELECT ranIterations, didConverge
FROM age_gds.pageRank_stats(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
);
-- expected: single row, no NULLs

-- Anonymous wcc_stream (community detection)
SELECT count(*) FROM age_gds.wcc_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
);
-- expected: 4

-- Anonymous bfs_stream (path finding)
-- Get a nodeId from anonymous pageRank first:
SELECT nodeId AS src_id
FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
)
ORDER BY score DESC LIMIT 1 \gset

SELECT count(*) FROM age_gds.bfs_stream(
    age_graph   => 'test_graph',
    node_query  => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query  => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                    RETURN id(a) AS src, id(b) AS dst$$,
    source_node => :src_id
);
-- expected: 4

-- Error: both graph_name and age_graph supplied
SELECT * FROM age_gds.pageRank_stream(
    graph_name => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst$$
);
-- must raise ERROR

-- Error: age_graph without node_query
SELECT * FROM age_gds.pageRank_stream(
    age_graph  => 'test_graph',
    edge_query => $$MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst$$
);
-- must raise ERROR

-- Error: neither graph_name nor age_graph
SELECT * FROM age_gds.pageRank_stream(damping_factor => 0.85);
-- must raise ERROR

-- Persistent path still works (regression)
SELECT age_gds.graph_project(
    name       => 'people',
    age_graph  => 'test_graph',
    node_query => $$MATCH (n:Person) RETURN id(n) AS id$$,
    edge_query => $$MATCH (a:Person)-[r:KNOWS]->(b:Person)
                   RETURN id(a) AS src, id(b) AS dst$$
);
SELECT count(*) FROM age_gds.pageRank_stream(graph_name => 'people');
-- expected: 4

-- Cleanup
SELECT age_gds.graph_drop('people');
DROP EXTENSION age_gds;
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] **NetworKit symbols present** (must not regress):
      ```bash
      nm build/age_gds.so | grep -c '_ZN10NetworKit'
      # must print a number > 0
      ```
- [ ] Anonymous `pageRank_stream` returns correct rows and scores
- [ ] Anonymous execution does NOT insert into session cache
- [ ] Anonymous `pageRank_stats` returns correct summary row
- [ ] At least one community detection algorithm works anonymously (wcc)
- [ ] At least one path finding algorithm works anonymously (bfs)
- [ ] `graph_name` + `age_graph` together raise ERROR
- [ ] `age_graph` without `node_query` raises ERROR
- [ ] Neither `graph_name` nor `age_graph` raises ERROR
- [ ] Persistent `graph_name` path still works after anonymous calls
      (regression)
