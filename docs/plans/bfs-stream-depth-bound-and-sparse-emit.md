# Plan: `bfs_stream` depth-bounded traversal + sparse emission

## Problem / Goal

`age_gds.bfs_stream` (SQL: `age_gds_bfs_stream`, `src/algorithms/pathfinding.cpp`)
and its Cypher-callable sibling (`age_gds_bfs_stream_cypher`, same file) are
the primitive behind the `article-graph` project's query-pipeline
redesign's "Path B" (BFS against a persistent, named projection). Benchmarking
that redesign against a synthetic 5,000-node graph found `bfs_stream` costs
roughly the same (~0.3ms in that case) regardless of `max_depth` or how few
nodes are actually within range of `source_node`.

Root cause, in the `target_nodes IS NULL` branch of `age_gds_bfs_stream`
(lines ~311-328) and the equivalent branch of `age_gds_bfs_stream_cypher`
(lines ~365-378):

```cpp
std::vector<int64_t> dist(pg.graph.upperNodeIdBound(), -1);
NetworKit::Traversal::BFSfrom(pg.graph, src,
    [&](NetworKit::node u, NetworKit::count d) {
        if (max_depth < 0 || (int64_t)d <= max_depth)
            dist[u] = (int64_t)d;
    });

pg.graph.forNodes([&](NetworKit::node u) {
    Datum values[2]; bool nulls[2] = {false, dist[u] < 0};
    values[0] = Int64GetDatum(pg.id_map.resolve(u));
    if (dist[u] >= 0) values[1] = Float8GetDatum((double)dist[u]);
    srf_emit(s, values, nulls);
});
```

Two independent problems:

1. **`max_depth` doesn't bound the traversal, only the recorded distance.**
   `Traversal::BFSfrom` (`third_party/networkit/include/networkit/graph/BFS.hpp`)
   has no depth parameter — its handle callback is informational only; the
   surrounding loop unconditionally calls `G.forNeighborsOf(u, ...)` for
   *every* dequeued node regardless of what the handle does. `max_depth` is
   checked only to decide whether to *record* `dist[u]`; the queue keeps
   expanding until the entire connected component is exhausted. Cost:
   **O(size of the connected component)**, not O(size of the requested
   radius).

2. **The emit loop walks every node in the projected graph**, not just the
   ones actually visited, to preserve the documented contract ("`distance`
   is NULL for nodes unreachable from the source (or beyond `max_depth`,
   when set)" — `docs/reference/path-finding.md`). Cost: **O(total nodes in
   the graph)**, always, regardless of `max_depth` or how small the actual
   result is.

Both costs are invisible on small graphs (the whole thing finishes in under
a millisecond) but mean `bfs_stream` does not get cheaper as `max_depth`
shrinks or as the projected graph's connected components get larger — the
opposite of what a caller doing shallow neighborhood browsing (the common
"click a node, expand 1-2 hops" case) wants. Goal: make `bfs_stream`'s cost
proportional to the size of the actual output (nodes within `max_depth` of
`source_node`), not to the size of the graph or its connected component.

## Design

Per project rule ("Never implement a graph algorithm from scratch... must
delegate to a NetworKit class"), this plan does **not** replace
`NetworKit::Traversal::BFSfrom` with a hand-rolled queue/frontier loop. It
uses `BFSfrom`'s existing callback-based early-termination idiom — already
established in this same file (`age_gds_bfs_stream_cypher`'s `targetNodes`
handling uses a `BfsStopException` thrown from inside the handle to abort
`BFSfrom` early once every target has been visited). BFS visits nodes in
non-decreasing depth order, so throwing once the handle observes
`d > max_depth` is a correct, general early-termination signal: every
remaining node still in the queue has depth `>= d`, so none of them could
ever pass the check either — throwing does not skip over any node that
BFSfrom would otherwise still need to visit.

### Fix 1 — bound the traversal via early-termination exception

In both `age_gds_bfs_stream`'s and `age_gds_bfs_stream_cypher`'s
`target_nodes`-absent branch, wrap the existing `BFSfrom` call: when
`max_depth >= 0` and the handle observes `d > max_depth`, throw the
existing `BfsStopException{}` (currently private to the `_cypher` function;
hoist it to file scope so both functions can use it) instead of just
skipping the `dist[u] = d` assignment. Catch it around the call the same
way `age_gds_bfs_stream_cypher` already does for its target-exhaustion
case.

This changes traversal cost from O(connected component size) to O(nodes
within `max_depth` hops of `source_node`) — a pure performance fix. It does
**not** change any output value: every `dist[u]` that would have been set
by the current code is still set identically; the only difference is that
nodes strictly beyond `max_depth` are never visited at all instead of being
visited-then-discarded. Fully backward compatible, including for
`max_depth = -1` (unbounded — behavior is untouched, since the depth check
never trips).

### Fix 2 — sparse emission (breaking change, per user decision)

Change the emit loop in `age_gds_bfs_stream` from
`pg.graph.forNodes(...)` (one row per node in the graph) to iterating only
the nodes actually visited (`dist[u] >= 0`) — collect visited node ids into
a `std::vector<NetworKit::node>` inside the (now-bounded) `BFSfrom` handle,
the same pattern `age_gds_bfs_stream_cypher` already uses for its
`visited` vector, and emit from that vector instead of scanning the whole
graph.

**This is a breaking change to the documented contract** in
`docs/reference/path-finding.md` ("`distance` is NULL for nodes unreachable
from the source... `source_node` must be a valid AGE graphid present in the
graph" implies one row per graph node today). Per the explicit decision for
this plan: **ship it as a breaking change in place** (this project has no
extension-upgrade-script convention — `age_gds.control`'s
`default_version` stays `'1.0'`, and prior breaking changes in this same
file (`age_gds_shortestpath_dijkstra_stream`'s Neo4j-parity split,
`source_node` → `source_nodes` on `allShortestPaths_dijkstra_stream`) are
marked in place with a `/* BREAKING CHANGE (per plan N) */` comment
referencing this plan, not a versioned SQL migration). Update
`docs/reference/path-finding.md`'s `bfs_stream` entries (both SQL and
Cypher-callable) to state the new contract: only nodes within `max_depth`
(or, when unbounded, only nodes in `source_node`'s connected component) are
returned; `distance` is never NULL in the SQL form's result rows.

The Cypher-callable form (`age_gds_bfs_stream_cypher`) already only ever
returns a `nodeIds` array of visited nodes (no null-padding concept exists
there), so Fix 2 only touches the SQL form, `age_gds_bfs_stream`. Fix 1
(traversal bound) applies to both.

### Net effect

Both fixes together make `age_gds.bfs_stream`'s cost proportional to the
size of the actual requested neighborhood, not to the size of the graph or
its connected component — the property the `article-graph` query pipeline
needs for its "Path B" fast-path neighborhood browsing to actually be
cheap at scale.

## Implementation order

1. Hoist `struct BfsStopException {}` out of the anonymous namespace
   currently local to `age_gds_bfs_stream_cypher` (~line 340) to file scope
   (or a shared header, if another file needs it later), so
   `age_gds_bfs_stream` can reuse it too.
2. `age_gds_bfs_stream` (SQL form): rewrite the `target_nodes IS NULL`
   branch — bounded `BFSfrom` (Fix 1) + visited-vector emission (Fix 2).
3. `age_gds_bfs_stream_cypher`: apply Fix 1 only (bound the existing
   `BFSfrom` call the same way; it already does visited-vector-style
   emission, so Fix 2 is a no-op there).
4. Add/update unit or SQL-level tests exercising: `max_depth` smaller than
   the connected component (assert output row count == exact expected
   radius, not full component); `max_depth = -1` unbounded (assert
   unchanged behavior vs. current tests); a disconnected graph (assert
   nodes outside `source_node`'s component are absent, not null-padded).
5. Update `docs/reference/path-finding.md`'s `bfs_stream` entries to
   describe the new (SQL form) output contract.

## Files to change

| File | Change |
|---|---|
| `src/algorithms/pathfinding.cpp` | `age_gds_bfs_stream`: bounded-BFS + sparse emission (Fixes 1+2). `age_gds_bfs_stream_cypher`: bounded-BFS only (Fix 1). Hoist `BfsStopException` to file scope. |
| `docs/reference/path-finding.md` | Update `bfs_stream` (SQL form) contract description: no more NULL-distance rows; row count == nodes actually within `max_depth`. Mark as breaking change with a reference to this plan. |
| `test/sql/14-pathfinding-parity.sql`, `test/sql/17-cypher-callable-pathfinding.sql`, `test/sql/06-algorithms.sql`, `test/sql/07-anonymous.sql` | Existing `bfs_stream` coverage in these files; add/adjust cases per Implementation order step 4 (expect row-count/output changes wherever a test currently asserts the full-graph NULL-padded shape). |

## Docs to update

- `docs/reference/path-finding.md` — `bfs_stream` SQL-form contract (breaking change note + this plan's reference).
- `docs/architecture.md` — only if it describes `bfs_stream`'s cost model or output shape anywhere; otherwise no change.
- `docs/implemented-plans/index.md` — add this plan once implemented, per the project's plan-workflow convention.
