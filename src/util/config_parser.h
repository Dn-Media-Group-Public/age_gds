// src/util/config_parser.h
#pragma once

extern "C" {
#include "postgres.h"
#include "utils/jsonb.h"
}

struct PageRankConfig {
    double  damping_factor  = 0.85;
    int     max_iterations  = 20;
    double  tolerance       = 1e-7;
};

PageRankConfig parse_pagerank_config(
    bool   has_config,    Datum  config_jsonb,
    bool   has_damping,   double damping_factor,
    bool   has_max_iter,  int    max_iterations,
    bool   has_tolerance, double tolerance
);
