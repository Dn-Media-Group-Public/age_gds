# Stage 1 — Extension Skeleton

## Scope

Build system, extension skeleton, `_PG_init`, GUC registration. No
algorithms, no graph loading, no catalog. Extension compiles, loads into
PostgreSQL, and exposes one dummy SQL function to prove the C++ entry point
works.

## Files to Create

```
CMakeLists.txt              # finds PG headers, NetworKit submodule, builds .so
Makefile                    # thin cmake wrapper
age_gds.control             # extension metadata
sql/age_gds--1.0.sql        # CREATE SCHEMA age_gds; CREATE FUNCTION age_gds.version()
third_party/networkit/      # git submodule — run: git submodule update --init --recursive
src/age_gds.cpp             # PG_MODULE_MAGIC, _PG_init, GUC registration
```

### CMakeLists.txt — required content

The build file must include all of the following. Do not omit or stub any of
these lines:

```cmake
cmake_minimum_required(VERSION 3.16)
project(age_gds CXX)
set(CMAKE_CXX_STANDARD 17)

# NetworKit submodule
add_subdirectory(third_party/networkit EXCLUDE_FROM_ALL)

# OpenMP (required by NetworKit)
find_package(OpenMP REQUIRED)

# PostgreSQL headers
find_program(PG_CONFIG pg_config REQUIRED)
execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --sharedir
    OUTPUT_VARIABLE PG_SHAREDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

add_library(age_gds MODULE
    src/age_gds.cpp
)

target_include_directories(age_gds PRIVATE
    ${PG_INCLUDEDIR_SERVER}
    ${CMAKE_SOURCE_DIR}/src
)

# Link NetworKit and OpenMP
target_link_libraries(age_gds PRIVATE NetworKit OpenMP::OpenMP_CXX)

set_target_properties(age_gds PROPERTIES PREFIX "")
target_link_options(age_gds PRIVATE -Wl,--allow-shlib-undefined)

install(TARGETS age_gds DESTINATION ${PG_PKGLIBDIR})
install(FILES age_gds.control DESTINATION ${PG_SHAREDIR}/extension)
install(FILES sql/age_gds--1.0.sql DESTINATION ${PG_SHAREDIR}/extension)
```

`add_subdirectory(third_party/networkit EXCLUDE_FROM_ALL)`,
`target_link_libraries(age_gds PRIVATE NetworKit)`, and
`find_package(OpenMP REQUIRED)` are **non-negotiable**. Do not remove them.

### GUCs to register in `_PG_init`

| GUC | Type | Default | Description |
|---|---|---|---|
| `age_gds.session_cache_mb` | int | 512 | Per-session nk::Graph cache size limit |
| `age_gds.data_dir` | string | `$PGDATA/age_gds` | Directory for `.bin` projection files |

### Dummy SQL function

```sql
-- Returns extension version string; proves C++ entry point reachable
CREATE FUNCTION age_gds.version() RETURNS text
    AS 'age_gds', 'age_gds_version'
    LANGUAGE C STRICT;
```

Implement `age_gds_version` in `src/age_gds.cpp` returning a hardcoded
`"0.1.0"` string.

## Docker Test Environment

All testing uses Docker to provide a reproducible PostgreSQL + Apache AGE
environment. No host PostgreSQL installation required.

### Image

Use the official Apache AGE image (ships with PostgreSQL):

```bash
docker pull apache/age:latest
```

### Start container

```bash
docker run --rm -d \
    --name age_gds_test \
    -e POSTGRES_PASSWORD=password \
    -v "$(pwd):/workspace" \
    apache/age:latest

# Wait for PG to be ready
docker exec age_gds_test pg_isready -U postgres
```

### Build inside container

```bash
docker exec age_gds_test bash -c "
    apt-get update -q &&
    apt-get install -y build-essential cmake libomp-dev postgresql-server-dev-16 &&
    cd /workspace &&
    git submodule update --init --recursive &&
    cmake -B build -DCMAKE_BUILD_TYPE=Debug &&
    cmake --build build -j$(nproc) &&
    cmake --install build
"
```

### Run tests

```sql
-- Connect:
docker exec -it age_gds_test psql -U postgres

-- In psql:
CREATE EXTENSION age_gds;
SELECT age_gds.version();          -- must return '0.1.0'
SHOW age_gds.session_cache_mb;     -- must return '512'
SHOW age_gds.data_dir;             -- must return path ending in 'age_gds'
```

Expected output:
```
 version
---------
 0.1.0
(1 row)
```

## Done Criteria

**Testing is mandatory. Stage is not complete until all tests pass.**

- [ ] `git submodule update --init --recursive` completes without error
- [ ] `cmake -B build && cmake --build build` succeeds with no errors
      (NetworKit will be compiled as part of this step — it takes a few minutes)
- [ ] `CREATE EXTENSION age_gds` runs without error in Docker container
- [ ] `SELECT age_gds.version()` returns `'0.1.0'`
- [ ] `SHOW age_gds.session_cache_mb` returns `'512'`
- [ ] `SHOW age_gds.data_dir` returns a valid path
- [ ] `DROP EXTENSION age_gds` runs without error
