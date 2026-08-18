# Plan: Similarity & link-prediction parity fixes

## Problem / Goal

Close the ✅/⚠️-closable gaps in `docs/neo4j-gds-parity-report.md` §4
(Similarity) and §5 (Link prediction), plus the closable part of Filtered
Node Similarity from §7. Depends on `docs/plans/00-weight-plumbing.md` for
weighted-Jaccard items.

Excluded (❌, out of scope): `knn_stream`'s true property-vector KNN
semantics and approximate-NN internals (`sampleRate`/`deltaThreshold`/
`randomSeed`) — genuine NetworKit gap, tracked in
`docs/neo4j-gds-parity-external-libs.md`. Filtered KNN's NN-descent-specific
params (`maxIterations`/`randomJoins`/`initialSampler`/`perturbationRate`) —
same gap.

## Design

1. **`nodeSimilarity_stream`**: add `similarity_cutoff float8 DEFAULT 1e-42`
   (post-filter: skip pairs below this score), `degree_cutoff integer
   DEFAULT 1` (pre-filter: skip nodes with `Graph::degree(u) < degree_cutoff`
   before the pairwise loop), `top_k integer DEFAULT 10` (bounded per-node
   top-K heap instead of materializing all `O(n²)` pairs — this is the
   headline scalability fix), `top_n integer DEFAULT 0` (`0` = unlimited,
   global row cap on top of top-K), `relationship_weight_property text
   DEFAULT NULL` (weighted-Jaccard: `sum(min(w_u,w_v)) / sum(max(w_u,w_v))`
   over neighbor edge weights — closed-form arithmetic computed directly
   from the graph's weights in the wrapper, not a NetworKit call). Still
   backed by `NetworKit::JaccardIndex` for the unweighted case.
2. **`knn_stream`** (topology-Jaccard interpretation only, current
   behavior — do not attempt the true property-vector semantics here): fix
   `top_k` default from `5` to `10`, add `similarity_cutoff float8 DEFAULT
   0`. Document in `docs/api.md` that this remains a Jaccard-based
   substitution for Neo4j's actual property-vector KNN, cross-referencing
   `docs/neo4j-gds-parity-external-libs.md`.
3. **New: filtered node similarity extension** on `nodeSimilarity_stream`
   (rather than a separate function, since the report shows it as the same
   base config plus filters): add `upper_degree_cutoff bigint DEFAULT
   9223372036854775807`, `bottom_k integer DEFAULT 10`, `bottom_n integer
   DEFAULT 0` (same bounded-heap pattern as `top_k`/`top_n`, inverted
   comparator for "keep lowest-scoring"), `source_node_filter bigint[]
   DEFAULT NULL`, `target_node_filter bigint[] DEFAULT NULL` (pre-filter
   restricting which pairs are considered), `use_components boolean DEFAULT
   false` (run `NetworKit::ConnectedComponents` first, only compare pairs
   within the same component), `similarity_metric text DEFAULT 'JACCARD'`
   (`'JACCARD'|'OVERLAP'|'COSINE'` — `OVERLAP` = `|A∩B|/min(|A|,|B|)`,
   `COSINE` = cosine of 0/1 membership vectors, both closed-form set
   arithmetic, no new NetworKit class). Confirm with the user whether this
   should be one function with all filters (simplest, matches "same base
   config plus filters" framing) or Neo4j's separate `_filtered` naming
   convention — default to extending the existing function unless
   `docs/api.md`'s existing naming conventions suggest otherwise.
4. **Link prediction functions** (`adamicAdar`, `commonNeighbors`,
   `preferentialAttachment`, `resourceAllocation`): add a **new** single-pair
   scalar function per predictor, e.g. `age_gds.linkprediction_adamicAdar(graph_name,
   node1 bigint, node2 bigint, relationship_query text DEFAULT NULL,
   direction text DEFAULT 'BOTH')` returning a single `float8`, matching
   Neo4j's `gds.alpha.linkprediction.adamicAdar(node1, node2, {...})` shape.
   Implementation: construct the appropriate directed/reversed/undirected
   `Graph` view per `direction`, pass to the predictor's `setGraph()`, call
   the base `LinkPredictor::run(u, v)` single-pair method (not `runAll()`).
   `relationship_query` threads through as `edge_query` already does
   elsewhere. Keep the existing `*_stream` all-pairs functions unchanged
   (both variants are useful for different use cases — do not deprecate the
   streaming form, per user's "extend, don't remove" precedent elsewhere in
   this report).

## Implementation order

1. `nodeSimilarity_stream` core filters: `similarity_cutoff`, `degree_cutoff`,
   `top_k`, `top_n` (biggest scalability win in the report, do first).
2. `nodeSimilarity_stream` weighted-Jaccard (`relationship_weight_property`).
3. `nodeSimilarity_stream` extended filters (`upper_degree_cutoff`,
   `bottom_k`, `bottom_n`, `source_node_filter`, `target_node_filter`,
   `use_components`, `similarity_metric`).
4. `knn_stream` default/cutoff fixes.
5. Link-prediction single-pair scalar functions (four new functions, same
   pattern each — implement `adamicAdar` first as the template, then the
   other three).

## Files to change

| File | Change |
|---|---|
| `src/algorithms/similarity.cpp` | `nodeSimilarity_stream` filters, `knn_stream` fixes |
| `src/algorithms/linkprediction.cpp` | Four new single-pair scalar functions |
| `sql/age_gds--1.0.sql` | New/changed function signatures |
| `docs/api.md` §7, §8 | Update/add function docs |
| `test/sql/` | New/updated test cases, including top-K bound verification |

## Docs to update

- `docs/api.md` §7 (Similarity) and §8 (Link prediction).
- `docs/architecture.md` — note the bounded top-K heap approach for
  `nodeSimilarity_stream` as the fix for its previously-flagged `O(n²)`
  scalability concern.
