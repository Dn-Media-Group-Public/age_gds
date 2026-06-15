// src/catalog/session_cache.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"
#include "bridge/age_loader.h"

#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

struct ProjectedGraph {
    std::string              name;
    NetworKit::Graph         graph;
    NodeIdMap                id_map;
    bool                     directed;
    bool                     weighted;
    size_t                   approx_bytes;
    std::chrono::steady_clock::time_point last_used;

    // Non-copyable (NetworKit::Graph is move-only)
    ProjectedGraph() = default;
    ProjectedGraph(const ProjectedGraph&) = delete;
    ProjectedGraph& operator=(const ProjectedGraph&) = delete;
    ProjectedGraph(ProjectedGraph&&) = default;
    ProjectedGraph& operator=(ProjectedGraph&&) = default;
};

// Get or load a graph from the session cache.
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

// Remove a single entry from cache (call from graph_drop).
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
