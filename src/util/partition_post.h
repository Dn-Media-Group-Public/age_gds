// src/util/partition_post.h
#pragma once

#include <networkit/graph/Graph.hpp>
#include <networkit/structures/Partition.hpp>
#include <networkit/Globals.hpp>
#include "bridge/node_id_map.h"

#include <cstddef>
#include <vector>

// Renumber community ids in `p` to a dense 0..k-1 range, in place.
// Thin wrapper over NetworKit::Partition::compact() -- no hand-rolled
// renumbering logic (Partition::compact() already does exactly this).
void consecutive_ids(NetworKit::Partition &p);

// Neo4j GDS `minCommunitySize` semantics: nodes belonging to a community
// smaller than `min_size` are *omitted* from the result set entirely (not
// merged into a neighboring community). Implemented via
// NetworKit::Partition::remove(), which unassigns an element (sets it to
// NetworKit::none). Callers must skip emitting a row for any node `u` for
// which `p.contains(u)` is false after calling this.
//
// Call this *after* any consecutive_ids()/compact() pass on the same
// Partition -- removed entries become NetworKit::none, and compact() run
// afterwards could try to renumber (and effectively resurrect) them.
void filter_min_community_size(NetworKit::Partition &p, size_t min_size);

// Re-run `node_query` (the query recorded for the projection at
// graph_project/graph_index time, exposed on the cached ProjectedGraph) via
// SPI against `age_graph_name`, reading the named node `property`, and
// build a NetworKit::Partition suitable as PLP's `baseClustering` argument
// or as the `zeta` argument to Modularity::getQuality().
//
// Nodes whose property value is NULL/absent are left in their own
// singleton community (never grouped with each other or with unrelated
// property values), so the returned Partition is always a fully valid
// input to a NetworKit community-detection/quality class -- no node is
// ever left unassigned (NetworKit::none).
//
// Distinct property values are mapped to fresh, dense subset ids starting
// at `graph.upperNodeIdBound()` (strictly above every node's default
// singleton id), so:
//   - two nodes that happen to share a default singleton id and an
//     unrelated raw property value can never collide, and
//   - Partition::upperBound()/numberOfSubsets() stay proportional to the
//     graph size rather than to unrelated raw property values (e.g. an AGE
//     `community_id` property containing large arbitrary integers, which
//     would otherwise blow up Modularity::numberOfSubsets()'s internal
//     O(upperBound()) allocation if used directly as a subset id).
//
// Throws std::runtime_error if `node_query` does not follow the documented
// "RETURN id(<alias>) AS id" convention (see docs/architecture.md) --
// callers must catch this and turn it into ereport(ERROR), the same way
// src/util/weight_override.cpp does for reweight_from_property().
NetworKit::Partition read_node_property_as_partition(
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *node_query,
    const char             *property);

// Same query mechanism as read_node_property_as_partition(), but returns a
// raw numeric value per node (indexed by nk::node) instead of grouping
// nodes into a Partition. Used by localClusteringCoefficient_stream's
// triangle_count_property, which needs the actual precomputed count rather
// than a community/grouping id. Nodes without a value default to
// `default_value`.
//
// Throws std::runtime_error under the same condition as
// read_node_property_as_partition().
std::vector<double> read_node_property_as_values(
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *node_query,
    const char             *property,
    double                  default_value = 0.0);
