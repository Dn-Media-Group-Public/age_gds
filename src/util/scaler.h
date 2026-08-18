// src/util/scaler.h
#pragma once

#include <string>
#include <vector>

// Applies a Neo4j-GDS-style post-processing scaler to a vector of centrality
// scores, in place.
//
// Supported `scaler` values (case-sensitive, matching Neo4j GDS naming):
//   "MinMax"   — (x - min) / (max - min); 0 for all values if max == min.
//   "Max"      — x / max(|x|); 0 for all values if max(|x|) == 0.
//   "Mean"     — x / mean(x); 0 for all values if mean(x) == 0.
//   "Log"      — natural log of x; non-positive inputs map to 0 (log is
//                undefined there) rather than producing NaN/-inf.
//   "StdScore" — (x - mean) / stddev; 0 for all values if stddev == 0.
//
// Empty `scaler` is a no-op (scores left untouched). An unrecognized
// non-empty value throws std::invalid_argument — callers should catch this
// alongside other algorithm exceptions and turn it into a SQL-level error.
void apply_scaler(std::vector<double> &scores, const std::string &scaler);
