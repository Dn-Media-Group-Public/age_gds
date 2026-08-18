// src/bridge/age_loader.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"

struct LoadedGraph {
    NetworKit::Graph graph;
    NodeIdMap        id_map;
};

LoadedGraph age_load_graph(
    const char *age_graph_name,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted
);

// Re-run `edge_query` (the same query used at projection time) against the
// AGE graph `age_graph_name`, but substitute `weight_property` for whatever
// property the original query used to populate its "weight" column, and
// apply the resulting weights to a *weighted copy* of `g` (the cached
// topology is never mutated in place).
//
// Only edges already present in `g` (per `id_map`) are reweighted; the
// query is expected to follow the convention documented in
// docs/architecture.md ("Per-call weight override"): a single relationship
// alias bound in the MATCH clause, with the query ending in
// `RETURN id(a) AS src, id(b) AS dst, <expr> AS weight`.
//
// Throws std::runtime_error if the "AS weight" projection cannot be located
// in `edge_query` (i.e. it does not follow the documented convention).
NetworKit::Graph reweight_from_property(
    const NetworKit::Graph &g,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *edge_query,
    const char             *weight_property
);
