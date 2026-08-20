# Embeddings

### `age_gds.node2vec_stream`

**SQL**

```sql
age_gds.node2vec_stream(
    graph_name                    text    DEFAULT NULL,
    embedding_dim                 int     DEFAULT 128,
    walk_length                   int     DEFAULT 80,
    walks_per_node                int     DEFAULT 10,
    p                             float8  DEFAULT 1.0,
    q                             float8  DEFAULT 1.0,
    age_graph                     text    DEFAULT NULL,
    node_query                    text    DEFAULT NULL,
    edge_query                    text    DEFAULT NULL,
    relationship_weight_property  text    DEFAULT NULL,
    window_size                   int     DEFAULT 10,
    iterations                    int     DEFAULT 1
) RETURNS TABLE("nodeId" bigint, embedding float8[])
```

Node2Vec biased random-walk embeddings. Returns one row per node with a
`float8[]` array of length `embedding_dim`.

**Parameters:**
- `embedding_dim` — vector dimensionality. **Default changed from 64 to 128**
  (plan 05) to match Neo4j GDS's `embeddingDimension` default.
- `walk_length` — length of each random walk (L), default 80.
- `walks_per_node` — number of walks per node (N), default 10.
- `p` — return parameter (controls likelihood of revisiting the previous node),
  default 1.0.
- `q` — in-out parameter (controls BFS vs DFS tendency), default 1.0.
- `relationship_weight_property` (plan 05) — optional edge weight property
  used when computing walk transition probabilities. `NetworKit::Embedding::
  BiasedRandomWalk::preprocessNode()` reads edge weights directly (confirmed
  by reading `networkit/cpp/embedding/BiasedRandomWalk.cpp`), so this behaves
  like every other `relationship_weight_property` parameter in this API (see
  [docs/architecture.md's "Per-call weight
  override"](../architecture.md#per-call-weight-override) section) -- named
  graphs get a reweighted private copy, anonymous graphs must bake the weight into
  `edge_query`'s `AS weight` projection instead. **Known limitation shared
  with every other `relationship_weight_property` parameter, not specific to
  `node2vec_stream`:** if the projection's stored `edge_query` already reads
  `AS weight` from a property of the same name being requested (e.g.
  `edge_query` projects `r.weight AS weight` and the caller passes
  `relationship_weight_property => 'weight'`), the rewritten query is
  textually identical to the original and `reweight_from_property()`
  mistakes that for "no `AS weight` projection found," raising an error
  instead of a no-op. Project under a different placeholder expression (e.g.
  `1.0 AS weight`) if the override property may collide with the base
  projection's name.
- `window_size` (plan 05) — skip-gram context window size, default 10.
- `iterations` (plan 05) — skip-gram training epochs, default 1.

Parameters ≤ 0 are silently reset to their defaults.

**NetworKit functions:** `NetworKit::Embedding::BiasedRandomWalk` (walk
generation) and `NetworKit::Embedding::learnEmbeddings` (skip-gram trainer),
called directly rather than through the public `NetworKit::Node2Vec` wrapper.
`Node2Vec::run()` hardcodes `winSize=10, iterations=1` when it calls
`learnEmbeddings()` and does not expose them as parameters (the class is also
`final`, so it cannot be subclassed to override this) -- both are the same
real, vendored NetworKit functions `Node2Vec::run()` itself calls internally,
just re-exposed with configurable `window_size`/`iterations`. This mirrors
the precedent set by `randomWalk_stream` reaching into
`networkit/cpp/embedding/` for `BiasedRandomWalk` (see
[Plan: Path-finding & Flow Algorithm Parity Fixes](../implemented-plans/03-pathfinding-parity.md)).

Excluded, out of scope: `negativeSamplingRate` -- hardcoded inside
`LearnEmbedding.cpp` ("value taken from original word2vec code"), not
exposed as a parameter by NetworKit; would require a NetworKit-side change.
FastRP has no NetworKit class (tracked in
[docs/neo4j-gds-parity-external-libs.md's "3. FastRP" section](../neo4j-gds-parity-external-libs.md#3-fastrp-random-projection-node-embeddings)).
GraphSAGE likewise has no NetworKit class and no dedicated section in that
file.

See also: [Plan: Embedding algorithm parity fixes](../implemented-plans/05-embeddings-parity.md)

**Cypher**

```cypher
UNWIND age_gds.node2vec_stream('proj', {embeddingDim: 128, walkLength: 80}) AS row
RETURN row.nodeId, row.embedding
```

- `graphName`: named or anonymous graph.
- `config` keys: `embeddingDim`, `walkLength`, `walksPerNode`, `p`, `q`,
  `relationshipWeightProperty`, `windowSize`, `iterations`.
- Output: array of `{nodeId, embedding}` (`embedding` is a nested array of
  floats).

---

See also: [docs/reference/README.md](README.md)
