# Plan: Embedding algorithm parity fixes

## Problem / Goal

Close the ✅/⚠️-closable gaps in `docs/neo4j-gds-parity-report.md` §6
(Embeddings) for `node2vec_stream`. Depends on
`docs/plans/00-weight-plumbing.md` for the (unverified) weight param.

Excluded (❌, out of scope): `negativeSamplingRate` — hardcoded inside
NetworKit's `LearnEmbedding.cpp` ("value taken from original word2vec code"),
not exposed as a parameter; would require a NetworKit-side change. FastRP,
GraphSAGE — no NetworKit class, tracked in
`docs/neo4j-gds-parity-external-libs.md` / out of scope entirely (GraphSAGE).

## Design

1. Fix `embedding_dim` default from `64` to `128` to match Neo4j's
   `embeddingDimension` default.
2. Add `relationship_weight_property text DEFAULT NULL`. **Verify first**
   (implementation-time check, not assumed): does `NetworKit::BiasedRandomWalk`
   (used internally by `Node2Vec`) respect graph edge weights in its
   transition probabilities? Read `embedding/Node2Vec.hpp` and whatever
   `BiasedRandomWalk` header it includes. If yes, thread the weight-override
   helper from plan 00 through before constructing `Node2Vec`. If no, drop
   this parameter from the plan and document the gap in `docs/api.md` rather
   than silently ignoring a param the caller thinks is doing something.
3. Add `window_size integer DEFAULT 10` and `iterations integer DEFAULT 1`
   (training epochs) — both are direct constructor arguments of
   `NetworKit::Embedding::learnEmbeddings(walks, nn, dimensions, winSize,
   iterations)` (`networkit/cpp/embedding/LearnEmbedding.hpp`), the actual
   skip-gram trainer `node2vec_stream` already calls internally but doesn't
   expose — this is pure parameter plumbing, no new NetworKit dependency.

## Implementation order

1. `embedding_dim` default fix (one-line).
2. `window_size`/`iterations` plumbing (both already-available constructor
   args — do this before the weight-property investigation since it's
   guaranteed closable).
3. `relationship_weight_property` — investigate `BiasedRandomWalk` weight
   support first; implement only if confirmed, otherwise document as a gap.

## Files to change

| File | Change |
|---|---|
| `src/algorithms/embedding.cpp` | `node2vec_stream` signature + body |
| `sql/age_gds--1.0.sql` | Changed function signature (new optional params, non-breaking) |
| `docs/api.md` §9 | Update `node2vec_stream` docs |
| `test/sql/` | Updated test case exercising `window_size`/`iterations` |

## Docs to update

- `docs/api.md` §9 (Embeddings).
- `docs/neo4j-gds-parity-external-libs.md` — no change expected unless the
  weight-property investigation turns up a hard "not supported," in which
  case add a one-line note there for consistency with how other confirmed
  gaps are tracked.
