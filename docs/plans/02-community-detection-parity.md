# Plan: Community detection algorithm parity fixes

## Problem / Goal

Close the ✅/⚠️-closable gaps in `docs/neo4j-gds-parity-report.md` §2
(Community detection). This is the single largest parameter gap in the whole
report (Louvain: 0 of 8 params exposed). Depends on
`docs/plans/00-weight-plumbing.md`.

Excluded (❌, out of scope): Louvain `tolerance`/`includeIntermediateCommunities`/
`seedProperty`; Leiden `theta`/`tolerance`/`seedProperty`; WCC `seedProperty`.
`maxLevels` is ⚠️ approximated (see below), not an exact port.

## Design

1. **`louvain_stream`**: add `relationship_weight_property text DEFAULT
   NULL`, `gamma float8 DEFAULT 1.0` (`NetworKit::PLM` resolution param, no
   direct Neo4j name but the natural granularity knob), `max_iterations
   integer DEFAULT 10` (→ PLM's `maxIter`), `recurse boolean DEFAULT true`
   (approximates `maxLevels`: `true` = current multilevel behavior, `false` =
   single-level only — document as an approximation, not an exact numeric
   cap), `consecutive_ids boolean DEFAULT false`, `min_community_size integer
   DEFAULT 0` — last two as post-processing over the returned `Partition`
   (shared helper, see below).
2. **`leiden_stream`**: add `relationship_weight_property`, `gamma float8
   DEFAULT 1.0` (direct `ParallelLeiden` param), `iterations integer DEFAULT
   10` (loosely analogous to `maxLevels`, document the semantic difference —
   Neo4j's `maxLevels` is coarsening depth, NetworKit's `iterations` is
   repeated full Leiden runs).
3. **`labelPropagation_stream`**: add `max_iterations integer DEFAULT 10`
   (direct `PLP` constructor param), `relationship_weight_property`,
   `node_weight_property text DEFAULT NULL` (same weight-property read
   mechanism, applied to node weights instead of edge weights — needs a
   small extension: read a named *node* property via SPI into a
   `std::vector<double>` keyed by `nk::node`, used for PLP tie-breaking),
   `seed_property text DEFAULT NULL` (read a named node property into a
   `NetworKit::Partition`, pass as `baseClustering` to `PLP(G, baseClustering,
   theta)`), `consecutive_ids boolean DEFAULT false`.
4. **`wcc_stream`**: add `relationship_weight_property`, `threshold float8
   DEFAULT NULL` (build the loaded graph excluding edges below this weight —
   implemented as a filtered graph copy before running
   `ConnectedComponents`/`WeaklyConnectedComponents`, same pattern as the
   weight-override copy in plan 00), `consecutive_ids boolean DEFAULT false`.
5. **`scc_stream`**: add `consecutive_ids boolean DEFAULT false`.
6. **`triangleCount_stream`**: add `max_degree bigint DEFAULT
   9223372036854775807` (2^63-1) — pre-filter: for each node, if
   `Graph::degree(u) > max_degree`, emit `-1` for that node without running
   the triangle-count class on it (or exclude it from the class's input
   entirely, whichever `NetworKit`'s triangle-count API supports — confirm
   exact class name in `src/algorithms/community.cpp` at implementation
   time).
7. **`localClusteringCoefficient_stream`**: add `triangle_count_property
   text DEFAULT NULL` — when given, read the named node property as
   precomputed triangle counts and compute `2 * triangles(u) / (deg(u) *
   (deg(u) - 1))` directly, bypassing `NetworKit::LocalClusteringCoefficient`
   entirely for that call.
8. **`modularity_stats`**: add `community_property text` (required — read the
   named node property into a `NetworKit::Partition`, pass to
   `Modularity::getQuality(partition, G)` instead of always running an
   internal PLM), `relationship_weight_property`.

**Shared post-processing helpers** in `src/util/partition_post.h`/`.cpp`:
- `consecutive_ids(Partition &p)` — renumber community ids to a dense
  `0..k-1` range.
- `filter_min_community_size(Partition &p, size_t min_size)` — drop/merge
  members of communities below threshold (Neo4j semantics: excluded from
  output, not merged — confirm exact behavior against Neo4j docs at
  implementation time, likely "omit from results" not "reassign").
- `read_node_property_as_partition(age_graph_name, node_query, property,
  NodeIdMap&)` — shared by `seedProperty` (Louvain — actually excluded above
  since PLM has no seed constructor; used only by `labelPropagation_stream`'s
  `seed_property`) and `modularity_stats`'s `community_property`.

## Implementation order

1. `src/util/partition_post.h`/`.cpp` (consecutive_ids, min_community_size,
   read_node_property_as_partition — no algorithm dependencies).
2. `scc_stream` (consecutive_ids only — smallest change, validates the
   shared helper).
3. `wcc_stream` (threshold + consecutive_ids + weight property).
4. `modularity_stats` (community_property — "easiest fix in the whole
   report" per the annotation; do this early for a quick win).
5. `triangleCount_stream` (max_degree pre-filter).
6. `localClusteringCoefficient_stream` (triangle_count_property).
7. `labelPropagation_stream` (max_iterations, seed_property, weight
   properties, consecutive_ids — best-covered by NetworKit, do before the
   harder Louvain/Leiden work).
8. `louvain_stream` (gamma, max_iterations, recurse, weight property,
   consecutive_ids, min_community_size).
9. `leiden_stream` (gamma, iterations, weight property).

## Files to change

| File | Change |
|---|---|
| `src/util/partition_post.h`/`.cpp` (new) | Shared partition post-processing |
| `src/algorithms/community.cpp` | All nine function bodies above |
| `sql/age_gds--1.0.sql` | New/changed function signatures |
| `docs/api.md` §5 | Update all nine function docs |
| `test/sql/` | New/updated test cases per function |

## Docs to update

- `docs/api.md` §5 (Community detection algorithms).
- `docs/architecture.md` — document `recurse`/`iterations` as approximations
  of Neo4j's `maxLevels`, not exact ports (important caveat to preserve).
