// src/algorithms/flow_common.h
//
// Shared graph-augmentation helpers for the flow-family wrappers
// (maxFlow_stats, minCostMaxFlow_stats) in pathfinding.cpp. Both need to
// reduce a multi-source/multi-sink and/or node-capacity-constrained flow
// problem down to the single-source/single-sink, edge-capacitated shape
// that NetworKit's flow classes (EdmondsKarp, Dinic,
// SuccessiveShortestPathMinCostFlow) expect -- these are graph
// *transformations*, not algorithms, so they belong here rather than in a
// hand-written flow implementation.
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <vector>

// Result of augment_super_source_sink(): the augmented graph plus the node
// ids to treat as "the" source/sink when calling a single-source/sink flow
// class on it.
struct AugmentedFlowGraph {
    NetworKit::Graph graph;
    NetworKit::node  source;
    NetworKit::node  sink;
};

// Builds a copy of `g` (must already be directed) with a super-source node
// wired to every node in `sources` and a super-sink node wired from every
// node in `sinks`, using a capacity large enough to never bottleneck a real
// path (2x the sum of all real edge weights in `g`, or 1.0 if `g` has no
// edges).
//
// If `sources` (respectively `sinks`) has exactly one element, no virtual
// node is introduced for that side and the single element is used directly
// -- this keeps the common single-source/single-sink call from being
// perturbed by an unnecessary extra hop.
//
// The returned graph always has its edges freshly indexed
// (`indexEdges(true)` was called), since node/edge ids change for graphs
// that receive a virtual node.
AugmentedFlowGraph augment_super_source_sink(
    const NetworKit::Graph            &g,
    const std::vector<NetworKit::node> &sources,
    const std::vector<NetworKit::node> &sinks);

// (in_node, out_node) pair a node was split into by split_node_capacities().
struct NodeSplit {
    NetworKit::node in_node;
    NetworKit::node out_node;
};

// Node-splitting transform for node-capacity-constrained flow: every node
// `u` (0..g.upperNodeIdBound()-1) becomes two nodes `u_in`/`u_out` joined by
// an edge of weight `node_capacity[u]` (nodes with no entry in
// `node_capacity`, or a non-positive entry, get an effectively-unconstrained
// edge weight of `std::numeric_limits<double>::max() / 4`, safely larger
// than any real capacity but finite -- EdmondsKarp's arithmetic produces NaN
// on true `infinity()`). Every original edge `(u, v, w)` becomes
// `(u_out, v_in, w)`.
//
// `g` must be directed; `split_out` is resized to `g.upperNodeIdBound()` and
// filled with each original node's `(in_node, out_node)` pair -- callers
// address `u` as a flow source via `split_out[u].out_node` and as a flow
// sink via `split_out[u].in_node`.
NetworKit::Graph split_node_capacities(
    const NetworKit::Graph    &g,
    const std::vector<double> &node_capacity,
    std::vector<NodeSplit>    &split_out);
