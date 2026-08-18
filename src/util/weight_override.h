// src/util/weight_override.h
#pragma once

#include "catalog/session_cache.h"

// Apply a per-call relationship-weight-property override to `gh`, in place.
//
// - `weight_property == nullptr`: no-op, `gh` is left untouched.
// - `gh` is an anonymous graph (age_graph/node_query/edge_query supplied
//   directly): no-op. The anonymous path already satisfies
//   relationshipWeightProperty by writing the desired property into the
//   caller's edge_query "weight" column directly; see docs/api.md.
// - `gh` is a named/cached graph (`graph_name` resolved via
//   session_cache_get): re-runs the projection's stored edge_query against
//   `weight_property` (see reweight_from_property() in age_loader.h/.cpp)
//   and repoints `gh.pg` at a reweighted *copy* of the cached graph, owned
//   by `gh.owned`. The shared session-cache entry is never mutated.
//
// Raises ereport(ERROR) if the named graph's catalog row predates the
// age_graph/edge_query columns being populated for this projection (re-run
// graph_project/graph_index to populate them).
void apply_weight_override(GraphHandle &gh, const char *weight_property);
