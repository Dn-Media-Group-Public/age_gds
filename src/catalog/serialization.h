// src/catalog/serialization.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>   // NetworKit::node, NetworKit::edgeweight
#include "bridge/node_id_map.h"
#include <cstdint>

static constexpr uint64_t GDS_MAGIC   = 0x4147455F4744530AULL;
static constexpr uint32_t GDS_VERSION = 1;

/*
 * Binary format
 * -------------
 * Header  (64 bytes):
 *   magic        uint64   GDS_MAGIC
 *   version      uint32   GDS_VERSION
 *   flags        uint32   bit0=directed  bit1=weighted
 *   node_count   uint64
 *   edge_count   uint64
 *   reserved     uint8[24]
 *
 * Node table  (node_count × 8 bytes):
 *   graphid[]    int64    AGE graphids, index = nk::node
 *
 * Edge table  (edge_count × 24 bytes):
 *   src          uint64   nk::node index
 *   dst          uint64   nk::node index
 *   weight       float64
 */

void gds_serialize(
    const char             *path,
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    bool                    directed,
    bool                    weighted
);

struct DeserializedGraph {
    NetworKit::Graph graph;
    NodeIdMap        id_map;
    bool             directed;
    bool             weighted;
};

DeserializedGraph gds_deserialize(const char *path);
