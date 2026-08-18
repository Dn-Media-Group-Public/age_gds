# Plan: Centrality algorithm parity fixes

## Problem / Goal

Close the ✅/⚠️-closable gaps in `docs/neo4j-gds-parity-report.md` §1
(Centrality) between `age_gds` and Neo4j GDS, using only NetworKit classes
already vendored plus thin wrapper/post-processing code. Depends on
`docs/plans/00-weight-plumbing.md` for all `relationshipWeightProperty` items.

Excluded (❌ not in NetworKit, out of scope for this plan): PageRank/ArticleRank/
Eigenvector `maxIterations` (no setter in NetworKit), PageRank/ArticleRank/
Eigenvector `sourceNodes` (no teleport-bias mechanism), real HITS (no
Kleinberg HITS class — stays a documented substitution pending
`docs/neo4j-gds-parity-external-libs.md`).

## Design

All changes live in `src/algorithms/centrality.cpp` plus SQL declarations in
`sql/age_gds--1.0.sql`. Each function gets new optional parameters appended
after existing ones (backward compatible — existing callers unaffected)
except where noted.

1. **`pageRank_stream`/`_stats`**: add `relationship_weight_property text
   DEFAULT NULL` (via weight-override helper), `scaler text DEFAULT NULL`
   (`'MinMax'|'Max'|'Mean'|'Log'|'StdScore'`) applied as a post-processing
   pass over the score vector before streaming.
2. **`betweenness_stream`**: add `sampling_size integer DEFAULT NULL`,
   `sampling_seed integer DEFAULT NULL`. If `sampling_size` given, translate
   to `epsilon` (`epsilon = 1/sqrt(sampling_size)`, pick a fixed reasonable
   `delta` e.g. `0.1`) and call `NetworKit::ApproxBetweenness` instead of the
   current exact `Betweenness`; call `Aux::Random::setSeed(sampling_seed)`
   first if seed given. Add `relationship_weight_property`.
3. **`closeness_stream`**: add `use_wasserman_faust boolean DEFAULT false`,
   mapped to `NetworKit::ClosenessVariant::STANDARD` (true) vs `GENERALIZED`
   (false, current behavior) constructor arg.
4. **`degree_stream`**: add `orientation text DEFAULT 'NATURAL'`
   (`'NATURAL'|'REVERSE'|'UNDIRECTED'`) mapped to `DegreeCentrality(G,
   normalized, outDeg=...)` — `UNDIRECTED` sums in+out via two passes or
   `Graph::degree()` on an undirected-loaded graph. Add
   `relationship_weight_property`; when set, use `Graph::weightedDegree()`/
   `weightedDegreeIn()` directly instead of `DegreeCentrality` (no algorithm
   needed for the weighted case).
5. **`eigenvector_stream`**: add `tolerance float8 DEFAULT 1e-7` (already a
   real constructor param, just not plumbed), `relationship_weight_property`,
   `scaler text DEFAULT NULL` (same scaler pass as PageRank — factor the
   scaler logic into one shared helper, see below).
6. **`articleRank_stream`**: stop hardcoding damping/tolerance; add
   `damping_factor float8 DEFAULT 0.85`, `tolerance float8 DEFAULT 1e-7` (fix
   wrong `1e-6` default to match Neo4j), `relationship_weight_property`,
   `scaler text DEFAULT NULL`.
7. **`articulationPoints_stream`**: extend result shape with
   `resulting_components` — for each articulation point, build a graph copy
   with that node removed, run `NetworKit::ConnectedComponents`, reduce to
   `(count, min, max)` component sizes, return as a composite/row type or
   three extra columns `resulting_component_count int`, `resulting_component_min
   int`, `resulting_component_max int`.

**Shared scaler helper**: add `src/util/scaler.h`/`.cpp` with
`apply_scaler(std::vector<double> &scores, const std::string &scaler)`
implementing `MinMax`, `Max`, `Mean`, `Log`, `StdScore` as one-line arithmetic
reductions. Used by `pageRank_stream`, `eigenvector_stream`, `articleRank_stream`.

## Implementation order

1. `src/util/scaler.h`/`.cpp` (shared, no dependencies).
2. `degree_stream` (orientation + weighted degree — no weight-plumbing
   dependency since it uses `Graph` methods directly, good first item).
3. `closeness_stream` (`use_wasserman_faust` — trivial enum switch).
4. `pageRank_stream`/`_stats` (scaler + weight property).
5. `eigenvector_stream` (tolerance + scaler + weight property).
6. `articleRank_stream` (damping/tolerance/scaler/weight property).
7. `betweenness_stream` (sampling — largest single change, new NetworKit class).
8. `articulationPoints_stream` (resulting_components — result-shape change).

## Files to change

| File | Change |
|---|---|
| `src/util/scaler.h`/`.cpp` (new) | Shared post-processing scaler pass |
| `src/algorithms/centrality.cpp` | All eight function bodies above |
| `sql/age_gds--1.0.sql` | New/changed function signatures |
| `docs/api.md` §4 | Update all eight function docs with new params |
| `test/sql/` | New/updated test cases per function |

## Docs to update

- `docs/api.md` §4 (Centrality algorithms) — update each function's
  parameter table and add "See also" links to this plan once implemented.
- `docs/architecture.md` — note `ApproxBetweenness` as a new NetworKit
  dependency alongside existing `Betweenness`, and the shared scaler helper.
