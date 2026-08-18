// src/catalog/session_cache.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"
#include "bridge/age_loader.h"
#include "util/pair_hash.h"

#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

struct ProjectedGraph {
    std::string              name;
    NetworKit::Graph         graph;
    NodeIdMap                id_map;
    bool                     directed;
    bool                     weighted;
    size_t                   approx_bytes;
    std::chrono::steady_clock::time_point last_used;

    // AGE graph name and the Cypher edge query used at projection time.
    // Needed by apply_weight_override()/reweight_from_property() to
    // re-run the projection's edge query against a different property for
    // a single call, without re-projecting the whole graph. Empty for
    // graphs projected before this field existed (weight_property override
    // will raise an error in that case; re-run graph_project to populate).
    std::string              age_graph;
    std::string              node_query;
    std::string              edge_query;

    // --- Live index fields (populated only when live=true) ---
    bool     live             = false;

    // --- Incremental (slot/tombstone) storage fields ---
    // Set whenever the catalog row has incremental=true, regardless of
    // whether AGE triggers are installed on top (live).
    bool     incremental      = false;
    std::string nodes_file_path;
    std::string edges_file_path;

    // --- Flat storage field (populated only when incremental=false) ---
    std::string file_path;

    // content_version: version_counter (incremental storage) or
    // gds header content_version (flat storage) as of the last load.
    uint64_t content_version  = 0;

    // (src_nk, dst_nk) -> slot index in edges file; used for O(1) edge lookup
    // in trigger functions that operate on the cached graph.
    std::unordered_map<
        std::pair<NetworKit::node, NetworKit::node>,
        uint64_t,
        PairHash
    > edge_file_index;

    // Non-copyable (NetworKit::Graph is move-only)
    ProjectedGraph() = default;
    ProjectedGraph(const ProjectedGraph&) = delete;
    ProjectedGraph& operator=(const ProjectedGraph&) = delete;
    ProjectedGraph(ProjectedGraph&&) = default;
    ProjectedGraph& operator=(ProjectedGraph&&) = default;
};

// Get or load a graph from the session cache.
// For live indices: checks version_counter on cache hit and reloads if stale.
// Calls ereport(ERROR) if graph not found in catalog.
const ProjectedGraph& session_cache_get(const char *graph_name);

// Stats for cache_stats() SQL function
struct CacheStats {
    int     entry_count;
    int64_t total_bytes;
    int64_t limit_bytes;
};

CacheStats session_cache_stats();

// Register on_proc_exit cleanup hook. Call from _PG_init.
void session_cache_register_exit_hook();

// Remove a single entry from cache (call from graph_drop / trigger invalidation).
void session_cache_invalidate(const char *graph_name);

// Clear entire cache.
void session_cache_clear();

// -------------------------------------------------------------------
// Anonymous / cached graph resolution
// -------------------------------------------------------------------

// GraphHandle owns the graph for anonymous execution, or holds a
// pointer to a cache entry for named projections.
struct GraphHandle {
    const ProjectedGraph          *pg;
    std::optional<ProjectedGraph>  owned;

    GraphHandle() : pg(nullptr) {}
    GraphHandle(const GraphHandle &) = delete;
    GraphHandle &operator=(const GraphHandle &) = delete;
    GraphHandle(GraphHandle &&) = default;
    GraphHandle &operator=(GraphHandle &&) = default;
};

// Resolve a graph from either the session cache (graph_name non-null)
// or an anonymous AGE load (age_graph non-null).
// Exactly one of graph_name / age_graph must be non-null.
// If age_graph is used, node_query and edge_query must also be non-null.
// Raises ERROR on invalid combinations.
GraphHandle get_or_load_graph(
    const char *graph_name,
    const char *age_graph,
    const char *node_query,
    const char *edge_query,
    bool        directed = false,
    bool        weighted = false
);
