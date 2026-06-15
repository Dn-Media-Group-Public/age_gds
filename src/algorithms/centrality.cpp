// src/algorithms/centrality.cpp
//
// PageRank streaming and stats SQL functions.
//
// Include order: NetworKit (defines ERROR as variadic macro) first, then
// PostgreSQL headers, then restore ERROR to the integer constant 21.

#include <networkit/centrality/PageRank.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <chrono>
#include <stdexcept>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

#include "catalog/session_cache.h"
#include "util/config_parser.h"
#include "util/srf_helper.h"

/* ---------------------------------------------------------------- helpers */

static void get_args(
    FunctionCallInfo fcinfo,
    char           **graph_name_out,
    PageRankConfig  *cfg_out)
{
    *graph_name_out = text_to_cstring(PG_GETARG_TEXT_PP(0));

    bool   has_damping  = !PG_ARGISNULL(1);
    double damping      = has_damping  ? PG_GETARG_FLOAT8(1) : 0.85;
    bool   has_max_iter = !PG_ARGISNULL(2);
    int    max_iter     = has_max_iter ? PG_GETARG_INT32(2)  : 20;
    bool   has_tol      = !PG_ARGISNULL(3);
    double tol          = has_tol      ? PG_GETARG_FLOAT8(3) : 1e-7;
    bool   has_config   = !PG_ARGISNULL(4);
    Datum  config_datum = has_config   ? PG_GETARG_DATUM(4)  : (Datum)0;

    *cfg_out = parse_pagerank_config(
        has_config,   config_datum,
        has_damping,  damping,
        has_max_iter, max_iter,
        has_tol,      tol);
}

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_pagerank_stream);
PG_FUNCTION_INFO_V1(age_gds_pagerank_stats);

/* ---------------------------------------------- pageRank_stream ---------- */

Datum age_gds_pagerank_stream(PG_FUNCTION_ARGS)
{
    char          *graph_name;
    PageRankConfig cfg;
    get_args(fcinfo, &graph_name, &cfg);

    const ProjectedGraph &pg = session_cache_get(graph_name);

    NetworKit::PageRank pr(pg.graph, cfg.damping_factor, cfg.tolerance);
    pr.maxIterations = (NetworKit::count)cfg.max_iterations;

    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("PageRank failed: %s", ex.what())));
    }

    const auto &scores = pr.scores();

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s   = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        int64_t age_id = pg.id_map.resolve(u);
        double  score  = scores[u];

        Datum values[2];
        bool  nulls[2]  = {false, false};
        values[0] = Int64GetDatum(age_id);
        values[1] = Float8GetDatum(score);

        srf_emit(s, values, nulls);
    });

    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- pageRank_stats ----------- */

Datum age_gds_pagerank_stats(PG_FUNCTION_ARGS)
{
    char          *graph_name;
    PageRankConfig cfg;
    get_args(fcinfo, &graph_name, &cfg);

    const ProjectedGraph &pg = session_cache_get(graph_name);

    auto t0 = std::chrono::steady_clock::now();

    NetworKit::PageRank pr(pg.graph, cfg.damping_factor, cfg.tolerance);
    pr.maxIterations = (NetworKit::count)cfg.max_iterations;

    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("PageRank failed: %s", ex.what())));
    }

    auto    t1         = std::chrono::steady_clock::now();
    int64_t compute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             t1 - t0).count();

    const auto       &scores   = pr.scores();
    NetworKit::count  ran_iter = pr.numberOfIterations();
    bool              converged = ran_iter < (NetworKit::count)cfg.max_iterations;

    double min_score = 0.0, max_score = 0.0, mean_score = 0.0;
    uint64_t n = 0;

    pg.graph.forNodes([&](NetworKit::node u) {
        double sc = scores[u];
        if (n == 0) {
            min_score = max_score = sc;
        } else {
            if (sc < min_score) min_score = sc;
            if (sc > max_score) max_score = sc;
        }
        mean_score += sc;
        ++n;
    });
    if (n > 0)
        mean_score /= (double)n;

    Oid types[6] = {INT4OID, BOOLOID, INT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID};
    SRFState s   = srf_begin(fcinfo, 6, types);

    Datum values[6];
    bool  nulls[6] = {false, false, false, false, false, false};
    values[0] = Int32GetDatum((int32_t)ran_iter);
    values[1] = BoolGetDatum(converged);
    values[2] = Int64GetDatum(compute_ms);
    values[3] = Float8GetDatum(min_score);
    values[4] = Float8GetDatum(max_score);
    values[5] = Float8GetDatum(mean_score);

    srf_emit(s, values, nulls);

    return srf_end(s, fcinfo);
}

} /* extern "C" */
