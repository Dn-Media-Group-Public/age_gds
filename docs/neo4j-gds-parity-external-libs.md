# External-library research: Neo4j GDS parity gaps NetworKit cannot cover

This document surveys open-source libraries that could close the gaps identified in
`docs/neo4j-gds-parity-report.md` §8 — features present in Neo4j GDS that NetworKit's
C++ API (checked directly against `third_party/networkit/include/networkit`) does not
provide. This is research only; nothing here has been implemented.

Evaluation criteria (from `CLAUDE.md`): usable from C/C++ in a PostgreSQL C extension
(header-only or a linkable static/shared lib), permissive license preferred (MIT/BSD/
Apache; GPL/LGPL flagged since it affects distribution of the linked `.so`), actively
maintained, and — per the "never implement a graph algorithm from scratch" rule —
delegates the actual algorithm rather than reimplementing it.

---

## 1. Real HITS (Kleinberg authority/hub scores)

HITS's core computation is just power iteration to find the dominant eigenvectors of
`AᵀA` (authority) and `AAᵀ` (hub), where `A` is the adjacency matrix. There is no
widely-maintained standalone "HITS library"; the clean fit is a sparse eigensolver
used to compute exactly that.

- **Best candidate: [Spectra](https://github.com/yixuan/spectra)** — header-only C++
  library for large-scale sparse eigenvalue problems, built on Eigen. MPL 2.0 license.
  Actively maintained (releases through 2024, still receiving maintenance commits).
  Header-only, trivial to vendor. Using `SymEigsSolver`/`GenEigsSolver` on `AᵀA`/`AAᵀ`
  gives the real, distinct Kleinberg scores via power iteration, matching the spec
  exactly.
- **Second candidate: [Eigen](https://eigen.tuxfamily.org/)** itself (MPL2, header-only,
  extremely well-established) — implement the power iteration directly over Eigen
  sparse matrices without Spectra if only the dominant eigenvector is needed and a
  minimal dependency footprint is preferred.
- **Integration risk:** low. Both are header-only, MPL2 (weak copyleft, safe for linking
  into a separately-distributed `.so`), and only touch the adjacency matrix already
  available from the NetworKit `Graph`. The `rdmpage/hubs-and-authorities` repo (raw
  HITS/PageRank in C++) exists but is stale/dependency-heavy (needs the old GTL) — not
  a good fit.

## 2. Delta-stepping SSSP (low priority — performance only)

- **Best candidate: [ucrparlay/Parallel-SSSP](https://github.com/ucrparlay/Parallel-SSSP)**
  — the reference implementation for "Efficient Stepping Algorithms and Implementations
  for Parallel Shortest Paths" (SPAA'21). C++17, supports delta-stepping directly.
  License unclear/unspecified in places — needs checking before use. Maintenance is
  academic-project level (tied to the paper, not a maintained product).
- **Second candidate: [Yowgf/delta-stepping](https://github.com/Yowgf/delta-stepping)**
  — small OpenMP C++ implementation with the bucket-fusion optimization. Low
  maintenance activity, effectively a single-author research artifact.
- **Integration risk:** medium-low effort but low value. Since NetworKit's Dijkstra is
  already correct and this is purely a performance optimization, recommend **not**
  pursuing unless profiling shows SSSP as an actual bottleneck. Neither candidate is a
  "library" in the productized sense — both would need vendoring and adaptation to
  read a NetworKit `Graph` in place.

## 3. FastRP (random-projection node embeddings)

FastRP's algorithm is: build a sparse random projection matrix (Achlioptas-style, very
sparse), then iteratively multiply by the (normalized) adjacency matrix and combine
weighted iterations. There is no maintained, general-purpose "FastRP C++ library" —
the only implementation found is the original research code
([GTmac/FastRP](https://github.com/GTmac/FastRP)), which is Python/Julia, unmaintained
since ~2019, and not structured as a reusable library.

- **Best candidate: [Eigen](https://eigen.tuxfamily.org/)** (sparse matrix module) —
  the linear-algebra primitives FastRP needs (sparse matrix-vector products, random
  sparse projection matrix construction) are exactly what a linear-algebra library
  provides; this is analogous to using NetworKit's own algebraic backend rather than
  hand-rolling BFS/Dijkstra. Header-only, MPL2, extremely well-established, effectively
  a permanent dependency of the C++ scientific ecosystem.
- **Second candidate:** none found that is better-fit; **flag for explicit decision**
  — building FastRP on top of Eigen means the "recipe" (iterate-and-weight-combine)
  would be written in this codebase, which sits close to the "never implement a graph
  algorithm from scratch" line. This should be raised with the user as an exception
  request (linear-algebra composition, not a novel graph algorithm) rather than
  decided unilaterally here.
- **Integration risk:** low technically (Eigen is trivial to vendor/link), but there is
  a genuine policy question about whether composing FastRP from Eigen primitives
  violates the "delegate to an established implementation" rule, since no established
  FastRP implementation exists to delegate to.

## 4. Yen's k-shortest-paths (low priority)

- **Best candidate:** none needed as a new dependency — Yen's algorithm is an outer
  loop around repeated constrained single-source shortest-path calls. The *shortest
  path itself* can still be delegated to NetworKit's `Dijkstra` class (with excluded
  edges/nodes per Yen's spanning-tree removal step); only the outer bookkeeping
  (candidate path priority queue, dedup) needs to be written, which is orchestration,
  not a hand-rolled shortest-path algorithm.
- **Second candidate (if a full reference implementation is preferred):**
  [iszczesniak/yen](https://github.com/iszczesniak/yen) — Yen's algorithm on top of
  Boost Graph Library (BGL). Small, single-purpose, low visible maintenance activity;
  usable mainly as a reference for the orchestration logic rather than a dependency to
  link.
- **Integration risk:** low. This is the one gap where the existing NetworKit
  dependency already covers the hard part; no new library is strictly required.

## 5. Minimum-weight k-spanning-tree (bounded-size spanning tree; niche)

No dedicated open-source library was found for the *k-bounded* (fixed number of edges)
minimum spanning tree variant — this is a less common NP-hard combinatorial problem
that graph libraries generally don't ship as a named algorithm (regular MST via
Kruskal/Prim is universal, but the k-edge-bounded version is not).

- **Best candidate: [LEMON](https://lemon.cs.elte.hu/)** ("Library for Efficient
  Modeling and Optimization in Networks") — C++ template library for combinatorial
  optimization on graphs/networks, from the COIN-OR family. Very permissive (Boost
  Software License). Provides the building blocks (Kruskal, general MIP/LP
  connectors) that a k-spanning-tree approximation could be built from, but does not
  provide the k-bounded variant directly.
- **Second candidate:** model it as a generic combinatorial optimization problem in
  **[Google OR-Tools](https://github.com/google/or-tools)** (Apache-2.0, actively
  maintained, C++ core with a real link library) using its CP-SAT solver — this
  delegates the actual optimization to an established solver rather than hand-rolling
  a k-spanning-tree heuristic.
- **Integration risk:** medium. Given the "niche/low priority" tag, recommend
  deferring; if pursued, OR-Tools is a heavier dependency (its own build system,
  protobuf, abseil) — worth confirming the value before taking it on.

## 6. Steiner tree (plain minimum Steiner tree + prize-collecting Steiner tree)

- **Plain/directed Steiner tree — best candidate:
  [OGDF](https://github.com/ogdf/ogdf)** (Open Graph Drawing Framework) — ships several
  named, citable Steiner tree approximation algorithms (`MinSteinerTreeTakahashi`,
  `MinSteinerTreeZelikovsky`, `MinSteinerTreeRZLoss`, `MinSteinerTreeShore`, etc.) as
  real C++ classes, i.e. exactly the "delegate to an established implementation"
  pattern this project wants. **License caveat: GPL-2/3 with a special linking
  exception** (per `LICENSE.txt`) — this needs a licensing review before adoption,
  since it may impose copyleft obligations on the combined `age_gds.so` beyond what the
  exception covers. Actively maintained (GitHub org, recent commits).
- **Second candidate:** none as clean — most other Steiner tree repos found
  (`gabrielmorete/prize-collecting-steiner-problem`, various course-project repos) are
  single-author, unmaintained, and not structured as libraries.
- **Prize-collecting Steiner tree — best candidate:
  [fraenkel-lab/pcst_fast](https://github.com/fraenkel-lab/pcst_fast)** — dedicated,
  focused C++ implementation of the Goemans-Williamson prize-collecting Steiner
  forest scheme. No dependencies beyond a C++11 compiler. Used as the PCST backend in
  established bioinformatics tools (OmicsIntegrator), which is a positive maintenance/
  adoption signal. License: MIT. Not header-only but a small, self-contained static
  library — easy to vendor.
- **Integration risk:** OGDF (plain Steiner tree) — medium-high risk due to the GPL
  license requiring explicit review; pcst_fast (prize-collecting) — low risk, small
  MIT C++ library, straightforward to link.

## 7 / 8 / 9. Vector search (property-vector KNN, ANN-descent internals) + K-Means + HDBSCAN over node property vectors

These three gaps share a domain (numeric/vector operations on node property vectors,
not graph topology) but split into two different kinds of library:

### 7. Approximate nearest-neighbor / vector similarity search

- **Best candidate: [hnswlib](https://github.com/nmslib/hnswlib)** — header-only C++
  (with Python bindings, unused here) implementation of Hierarchical Navigable Small
  World graphs for ANN search. Apache-2.0. Widely adopted (used inside FAISS, many
  vector DBs) and actively maintained. Ideal fit: no heavy ML-framework dependency,
  trivially embeddable in a C extension, and its incremental/greedy-join construction
  is conceptually close enough to Neo4j's NN-descent-style approximate KNN
  (`sampleRate`, `randomJoins`, etc.) that it can serve both the plain vector-KNN need
  (7a) and, with custom parameters exposed, approximate the tunable-randomized join
  behavior (7b) — though an exact NN-descent implementation (true `randomJoins`/
  `perturbationRate` semantics) is not identical to HNSW and may need supplementary
  logic.
- **Second candidate: [usearch](https://github.com/unum-cloud/usearch)** — single-header
  C++11 library, also HNSW-based, with SIMD optimizations and support for arbitrary
  metrics/quantization. Apache-2.0. Actively maintained (recent 2026 releases per repo
  activity) and already integrated into ClickHouse/DuckDB, a good production-adoption
  signal. Slightly newer/less battle-tested than hnswlib but comparable or better
  performance and a lighter single-file footprint.
- (Annoy is Apache-2.0 too but not header-only and generally superseded in
  performance by HNSW-based libraries; not recommended here.)
- **Integration risk:** low. Both are header-only/single-header, permissively
  licensed, and only need node property vectors already available in Postgres —
  straightforward to wrap as a SQL function returning KNN edges.

### 8. K-Means clustering over node property vectors

- **Best candidate: [mlpack](https://github.com/mlpack/mlpack)** — general-purpose C++
  machine learning library, BSD-3-Clause, actively maintained, ships a solid
  `KMeans` implementation with multiple initialization strategies. Not header-only in
  the strictest sense but a real, linkable C++ library with minimal dependencies
  (Armadillo/Eigen backend).
- **Second candidate: [dlib](http://dlib.net/)** — Boost Software License (effectively
  public-domain-permissive), long-maintained, provides `kkmeans` (kernelized k-means).
  Slightly more general than needed (full ML toolkit) but well-established and easy to
  vendor as header+source.
- **Integration risk:** low-medium. Both are bigger libraries than the single feature
  needed, but cleanly cover just k-means without pulling in GPU/Python dependencies;
  mlpack is the more surgical choice if only clustering primitives are wanted.

### 9. HDBSCAN over node property vectors

- **Best candidate: [wangyiqiu/hdbscan](https://github.com/wangyiqiu/hdbscan)** — "A
  Fast Parallel Algorithm for HDBSCAN*" (SIGMOD 2021), C++ with built-in parallelism,
  from an academic group at MIT/UCR. No Python/JVM requirement for the core C++.
  License: check repo (typically MIT/BSD for this research-code style, but must be
  confirmed before adoption) — this is the most credible/maintained HDBSCAN C++
  implementation found.
- **Second candidate: [rohanmohapatra/hdbscan-cpp](https://github.com/rohanmohapatra/hdbscan-cpp)**
  — small, dependency-free HDBSCAN in C++ using only STL; simpler to vendor but far
  less maintained/tested (single author, old CI) and unparallelized.
- **Integration risk:** medium. Unlike k-means, there is no big, well-known,
  actively-maintained general ML library with HDBSCAN built in (scikit-learn's is
  Python-only); the best options are narrower academic-quality C++ projects, so expect
  more validation/hardening work before trusting this in production compared to
  gaps 7/8.

## 10. SLLPA (Speaker-Listener Label Propagation, overlapping communities)

No actively-maintained, general-purpose C++ library implementing SLPA/SLLPA was
found. The only C++ candidates are small, single-purpose, largely unmaintained
research repos.

- **Best candidate: [pdscott/slpa](https://github.com/pdscott/slpa)** — direct C++
  implementation of Xie & Szymanski's SLPA. License and maintenance status are weak
  (appears to be an unmaintained personal project, no recent activity).
  Usable as a reference implementation to adapt rather than a dependency to link
  confidently.
- **Second candidate:** none of comparable or better quality found (the TigerGraph/
  Neo4j GDS implementations are proprietary and not available as standalone
  libraries).
- **Integration risk:** high relative to other gaps — this is a genuine hole in the
  open-source C++ ecosystem. Recommend flagging this gap back to the user/plan stage
  rather than silently vendoring an unmaintained single-author repo; if pursued,
  budget time to review and likely rewrite substantial parts of `pdscott/slpa` for
  correctness and to interoperate with NetworKit's `Graph`.

## 11. K-1 Coloring (greedy/distributed graph vertex coloring)

- **Best candidate: [Boost Graph Library (BGL)](https://www.boost.org/doc/libs/release/libs/graph/doc/graph_coloring.html)**
  — `boost::sequential_vertex_coloring` is a header-only, well-tested, actively
  maintained (as part of Boost's regular release cadence) greedy graph coloring
  algorithm. Boost Software License (very permissive). This is by far the cleanest
  fit of any gap in this list: a mainstream, ubiquitous C++ library already commonly
  available in build environments, with exactly the needed algorithm.
- **Second candidate: [brrcrites/GraphColoring](https://github.com/brrcrites/GraphColoring)**
  — a small collection of graph coloring algorithms in C++; far less
  established/maintained than BGL, useful only if BGL's specific ordering heuristics
  (largest-first, smallest-last, incidence-degree) turn out to be insufficient.
- **Integration risk:** very low. BGL is header-only and only needs an adjacency
  view of the NetworKit `Graph` to be built and passed in; this is the easiest gap to
  close in the entire list.

## 12. Approximate Maximum k-cut

No dedicated, maintained open-source library implementing max-k-cut via
variable-neighborhood-search/local-search was found — this appears to be an area
covered mostly by academic papers with one-off, non-reusable code, not packaged
libraries.

- **Best candidate: [Google OR-Tools](https://github.com/google/or-tools)** — rather
  than looking for a max-k-cut-specific library, model the problem generically as a
  combinatorial optimization / CP-SAT (or MIP) problem and delegate solving to
  OR-Tools' constraint solver. Apache-2.0, C++ core, very actively maintained
  (Google-backed, frequent releases). This keeps the "delegate to an established
  implementation" property since the actual search/optimization is OR-Tools',
  not hand-rolled local search.
- **Second candidate:** none found as a cleaner algorithm-specific fit;
  alternative would be a heavier general solver like SCIP (ZIB Academic
  License — not permissive, would need commercial licensing considerations) or a
  hand-adapted academic VNS implementation (e.g. from the "Breakout Local Search for
  Max-Cut" line of research), none of which are packaged/maintained libraries.
- **Integration risk:** medium-high. OR-Tools is a large dependency (its own build
  system, Abseil, Protobuf) relative to the size of this feature — worth weighing
  whether this specific gap justifies pulling it in, versus deferring given it's a
  niche combinatorial-optimization feature.

## 13. CELF / influence maximization under independent cascade

No actively-maintained, general-purpose open-source C++ library implementing
CELF/CELF++ influence maximization under the independent-cascade model was found.
All discovered repositories are course-project or single-paper research code.

- **Best candidate: [todo314/td-infmax](https://github.com/todo314/td-infmax)** — C++
  implementation of a scalable influence-maximization algorithm under
  time-varying independent-cascade and linear-threshold models, tied to a specific
  paper. Maintenance activity is low (research-artifact status); license needs
  checking before use.
- **Second candidate:** none of comparable quality; most other repos found
  (`NikhilGupta1997/Influence-Maximization-CELF`, `prasanna08/SNInfluenceMaximization`,
  etc.) are student/course projects, unmaintained, and generally in Python rather
  than C++.
- **Integration risk:** high — this is, along with SLLPA (#10), one of the two gaps
  with no credible production-grade library to delegate to. Recommend treating this
  as a genuine open gap to report back rather than resolve: per the project's "never
  implement a graph algorithm from scratch" rule, and given the absence of a
  trustworthy library, the honest position is to flag this as currently unclosable
  without either (a) accepting a research-quality/unmaintained dependency with
  attendant correctness risk, or (b) an explicit scoping decision to skip this
  feature.

---

## Summary table

| # | Gap | Best library | License | Header-only/link | Risk |
|---|---|---|---|---|---|
| 1 | Real HITS | Spectra (on Eigen) | MPL2 | header-only | low |
| 2 | Delta-stepping SSSP | ucrparlay/Parallel-SSSP | unclear | source | low priority, skip |
| 3 | FastRP | Eigen (composed) | MPL2 | header-only | low tech / policy question |
| 4 | Yen's k-shortest-paths | none needed (NetworKit Dijkstra + orchestration) | — | — | low |
| 5 | Min-weight k-spanning-tree | LEMON / OR-Tools | Boost SL / Apache-2.0 | link lib | medium, low priority |
| 6a | Steiner tree (plain) | OGDF | **GPL-2/3 + exception** | link lib | medium-high (license) |
| 6b | Prize-collecting Steiner tree | pcst_fast | MIT | link lib (small) | low |
| 7 | ANN / vector search | hnswlib (or usearch) | Apache-2.0 | header-only | low |
| 8 | K-Means | mlpack (or dlib) | BSD-3 (or Boost SL) | link lib | low-medium |
| 9 | HDBSCAN | wangyiqiu/hdbscan | check license | source/link | medium |
| 10 | SLLPA | pdscott/slpa | unmaintained | source | high — genuine gap |
| 11 | K-1 Coloring | Boost Graph Library | Boost SL | header-only | very low |
| 12 | Approximate max-k-cut | Google OR-Tools (generic solver) | Apache-2.0 | link lib | medium-high (dependency weight) |
| 13 | CELF influence maximization | td-infmax | check license | source | high — genuine gap |
