cmake_minimum_required(VERSION 3.16)
project(age_gds CXX)
set(CMAKE_CXX_STANDARD 20)

# NetworKit submodule — build as static lib so age_gds.so is self-contained
# -fPIC required because we link into a shared module (.so)
set(NETWORKIT_STATIC ON CACHE BOOL "Build static libraries" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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
    src/catalog/serialization.cpp
    src/catalog/graph_project.cpp
    src/catalog/session_cache.cpp
    src/catalog/index_storage.cpp
    src/catalog/graph_index.cpp
    src/bridge/age_loader.cpp
    src/util/config_parser.cpp
    src/util/srf_helper.cpp
    src/util/writeback.cpp
    src/util/weight_override.cpp
    src/util/scaler.cpp
    src/util/partition_post.cpp
    src/algorithms/centrality.cpp
    src/algorithms/community.cpp
    src/algorithms/flow_common.cpp
    src/algorithms/pathfinding.cpp
    src/algorithms/similarity.cpp
    src/algorithms/linkprediction.cpp
    src/algorithms/embedding.cpp
)

target_include_directories(age_gds PRIVATE
    ${PG_INCLUDEDIR_SERVER}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/networkit/include
    ${CMAKE_SOURCE_DIR}/third_party/networkit/extlibs/tlx
    ${CMAKE_SOURCE_DIR}/src/util
    ${CMAKE_SOURCE_DIR}/src/algorithms
    # NetworKit::Embedding::BiasedRandomWalk (real NetworKit code from the
    # pinned submodule, compiled into the `networkit` target we link
    # against) lives beside its own .cpp in networkit/cpp/embedding/ rather
    # than under include/networkit/ -- Node2Vec.hpp (the only public,
    # installed header) doesn't expose the raw walks randomWalk_stream
    # needs. See docs/architecture.md "Path-finding algorithms" section.
    ${CMAKE_SOURCE_DIR}/third_party/networkit/networkit/cpp/embedding
)

# Link NetworKit and OpenMP
target_link_libraries(age_gds PRIVATE networkit OpenMP::OpenMP_CXX)

set_target_properties(age_gds PROPERTIES PREFIX "")
target_link_options(age_gds PRIVATE -Wl,--allow-shlib-undefined)

install(TARGETS age_gds DESTINATION ${PG_PKGLIBDIR})
install(FILES age_gds.control DESTINATION ${PG_SHAREDIR}/extension)
install(FILES sql/age_gds--1.0.sql DESTINATION ${PG_SHAREDIR}/extension)
