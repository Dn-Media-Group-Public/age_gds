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
