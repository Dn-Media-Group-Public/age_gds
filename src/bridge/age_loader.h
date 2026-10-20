// src/bridge/age_loader.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"

struct LoadedGraph {
    NetworKit::Graph graph;
    NodeIdMap        id_map;
};

// node_query / edge_query: both NULL (or empty) = the whole AGE graph, read
// with SQL from <graph>._ag_label_vertex / _ag_label_edge (no Cypher). With
// the whole-graph form, weighted=false never reads edge properties and
// weighted=true uses the edge's numeric `weight` property (else 1.0). Giving
// exactly one of the two queries is an error.
LoadedGraph age_load_graph(
    const char *age_graph_name,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted
);

// (An empty `edge_query` means a whole-graph projection: the property is read
// from the edge label parent with SQL.)
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

// Run, inside an already-open SPI session, the whole-graph read of one vertex
// property: columns (id, property agtype-or-NULL) from <graph>._ag_label_vertex.
// The property name is a bound parameter. Returns the SPI return code.
int age_spi_read_node_property(const char *age_graph_name, const char *property);
