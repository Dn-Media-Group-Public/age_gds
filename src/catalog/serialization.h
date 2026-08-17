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
 *   magic           uint64   GDS_MAGIC
 *   version         uint32   GDS_VERSION
 *   flags           uint32   bit0=directed  bit1=weighted
 *   node_count      uint64
 *   edge_count      uint64
 *   content_version uint64   incremented on every graph_reindex (was reserved[0..8])
 *   reserved        uint8[16]                                   (was reserved[8..24])
 *
 * Node table  (node_count × 8 bytes):
 *   graphid[]    int64    AGE graphids, index = nk::node
 *
 * Edge table  (edge_count × 24 bytes):
 *   src          uint64   nk::node index
 *   dst          uint64   nk::node index
 *   weight       float64
 *
 * content_version was carved out of the old 24-byte reserved region. Files
 * written before this field existed have that region zeroed, so they
 * deserialize with content_version == 0 — no format-version bump needed.
 */

void gds_serialize(
    const char             *path,
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    bool                    directed,
    bool                    weighted,
    uint64_t                content_version
);

// Cheap read of just the content_version field (no full deserialize).
uint64_t gds_read_version(const char *path);

struct DeserializedGraph {
    NetworKit::Graph graph;
    NodeIdMap        id_map;
    bool             directed;
    bool             weighted;
    uint64_t         content_version = 0;
};

DeserializedGraph gds_deserialize(const char *path);
