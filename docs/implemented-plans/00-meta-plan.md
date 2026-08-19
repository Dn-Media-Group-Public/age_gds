# Meta-Plan — Execution Order

Execute stages 1–8 in order. Each stage is complete only when all its tests
pass inside Docker. Commit to git after each stage passes.

---

## Prerequisites (do once, before stage 1)

```bash
# Clone submodule
git submodule update --init --recursive

# Pull Docker image
docker pull apache/age:latest
```

---

## Workflow (repeat for every stage)

**Use a subagent for each stage** to avoid exhausting the context window.
Launch a fresh subagent per stage; pass it the stage plan file path and the
workflow steps below. The parent agent only tracks stage completion and moves
to the next stage.

```
1. Read the stage plan file
2. Implement the code exactly as specified — see Dependency Rule below
3. Build inside Docker container (see build blocks per stage; includes submodule init)
4. From Stage 3 onward: verify NetworKit is linked before running tests:
       nm build/age_gds.so | grep -c '_ZN10NetworKit'
   This must print a number > 0. If it prints 0, NetworKit is not linked.
   Fix the build — do NOT advance to tests.
5. Run all tests for that stage inside Docker container
6. All tests must pass — fix failures before continuing
7. Move the stage plan to docs/implemented-plans/
8. Commit to git
9. Move to next stage
```

### Dependency Rule

This codebase is a thin PostgreSQL glue layer over NetworKit. It does not
contain graph algorithm implementations.

**Do not stub, fake, or reimplement any external library named in the plan.**
Specifically:
- Do not write `namespace nk { using node = uint64_t; }` or any similar
  local alias that hides the real NetworKit dependency.
- Do not implement PageRank, BFS, Dijkstra, Louvain, or any other algorithm
  yourself. Use the NetworKit class specified in the plan.
- Do not replace `NetworKit::Graph` with a plain struct or adjacency list.

If a dependency is unavailable, fails to build, or the required NetworKit
API cannot be found, **stop and report the problem**. Do not work around it.

---

## Stage Execution

### Stage 1 — Extension Skeleton

Plan: `docs/plans/01-extension-skeleton.md`

```bash
# Start container
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

# Build
docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake -B build -DCMAKE_BUILD_TYPE=Debug &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Test
docker exec age_gds_test psql -U postgres -c "CREATE EXTENSION age_gds"
docker exec age_gds_test psql -U postgres -c "SELECT age_gds.version()"
docker exec age_gds_test psql -U postgres -c "SHOW age_gds.session_cache_mb"
docker exec age_gds_test psql -U postgres -c "SHOW age_gds.data_dir"
docker exec age_gds_test psql -U postgres -c "DROP EXTENSION age_gds"

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 1: extension skeleton, _PG_init, GUC registration"
```

---

### Stage 2 — Catalog Persistence

Plan: `docs/plans/02-catalog-persistence.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Run all tests from 02-catalog-persistence.md test block
docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/02-catalog-persistence.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 2: catalog table, .bin serialization, graph_project/drop/list"
```

---

### Stage 3 — AGE→NetworKit Bridge

Plan: `docs/plans/03-age-networkit-bridge.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

# Seed AGE test data, then run tests
docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/03-age-bridge.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 3: AGE→NetworKit bridge, node ID map, real graph_project"
```

---

### Stage 4 — Session LRU Cache and mmap

Plan: `docs/plans/04-session-cache-mmap.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/04-session-cache.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 4: per-session LRU cache, mmap .bin loader, cache_stats()"
```

---

### Stage 5 — Config Parser, SRF Helper, PageRank

Plan: `docs/plans/05-config-srf-pagerank.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/05-pagerank.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 5: config parser, SRF helper, pageRank_stream/stats"
```

---

### Stage 6 — Remaining Algorithms

Plan: `docs/plans/06-algorithms.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/06-algorithms.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 6: centrality, community detection, path finding algorithms"
```

---

### Stage 7 — Anonymous Execution

Plan: `docs/plans/07-anonymous-execution.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/07-anonymous.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 7: anonymous query-scoped execution without persistent projection"
```

---

### Stage 8 — Write-Back, Link Prediction, Embeddings, Staleness

Plan: `docs/plans/08-writeback-linkpred-embeddings-staleness.md`

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest
docker exec age_gds_test pg_isready -U postgres

docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake --build build -j\$(nproc) &&
    cmake --install build
"

# Verify NetworKit is linked (must print > 0)
docker exec age_gds_test bash -c \
    "nm /workspace/build/age_gds.so | grep -c '_ZN10NetworKit'"

docker exec age_gds_test psql -U postgres \
    -f /workspace/test/sql/03-seed.sql \
    -f /workspace/test/sql/08-writeback.sql \
    -f /workspace/test/sql/08-linkpred.sql \
    -f /workspace/test/sql/08-embeddings.sql \
    -f /workspace/test/sql/08-staleness.sql

docker stop age_gds_test
```

All tests must pass. Then commit:

```bash
git add -A
git commit -m "stage 8: write-back, link prediction, node2vec, graph_is_stale"
```

---

## Test SQL Files

Each stage's tests live in `test/sql/`. Write the SQL from the stage plan's
**Tests** section into the corresponding file before running. The seed data
file `test/sql/03-seed.sql` is shared across stages 3–8 and must create:

- AGE extension and `test_graph`
- 4 Person nodes: Alice, Bob, Carol, Dave
- 5 KNOWS edges as specified in `03-age-networkit-bridge.md`

---

## Failure Policy

- A stage is **not done** until every assertion in its test file passes.
- Fix the code, rebuild inside Docker, rerun tests. Repeat until clean.
- Do not advance to the next stage or commit with failing tests.
- If a later stage breaks an earlier stage's assertions, fix the regression
  before committing.
- From Stage 3 onward, the NetworKit symbol check
  (`nm build/age_gds.so | grep -c '_ZN10NetworKit'`) must print > 0 before
  running any tests. A clean test suite with 0 NetworKit symbols means the
  wrong implementation was committed — do not accept it.
