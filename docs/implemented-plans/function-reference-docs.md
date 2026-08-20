# Per-theme function reference docs (SQL + Cypher variants)

## Problem / Goal

`docs/api.md` is a single 2000+ line file documenting all 63 SQL-callable
`age_gds` functions, grouped into 10 numbered sections (Projection
management, Session cache, Utility, Centrality, Community detection,
Path-finding, Similarity, Link prediction, Embeddings, Write-back). It's
grown unwieldy, and it only documents the SQL calling convention.

`docs/plans/cypher-callable-interface.md` will give every function a second,
Cypher-callable `(agtype, agtype)` overload. Once implemented, each function
has two forms that belong next to each other, not in two different docs.

This plan replaces `docs/api.md` with one markdown file per theme under a
new `docs/reference/` subdirectory, each documenting both the SQL and
Cypher-callable form of every function in that theme side by side.

**This plan assumes `docs/plans/cypher-callable-interface.md` has already
been implemented.** It must not be executed before that plan lands — the
Cypher signatures, config-key names, and output shapes documented here come
from that plan's design and from the real function signatures once built,
not invented independently.

## Design

### Directory layout

```
docs/reference/
  README.md                 -- index: intro, common conventions, catalog
                                tables, GUC settings, links to every
                                theme file
  projection-management.md
  session-cache.md
  utility.md
  centrality.md
  community-detection.md
  path-finding.md
  similarity.md
  link-prediction.md
  embeddings.md
  write-back.md
```

One file per existing `docs/api.md` numbered section — same function
assignment, same boundaries, just split out. `docs/api.md` is deleted;
`README.md` (root) and any other doc that links to it is repointed to
`docs/reference/README.md`.

### `docs/reference/README.md` (index)

Carries everything in `docs/api.md` that isn't a per-function entry:

- Short intro (what this reference covers, how SQL vs Cypher forms relate)
- "Common parameter conventions" section (graph source, weight override,
  node-property read, nodeId values, unreachable nodes) — unchanged from
  `docs/api.md` lines 5–114
- Catalog tables (`age_gds.graphs`, `age_gds.index_triggers`)
- GUC settings
- A table of contents linking to each theme file

### Per-function entry format

Each function gets one `###` entry per theme file, containing both forms:

```markdown
### `age_gds.<name>`

<one-line description>

**SQL**

​```sql
age_gds.<name>(<sql signature>) RETURNS <sql return type>
​```

<SQL param table / notes, same content as current docs/api.md entry>

**Cypher**

​```cypher
UNWIND age_gds.<name>(graphName, config) AS row
RETURN row.<field1>, row.<field2>
​```

- `graphName`: <named-graph or NULL-for-anonymous, per cypher-callable-interface.md>
- `config` keys: <camelCase key> — <meaning, default>
- Output: <array-of-maps | single map>, fields: <...>

<Any deliberate discrepancy vs SQL form or vs Neo4j GDS, e.g. bfs_stream's
omitted `path` field, called out explicitly with a reason.>
```

For the 4 single-pair `linkprediction_*` functions (already scalar in SQL),
the Cypher form is a plain scalar overload — no `UNWIND`/array packing;
document it as a direct expression instead.

Catalog functions (`graph_project`, `graph_drop`, `graph_list`,
`graph_exists`, `graph_reindex`, `cache_stats`, `cache_load`,
`graph_is_stale`) follow the same two-form format in
`projection-management.md` / `session-cache.md`. `age_gds.version` has no
Cypher overload per the other plan's implementation-order note (used only
to prove the shared plumbing) — state that explicitly rather than
fabricating one.

### Cross-linking

Each theme file's entries reference `docs/architecture.md`'s Cypher-calling
convention section (added by `cypher-callable-interface.md`) once instead
of repeating the `agtype`/`UNWIND`-vs-`YIELD` rationale per function.

## Implementation order

1. Confirm `cypher-callable-interface.md` has landed: every function has its
   `(agtype, agtype)` overload in `sql/age_gds--1.0.sql`, and
   `docs/architecture.md` has the Cypher-calling-convention section it
   promised.
2. Create `docs/reference/README.md` — move the shared front/back matter
   from `docs/api.md` (conventions, catalog tables, GUC settings), write the
   intro and table of contents.
3. Create the 10 theme files, one at a time, in this order (small/simple
   first, to settle the entry format before the long files):
   `utility.md` → `session-cache.md` → `write-back.md` →
   `projection-management.md` → `similarity.md` → `link-prediction.md` →
   `embeddings.md` → `centrality.md` → `community-detection.md` →
   `path-finding.md`.
   For each: port the existing SQL entry content from `docs/api.md`
   unchanged, then add the Cypher form using that function's actual
   implemented signature/config keys/output shape.
4. Delete `docs/api.md`.
5. Update `README.md` (repo root) "Documentation" section to link
   `docs/reference/README.md` instead of `docs/api.md`.
6. Grep the repo for remaining `docs/api.md` references
   (`docs/user-guide.md`, `docs/architecture.md`,
   `docs/neo4j-gds-parity-report.md`, etc.) and repoint each to the specific
   new theme file it actually concerns, or to `docs/reference/README.md` for
   general references.

## Files to change

| File | Change |
|---|---|
| `docs/reference/README.md` | New: intro, conventions, catalog tables, GUC settings, TOC |
| `docs/reference/projection-management.md` | New: `graph_index`, `graph_reindex`, `graph_project`, `graph_drop`, `graph_list`, `graph_exists`, `graph_is_stale` — SQL + Cypher |
| `docs/reference/session-cache.md` | New: `cache_stats`, `cache_load` — SQL + Cypher |
| `docs/reference/utility.md` | New: `version` — SQL only (no Cypher overload) |
| `docs/reference/centrality.md` | New: PageRank, Betweenness, Closeness, Harmonic, Degree, Eigenvector, HITS, ArticleRank, Articulation Points — SQL + Cypher |
| `docs/reference/community-detection.md` | New: Louvain, Leiden, Label Propagation, WCC, SCC, k-Core, Triangle Count, Local Clustering Coefficient, Modularity, Conductance, Clique Counting — SQL + Cypher |
| `docs/reference/path-finding.md` | New: BFS, Dijkstra, Delta-stepping, Bellman-Ford, DFS, Random Walk, Spanning Tree, All-Pairs SP, A*, Max Flow, Min-Cost Max Flow, Topological Sort, Longest Path, Bridges — SQL + Cypher |
| `docs/reference/similarity.md` | New: Node Similarity, KNN — SQL + Cypher |
| `docs/reference/link-prediction.md` | New: Adamic-Adar, Common Neighbors, Preferential Attachment, Resource Allocation (stream + scalar forms) — SQL + Cypher |
| `docs/reference/embeddings.md` | New: Node2Vec — SQL + Cypher |
| `docs/reference/write-back.md` | New: `pageRank_write`, `betweenness_write` — SQL + Cypher |
| `docs/api.md` | Deleted |
| `README.md` | Repoint "SQL API Reference" link to `docs/reference/README.md` |
| `docs/user-guide.md`, `docs/architecture.md`, `docs/neo4j-gds-parity-report.md` | Repoint any `docs/api.md` links to the relevant new file |

## Docs to update

This plan *is* the doc restructuring — no separate doc-update step beyond
the cross-link repointing listed above.
