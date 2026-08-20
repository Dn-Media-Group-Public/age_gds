// src/algorithms/centrality.cpp
//
// Centrality SQL functions: pageRank, betweenness, closeness, harmonic,
// degree, eigenvector, HITS (via EigenvectorCentrality), articleRank,
// articulationPoints.
//
// Include order: NetworKit first (it defines ERROR as a variadic macro),
// then PostgreSQL headers, then restore ERROR to the integer constant 21.

#include <networkit/centrality/PageRank.hpp>
#include <networkit/centrality/Betweenness.hpp>
#include <networkit/centrality/ApproxBetweenness.hpp>
#include <networkit/centrality/Closeness.hpp>
#include <networkit/centrality/HarmonicCloseness.hpp>
#include <networkit/centrality/DegreeCentrality.hpp>
#include <networkit/centrality/EigenvectorCentrality.hpp>
#include <networkit/components/BiconnectedComponents.hpp>
#include <networkit/components/ConnectedComponents.hpp>
#include <networkit/auxiliary/Random.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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
#include "util/weight_override.h"
#include "util/scaler.h"
#include "util/cypher_agtype.h"
#include "util/writeback.h"

/* ---------------------------------------------------------------- helpers */

// Helper: extract graph (from cache or anonymous) + PageRank config.
// pageRank arg layout:
//   0=graph_name, 1=damping_factor, 2=max_iterations, 3=tolerance,
//   4=config, 5=age_graph, 6=node_query, 7=edge_query,
//   8=weight_property (relationship_weight_property), 9=scaler
static GraphHandle get_args(
    FunctionCallInfo fcinfo,
    PageRankConfig  *cfg_out,
    std::string     *scaler_out)
{
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

    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    const char *nq     = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    const char *eq     = PG_ARGISNULL(7) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(7));
    const char *wprop  = PG_ARGISNULL(8) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(8));

    if (scaler_out)
        *scaler_out = PG_ARGISNULL(9) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(9));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    return gh;
}

// Helper: resolve graph for simple 1-arg functions.
// New layout: 0=graph_name, age_graph_idx=age_graph, +1=node_query, +2=edge_query
static GraphHandle resolve_graph_args(FunctionCallInfo fcinfo, int age_graph_idx)
{
    const char *gname  = PG_ARGISNULL(0)                  ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(age_graph_idx)     ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx));
    const char *nq     = PG_ARGISNULL(age_graph_idx + 1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 1));
    const char *eq     = PG_ARGISNULL(age_graph_idx + 2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 2));
    return get_or_load_graph(gname, agraph, nq, eq);
}

// Applies `scaler` (may be empty => no-op) to `scores`, translating a bad
// scaler name into a SQL-level ERROR instead of a C++ exception.
static void apply_scaler_checked(std::vector<double> &scores, const std::string &scaler)
{
    try {
        apply_scaler(scores, scaler);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s", ex.what())));
    }
}

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_pagerank_stream);
PG_FUNCTION_INFO_V1(age_gds_pagerank_stats);
PG_FUNCTION_INFO_V1(age_gds_betweenness_stream);
PG_FUNCTION_INFO_V1(age_gds_closeness_stream);
PG_FUNCTION_INFO_V1(age_gds_harmoniccentrality_stream);
PG_FUNCTION_INFO_V1(age_gds_degree_stream);
PG_FUNCTION_INFO_V1(age_gds_eigenvector_stream);
PG_FUNCTION_INFO_V1(age_gds_hits_stream);
PG_FUNCTION_INFO_V1(age_gds_articlerank_stream);
PG_FUNCTION_INFO_V1(age_gds_articulationpoints_stream);

/* Shared config-driven PageRank runner for the Cypher wrappers (pageRank_stream/
 * _stats, articleRank_stream reuse the same PageRank class + config keys). */
static void run_pagerank_cypher(const ProjectedGraph &pg, double damping, int max_iter,
                                double tolerance, NetworKit::PageRank **out)
{
    *out = new NetworKit::PageRank(pg.graph, damping, tolerance);
    (*out)->maxIterations = (NetworKit::count)max_iter;
    try { (*out)->run(); }
    catch (const std::exception &ex) {
        delete *out;
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("PageRank failed: %s", ex.what())));
    }
}

/* ---------------------------------------------- pageRank_stream ---------- */

Datum age_gds_pagerank_stream(PG_FUNCTION_ARGS)
{
    PageRankConfig cfg;
    std::string    scaler;
    GraphHandle    gh = get_args(fcinfo, &cfg, &scaler);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PageRank pr(pg.graph, cfg.damping_factor, cfg.tolerance);
    pr.maxIterations = (NetworKit::count)cfg.max_iterations;

    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("PageRank failed: %s", ex.what())));
    }

    std::vector<double> scores = pr.scores();
    apply_scaler_checked(scores, scaler);

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

/* ---------------------------------------------- pageRank_stream (Cypher) - */
/* config: dampingFactor, maxIterations, tolerance, relationshipWeightProperty,
 * scaler */

PG_FUNCTION_INFO_V1(age_gds_pagerank_stream_cypher);

Datum age_gds_pagerank_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"dampingFactor", "maxIterations", "tolerance", "relationshipWeightProperty", "scaler"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double damping = 0.85, tolerance = 1e-7;
    int max_iter = 20;
    cfg.get_double("dampingFactor", damping);
    cfg.get_int("maxIterations", max_iter);
    cfg.get_double("tolerance", tolerance);
    std::string scaler;
    cfg.get_string("scaler", scaler);

    NetworKit::PageRank *pr = nullptr;
    run_pagerank_cypher(pg, damping, max_iter, tolerance, &pr);
    std::vector<double> scores = pr->scores();
    delete pr;
    apply_scaler_checked(scores, scaler);

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- pageRank_stats ----------- */

Datum age_gds_pagerank_stats(PG_FUNCTION_ARGS)
{
    PageRankConfig cfg;
    std::string    scaler;
    GraphHandle    gh = get_args(fcinfo, &cfg, &scaler);
    const ProjectedGraph &pg = *gh.pg;

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

    std::vector<double> scores    = pr.scores();
    NetworKit::count     ran_iter = pr.numberOfIterations();
    bool                 converged = ran_iter < (NetworKit::count)cfg.max_iterations;

    apply_scaler_checked(scores, scaler);

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

/* ---------------------------------------------- pageRank_stats (Cypher) -- */

PG_FUNCTION_INFO_V1(age_gds_pagerank_stats_cypher);

Datum age_gds_pagerank_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"dampingFactor", "maxIterations", "tolerance", "relationshipWeightProperty", "scaler"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double damping = 0.85, tolerance = 1e-7;
    int max_iter = 20;
    cfg.get_double("dampingFactor", damping);
    cfg.get_int("maxIterations", max_iter);
    cfg.get_double("tolerance", tolerance);
    std::string scaler;
    cfg.get_string("scaler", scaler);

    auto t0 = std::chrono::steady_clock::now();
    NetworKit::PageRank *pr = nullptr;
    run_pagerank_cypher(pg, damping, max_iter, tolerance, &pr);
    auto t1 = std::chrono::steady_clock::now();
    int64_t compute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::vector<double> scores = pr->scores();
    NetworKit::count ran_iter = pr->numberOfIterations();
    bool converged = ran_iter < (NetworKit::count)max_iter;
    delete pr;
    apply_scaler_checked(scores, scaler);

    double min_score = 0.0, max_score = 0.0, mean_score = 0.0;
    uint64_t n = 0;
    pg.graph.forNodes([&](NetworKit::node u) {
        double sc = scores[u];
        if (n == 0) min_score = max_score = sc;
        else { if (sc < min_score) min_score = sc; if (sc > max_score) max_score = sc; }
        mean_score += sc;
        ++n;
    });
    if (n > 0) mean_score /= (double)n;

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("ranIterations"); b.add_int64((int64_t)ran_iter);
    b.key("didConverge"); b.add_bool(converged);
    b.key("computeMillis"); b.add_int64(compute_ms);
    b.key("minScore"); b.add_double(min_score);
    b.key("maxScore"); b.add_double(max_score);
    b.key("meanScore"); b.add_double(mean_score);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- betweenness_stream ------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=sampling_size, 5=sampling_seed, 6=relationship_weight_property

Datum age_gds_betweenness_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    bool has_sampling_size = !PG_ARGISNULL(4);
    int  sampling_size     = has_sampling_size ? PG_GETARG_INT32(4) : 0;
    bool has_sampling_seed = !PG_ARGISNULL(5);
    int  sampling_seed     = has_sampling_seed ? PG_GETARG_INT32(5) : 0;
    const char *wprop  = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));

    if (has_sampling_size && sampling_size <= 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("sampling_size must be a positive integer")));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    std::vector<double> scores;

    if (has_sampling_size) {
        if (has_sampling_seed)
            Aux::Random::setSeed((uint64_t)sampling_seed, false);

        double epsilon = 1.0 / std::sqrt((double)sampling_size);
        double delta   = 0.1;

        NetworKit::ApproxBetweenness algo(pg.graph, epsilon, delta);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("ApproxBetweenness failed: %s", ex.what())));
        }
        scores = algo.scores();
    } else {
        NetworKit::Betweenness algo(pg.graph, false);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("Betweenness failed: %s", ex.what())));
        }
        scores = algo.scores();
    }

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- betweenness_stream (Cypher) */
/* config: samplingSize, samplingSeed, relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_betweenness_stream_cypher);

Datum age_gds_betweenness_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"samplingSize", "samplingSeed", "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    int sampling_size = 0, sampling_seed = 0;
    bool has_sampling_size = cfg.get_int("samplingSize", sampling_size);
    bool has_sampling_seed = cfg.get_int("samplingSeed", sampling_seed);
    if (has_sampling_size && sampling_size <= 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.samplingSize must be a positive integer")));

    std::vector<double> scores;
    if (has_sampling_size) {
        if (has_sampling_seed)
            Aux::Random::setSeed((uint64_t)sampling_seed, false);
        double epsilon = 1.0 / std::sqrt((double)sampling_size);
        NetworKit::ApproxBetweenness algo(pg.graph, epsilon, 0.1);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("ApproxBetweenness failed: %s", ex.what())));
        }
        scores = algo.scores();
    } else {
        NetworKit::Betweenness algo(pg.graph, false);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("Betweenness failed: %s", ex.what())));
        }
        scores = algo.scores();
    }

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- closeness_stream --------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=use_wasserman_faust

Datum age_gds_closeness_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    bool use_wasserman_faust = PG_ARGISNULL(4) ? false : PG_GETARG_BOOL(4);

    // STANDARD == Wasserman-Faust variant; GENERALIZED is the previous
    // default (handles disconnected graphs without dividing by zero).
    NetworKit::ClosenessVariant variant = use_wasserman_faust
        ? NetworKit::ClosenessVariant::STANDARD
        : NetworKit::ClosenessVariant::GENERALIZED;

    NetworKit::Closeness algo(pg.graph, false, variant);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Closeness failed: %s", ex.what())));
    }

    const auto &scores = algo.scores();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- closeness_stream (Cypher) */
/* config: useWassermanFaust */

PG_FUNCTION_INFO_V1(age_gds_closeness_stream_cypher);

Datum age_gds_closeness_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"useWassermanFaust"});
    const ProjectedGraph &pg = *gh.pg;

    bool use_wasserman_faust = false;
    cfg.get_bool("useWassermanFaust", use_wasserman_faust);
    NetworKit::ClosenessVariant variant = use_wasserman_faust
        ? NetworKit::ClosenessVariant::STANDARD
        : NetworKit::ClosenessVariant::GENERALIZED;

    NetworKit::Closeness algo(pg.graph, false, variant);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Closeness failed: %s", ex.what())));
    }
    const auto &scores = algo.scores();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- harmonicCentrality_stream  */

Datum age_gds_harmoniccentrality_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::HarmonicCloseness algo(pg.graph, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("HarmonicCloseness failed: %s", ex.what())));
    }

    const auto &scores = algo.scores();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- harmonicCentrality (Cypher) */

PG_FUNCTION_INFO_V1(age_gds_harmoniccentrality_stream_cypher);

Datum age_gds_harmoniccentrality_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::HarmonicCloseness algo(pg.graph, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("HarmonicCloseness failed: %s", ex.what())));
    }
    const auto &scores = algo.scores();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- degree_stream ------------ */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=orientation, 5=relationship_weight_property

enum class DegreeOrientation { NATURAL, REVERSE, UNDIRECTED };

static DegreeOrientation parse_orientation(const char *text)
{
    if (!text)
        return DegreeOrientation::NATURAL;

    std::string s(text);
    if (s == "NATURAL")    return DegreeOrientation::NATURAL;
    if (s == "REVERSE")    return DegreeOrientation::REVERSE;
    if (s == "UNDIRECTED") return DegreeOrientation::UNDIRECTED;

    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("orientation must be one of 'NATURAL', 'REVERSE', "
                           "'UNDIRECTED' (got \"%s\")", text)));
    return DegreeOrientation::NATURAL; // unreachable
}

Datum age_gds_degree_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *orient_txt = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    const char *wprop  = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));

    DegreeOrientation orientation = parse_orientation(orient_txt);

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    std::vector<double> scores(pg.graph.upperNodeIdBound(), 0.0);

    if (wprop) {
        // Weighted case: NetworKit has no weighted-degree algorithm class —
        // Graph::weightedDegree()/weightedDegreeIn() already compute exactly
        // this in O(1)-per-node, so no Centrality subclass is needed here.
        pg.graph.forNodes([&](NetworKit::node u) {
            switch (orientation) {
            case DegreeOrientation::NATURAL:
                scores[u] = pg.graph.weightedDegree(u);
                break;
            case DegreeOrientation::REVERSE:
                scores[u] = pg.graph.weightedDegreeIn(u);
                break;
            case DegreeOrientation::UNDIRECTED:
                scores[u] = pg.graph.isDirected()
                    ? pg.graph.weightedDegree(u) + pg.graph.weightedDegreeIn(u)
                    : pg.graph.weightedDegree(u);
                break;
            }
        });
    } else {
        bool outDeg = (orientation != DegreeOrientation::REVERSE);

        NetworKit::DegreeCentrality algo(pg.graph, false, outDeg);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("DegreeCentrality failed: %s", ex.what())));
        }
        const auto &deg_scores = algo.scores();

        if (orientation == DegreeOrientation::UNDIRECTED && pg.graph.isDirected()) {
            NetworKit::DegreeCentrality algo_in(pg.graph, false, false);
            try { algo_in.run(); }
            catch (const std::exception &ex) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("DegreeCentrality failed: %s", ex.what())));
            }
            const auto &in_scores = algo_in.scores();
            pg.graph.forNodes([&](NetworKit::node u) {
                scores[u] = deg_scores[u] + in_scores[u];
            });
        } else {
            pg.graph.forNodes([&](NetworKit::node u) { scores[u] = deg_scores[u]; });
        }
    }

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- degree_stream (Cypher) --- */
/* config: orientation, relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_degree_stream_cypher);

Datum age_gds_degree_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"orientation", "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::string orient_str = "NATURAL";
    cfg.get_string("orientation", orient_str);
    DegreeOrientation orientation = parse_orientation(orient_str.c_str());

    std::vector<double> scores(pg.graph.upperNodeIdBound(), 0.0);
    if (has_wprop) {
        pg.graph.forNodes([&](NetworKit::node u) {
            switch (orientation) {
            case DegreeOrientation::NATURAL:
                scores[u] = pg.graph.weightedDegree(u);
                break;
            case DegreeOrientation::REVERSE:
                scores[u] = pg.graph.weightedDegreeIn(u);
                break;
            case DegreeOrientation::UNDIRECTED:
                scores[u] = pg.graph.isDirected()
                    ? pg.graph.weightedDegree(u) + pg.graph.weightedDegreeIn(u)
                    : pg.graph.weightedDegree(u);
                break;
            }
        });
    } else {
        bool outDeg = (orientation != DegreeOrientation::REVERSE);
        NetworKit::DegreeCentrality algo(pg.graph, false, outDeg);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("DegreeCentrality failed: %s", ex.what())));
        }
        const auto &deg_scores = algo.scores();
        if (orientation == DegreeOrientation::UNDIRECTED && pg.graph.isDirected()) {
            NetworKit::DegreeCentrality algo_in(pg.graph, false, false);
            try { algo_in.run(); }
            catch (const std::exception &ex) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("DegreeCentrality failed: %s", ex.what())));
            }
            const auto &in_scores = algo_in.scores();
            pg.graph.forNodes([&](NetworKit::node u) { scores[u] = deg_scores[u] + in_scores[u]; });
        } else {
            pg.graph.forNodes([&](NetworKit::node u) { scores[u] = deg_scores[u]; });
        }
    }

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- eigenvector_stream ------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=tolerance, 5=relationship_weight_property, 6=scaler

Datum age_gds_eigenvector_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    double tolerance   = PG_ARGISNULL(4) ? 1e-7 : PG_GETARG_FLOAT8(4);
    const char *wprop  = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    std::string scaler = PG_ARGISNULL(6) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(6));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::EigenvectorCentrality algo(pg.graph, tolerance);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EigenvectorCentrality failed: %s", ex.what())));
    }

    std::vector<double> scores = algo.scores();
    apply_scaler_checked(scores, scaler);

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- eigenvector_stream (Cypher) */
/* config: tolerance, relationshipWeightProperty, scaler */

PG_FUNCTION_INFO_V1(age_gds_eigenvector_stream_cypher);

Datum age_gds_eigenvector_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"tolerance", "relationshipWeightProperty", "scaler"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double tolerance = 1e-7;
    cfg.get_double("tolerance", tolerance);
    std::string scaler;
    cfg.get_string("scaler", scaler);

    NetworKit::EigenvectorCentrality algo(pg.graph, tolerance);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EigenvectorCentrality failed: %s", ex.what())));
    }
    std::vector<double> scores = algo.scores();
    apply_scaler_checked(scores, scaler);

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- hits_stream -------------- */
// NetworKit has no HITS class. Substitute: EigenvectorCentrality scores
// for both hub and auth (equivalent for undirected graphs; for directed
// graphs a proper HITS would use the transposed graph for auth).

Datum age_gds_hits_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::EigenvectorCentrality algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EigenvectorCentrality (HITS substitute) failed: %s", ex.what())));
    }

    const auto &scores = algo.scores();
    // columns: nodeId bigint, auth float8, hub float8
    Oid types[3] = {INT8OID, FLOAT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 3, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double sc = scores[u];
        Datum values[3]; bool nulls[3] = {false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(sc);   // auth
        values[2] = Float8GetDatum(sc);   // hub
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- hits_stream (Cypher) ----- */

PG_FUNCTION_INFO_V1(age_gds_hits_stream_cypher);

Datum age_gds_hits_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::EigenvectorCentrality algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EigenvectorCentrality (HITS substitute) failed: %s", ex.what())));
    }
    const auto &scores = algo.scores();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        double sc = scores[u];
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("auth"); b.add_double(sc);
        b.key("hub"); b.add_double(sc);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- articleRank_stream ------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=damping_factor, 5=tolerance, 6=relationship_weight_property,
//             7=scaler
// NetworKit has no ArticleRank class; PageRank is used with configurable
// damping/tolerance (previously hardcoded to 0.85/1e-6 — 1e-6 did not match
// Neo4j GDS's 1e-7 default, now fixed).

Datum age_gds_articlerank_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    double damping     = PG_ARGISNULL(4) ? 0.85 : PG_GETARG_FLOAT8(4);
    double tolerance   = PG_ARGISNULL(5) ? 1e-7 : PG_GETARG_FLOAT8(5);
    const char *wprop  = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    std::string scaler = PG_ARGISNULL(7) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(7));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PageRank pr(pg.graph, damping, tolerance);
    pr.maxIterations = 20;

    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ArticleRank (PageRank) failed: %s", ex.what())));
    }

    std::vector<double> scores = pr.scores();
    apply_scaler_checked(scores, scaler);

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(scores[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- articleRank_stream (Cypher) */
/* config: dampingFactor, tolerance, relationshipWeightProperty, scaler */

PG_FUNCTION_INFO_V1(age_gds_articlerank_stream_cypher);

Datum age_gds_articlerank_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"dampingFactor", "tolerance", "relationshipWeightProperty", "scaler"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double damping = 0.85, tolerance = 1e-7;
    cfg.get_double("dampingFactor", damping);
    cfg.get_double("tolerance", tolerance);
    std::string scaler;
    cfg.get_string("scaler", scaler);

    NetworKit::PageRank pr(pg.graph, damping, tolerance);
    pr.maxIterations = 20;
    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ArticleRank (PageRank) failed: %s", ex.what())));
    }
    std::vector<double> scores = pr.scores();
    apply_scaler_checked(scores, scaler);

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("score"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- articulationPoints_stream  */
// NetworKit has no ArticulationPoints class. Use BiconnectedComponents:
// a node is an articulation point iff it belongs to more than one
// biconnected component (getComponentsOfNode returns a set of component ids).
//
// For each articulation point, additionally report the sizes of the
// connected components that result from removing it (resulting_component_*
// columns), computed via NetworKit::ConnectedComponents on a private copy
// of the graph with that single node removed.

Datum age_gds_articulationpoints_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::BiconnectedComponents bcc(pg.graph);
    try { bcc.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("BiconnectedComponents failed: %s", ex.what())));
    }

    Oid types[4] = {INT8OID, INT4OID, INT4OID, INT4OID};
    SRFState s = srf_begin(fcinfo, 4, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        const auto &comps = bcc.getComponentsOfNode(u);
        if (comps.size() <= 1)
            return;

        NetworKit::Graph without_u(pg.graph);
        without_u.removeNode(u);

        NetworKit::ConnectedComponents cc(without_u);
        try { cc.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("ConnectedComponents failed: %s", ex.what())));
        }

        auto sizes = cc.getComponentSizes();
        int32_t comp_count = (int32_t)sizes.size();
        int32_t comp_min = 0, comp_max = 0;
        bool first = true;
        for (const auto &[comp_id, size] : sizes) {
            (void)comp_id;
            int32_t sz = (int32_t)size;
            if (first) { comp_min = comp_max = sz; first = false; }
            else {
                comp_min = std::min(comp_min, sz);
                comp_max = std::max(comp_max, sz);
            }
        }

        Datum values[4]; bool nulls[4] = {false, false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int32GetDatum(comp_count);
        values[2] = Int32GetDatum(comp_min);
        values[3] = Int32GetDatum(comp_max);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ----------------------------------- articulationPoints_stream (Cypher) -- */

PG_FUNCTION_INFO_V1(age_gds_articulationpoints_stream_cypher);

Datum age_gds_articulationpoints_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::BiconnectedComponents bcc(pg.graph);
    try { bcc.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("BiconnectedComponents failed: %s", ex.what())));
    }

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        const auto &comps = bcc.getComponentsOfNode(u);
        if (comps.size() <= 1)
            return;

        NetworKit::Graph without_u(pg.graph);
        without_u.removeNode(u);
        NetworKit::ConnectedComponents cc(without_u);
        try { cc.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("ConnectedComponents failed: %s", ex.what())));
        }

        auto sizes = cc.getComponentSizes();
        int32_t comp_count = (int32_t)sizes.size();
        int32_t comp_min = 0, comp_max = 0;
        bool first = true;
        for (const auto &[comp_id, size] : sizes) {
            (void)comp_id;
            int32_t sz = (int32_t)size;
            if (first) { comp_min = comp_max = sz; first = false; }
            else { comp_min = std::min(comp_min, sz); comp_max = std::max(comp_max, sz); }
        }

        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("resultingComponentCount"); b.add_int64(comp_count);
        b.key("resultingComponentMin"); b.add_int64(comp_min);
        b.key("resultingComponentMax"); b.add_int64(comp_max);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

} /* extern "C" */
