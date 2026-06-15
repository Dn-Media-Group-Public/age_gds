# AGENTS.md

## Project identity

`age_gds` is a **thin glue layer** between Apache AGE (graph storage in
PostgreSQL) and NetworKit (C++ graph-data-science library). It is not a
graph-data-science library. Its job:

1. Project an AGE Cypher graph into a `NetworKit::Graph`.
2. Call a NetworKit algorithm on that graph.
3. Stream results back to the SQL caller.

## Non-negotiable constraints

**Never implement a graph algorithm from scratch.** Every algorithm in this
codebase must delegate to a NetworKit class. If NetworKit does not provide a
suitable class for something required by a plan, stop and report the gap
rather than writing your own implementation.

**Never stub or fake NetworKit.** Do not create type aliases, mock classes,
or wrapper types that pretend to be NetworKit while hiding the dependency.
`nk::node` must come from `<networkit/Globals.hpp>`. A local
`namespace nk { using node = uint64_t; }` is forbidden.

**Keep the build honest.** After every build from Stage 3 onward, verify
NetworKit symbols are present in the compiled `.so`:

```bash
nm build/age_gds.so | grep -c '_ZN10NetworKit'
# must print a number > 0
```

If the count is 0, NetworKit is not linked — fix the build before proceeding.
Do not advance to tests or commit until this check passes.

## What belongs here

- PostgreSQL C extension boilerplate (`PG_MODULE_MAGIC`, SPI, SRF, GUC)
- AGE Cypher → `NetworKit::Graph` loading (`age_loader`)
- Binary serialization of graph topology to/from `.bin` files
- Session cache (`nk::Graph` objects keyed by projection name)
- Thin SQL wrapper functions that invoke a NetworKit class and stream results

## What does not belong here

- Hand-written graph traversal (BFS, DFS, Dijkstra, etc.)
- Hand-written centrality computations (PageRank, betweenness, etc.)
- Hand-written community detection (Louvain, label propagation, etc.)
- Any graph-science math that NetworKit already provides

If you find yourself writing a graph algorithm loop, stop — find and use the
NetworKit class instead.

## Dependencies

| Dependency | How obtained |
|---|---|
| NetworKit | `third_party/networkit` git submodule |
| OpenMP | `apt-get install libomp-dev` (NetworKit runtime dep) |
| PostgreSQL headers | `pg_config --includedir-server` |
| Apache AGE headers | same PostgreSQL include path |

NetworKit is not packaged for apt. It must be built from the submodule.
Initialize before the first build:

```bash
git submodule update --init --recursive
```

## CMakeLists.txt requirements

`CMakeLists.txt` must include all of the following. Do not remove or comment
out any of these lines:

```cmake
# Initialize NetworKit submodule before building
add_subdirectory(third_party/networkit EXCLUDE_FROM_ALL)

# Link age_gds against NetworKit
target_link_libraries(age_gds PRIVATE NetworKit)

# OpenMP (required by NetworKit)
find_package(OpenMP REQUIRED)
target_link_libraries(age_gds PRIVATE OpenMP::OpenMP_CXX)
```

## NetworKit include style

Use fully-qualified NetworKit includes. Do not re-declare or alias NetworKit
types locally:

```cpp
// correct
#include <networkit/Globals.hpp>          // nk::node, nk::edgeweight
#include <networkit/graph/Graph.hpp>       // NetworKit::Graph
#include <networkit/centrality/PageRank.hpp>

// forbidden — masks the real dependency
namespace nk { using node = uint64_t; }
```
