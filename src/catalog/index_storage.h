// src/catalog/index_storage.h
//
// Binary file format and I/O for live graph indices.
//
// Two files per index:
//   <name>.nodes  — header + node records (slot = nk::node)
//   <name>.edges  — header + edge records
//
// All reads/writes go through POSIX pread/pwrite (no mmap in this layer).
// Callers hold the appropriate LWLock before calling mutating functions.
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"
#include "util/pair_hash.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Magic constants
// ---------------------------------------------------------------------------

static constexpr uint64_t INDEX_NODES_MAGIC = 0x4147455F4E4F4445ULL; // "AGE_NODE"
static constexpr uint64_t INDEX_EDGES_MAGIC = 0x4147455F45444745ULL; // "AGE_EDGE"

// ---------------------------------------------------------------------------
// Deserialized result (returned from index_storage_deserialize)
// ---------------------------------------------------------------------------

struct IndexDeserializedGraph {
    NetworKit::Graph graph;
    NodeIdMap        id_map;
    bool             directed  = false;
    bool             weighted  = false;
    uint64_t         version_counter = 0;

    // (src_nk, dst_nk) -> slot index in edges file.
    // Populated during deserialization for O(1) edge lookup.
    std::unordered_map<
        std::pair<NetworKit::node, NetworKit::node>,
        uint64_t,
        PairHash
    > edge_file_index;
};

// ---------------------------------------------------------------------------
// File I/O API
// ---------------------------------------------------------------------------

// Create empty nodes + edges files with headers.
void index_storage_create(
    const char *nodes_path,
    const char *edges_path,
    bool directed,
    bool weighted);

// Write initial content from a fully-loaded graph (compact, no tombstones).
void index_storage_write_initial(
    const char *nodes_path,
    const char *edges_path,
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    bool directed,
    bool weighted);

// Read the current version_counter from the nodes file header.
uint64_t index_storage_read_version(const char *nodes_path);

// Append a new node record.  Returns the slot index (= nk::node for this node).
// Does NOT increment version_counter — caller must call index_storage_bump_version.
uint64_t index_storage_add_node(const char *nodes_path, int64_t graphid);

// Find the slot for graphid.  Returns UINT64_MAX if not found or tombstoned.
uint64_t index_storage_find_node(const char *nodes_path, int64_t graphid);

// Tombstone a node slot (set dead=1, graphid=-1, decrement live_node_count).
// Does NOT bump version_counter.
void index_storage_kill_node(const char *nodes_path, uint64_t slot);

// Append a new edge record.  Returns the slot index.
// Does NOT bump version_counter.
uint64_t index_storage_add_edge(
    const char *edges_path,
    uint64_t src_nk,
    uint64_t dst_nk,
    double   weight);

// Find edge slot for (src_nk, dst_nk).  Returns UINT64_MAX if not found.
uint64_t index_storage_find_edge(
    const char *edges_path,
    uint64_t src_nk,
    uint64_t dst_nk);

// Tombstone an edge slot.  Does NOT bump version_counter.
void index_storage_kill_edge(const char *edges_path, uint64_t slot);

// Overwrite the weight field of an existing (non-tombstoned) edge slot in
// place.  Used by the graph_project(incremental) diff-apply reindex when an
// edge's identity (src_gid, dst_gid) is unchanged but its weight changed.
// Does NOT bump version_counter.
void index_storage_set_edge_weight(
    const char *edges_path,
    uint64_t    slot,
    double      weight);

// Increment version_counter in the nodes file header.
void index_storage_bump_version(const char *nodes_path);

// Overwrite version_counter with an explicit value. Used when rewriting a
// file from scratch (index_storage_write_initial always resets it to 1),
// so the caller can force it past a previously-observed value and guarantee
// every session cache's version check notices the change.
void index_storage_set_version(const char *nodes_path, uint64_t version);

// Deserialize both files into an IndexDeserializedGraph.
// Slot indices in the file are mapped to compact nk::nodes in the result.
IndexDeserializedGraph index_storage_deserialize(
    const char *nodes_path,
    const char *edges_path);

// Compact: read all live records, write new .tmp files (no tombstones),
// then atomically rename over the originals.
// Caller must hold LW_EXCLUSIVE lock.
void index_storage_compact(
    const char *nodes_path,
    const char *edges_path);
