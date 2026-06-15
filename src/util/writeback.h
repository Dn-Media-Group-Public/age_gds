// src/util/writeback.h
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Write float scores back to AGE nodes via Cypher SET.
// age_graph : AGE graph name (schema name in pg)
// property  : Cypher property name to set
// scores    : map from AGE graphid -> score value
void writeback_scores(
    const char *age_graph,
    const char *property,
    const std::unordered_map<int64_t, double> &scores);

// Look up the age_graph name for a named projection from age_gds.graphs.
// Raises ERROR if projection does not exist.
std::string lookup_age_graph(const char *projection_name);
