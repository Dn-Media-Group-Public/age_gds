// src/util/writeback.cpp
//
// Write-back utility: stream algorithm scores back to AGE nodes via Cypher SET.
// Also provides pageRank_write and betweenness_write SQL entry points.
//
// Include order: NetworKit first (it redefines ERROR), then PG, then restore.

#include <networkit/centrality/PageRank.hpp>
#include <networkit/centrality/Betweenness.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "util/writeback.h"
#include "util/config_parser.h"
#include "catalog/session_cache.h"

#include <string>
#include <unordered_map>
#include <stdexcept>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ============================================================ utilities === */

std::string lookup_age_graph(const char *projection_name)
{
    char buf[256] = {0};

    SPI_connect();

    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(projection_name)};

    int ret = SPI_execute_with_args(
        "SELECT age_graph FROM age_gds.graphs WHERE name = $1",
        1, argtypes, args, nullptr, true, 1);

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("lookup_age_graph: catalog query failed (SPI %d)", ret)));
    }

    if (SPI_processed == 0) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("projected graph \"%s\" does not exist", projection_name)));
    }

    bool  isnull = false;
    Datum d      = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull) {
        char *ag = TextDatumGetCString(d);
        strlcpy(buf, ag, sizeof(buf));
    }

    SPI_finish();
    return std::string(buf);
}

void writeback_scores(
    const char *age_graph,
    const char *property,
    const std::unordered_map<int64_t, double> &scores)
{
    if (scores.empty())
        return;

    SPI_connect();

    for (auto &[graphid, score] : scores) {
        char sql[2048];
        snprintf(sql, sizeof(sql),
            "SELECT * FROM ag_catalog.cypher('%s', $$ "
            "MATCH (n) WHERE id(n) = %lld "
            "SET n.%s = %f "
            "RETURN n "
            "$$) AS (v agtype)",
            age_graph, (long long)graphid, property, score);

        int ret = SPI_execute(sql, false, 0);
        if (ret < 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("writeback_scores: Cypher SET failed for node %lld"
                            " (SPI %d)", (long long)graphid, ret)));
    }

    SPI_finish();
}

/* ============================================================ C functions == */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_pagerank_write);
PG_FUNCTION_INFO_V1(age_gds_betweenness_write);

/* ------------------------------------------ pageRank_write --------------- */
// Args: 0=graph_name, 1=write_property, 2=damping_factor,
//       3=max_iterations, 4=tolerance, 5=age_graph,
//       6=node_query, 7=edge_query

Datum age_gds_pagerank_write(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *prop   = PG_ARGISNULL(1) ? "pagerank" : text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool   has_damping  = !PG_ARGISNULL(2);
    double damping      = has_damping  ? PG_GETARG_FLOAT8(2) : 0.85;
    bool   has_max_iter = !PG_ARGISNULL(3);
    int    max_iter     = has_max_iter ? PG_GETARG_INT32(3)  : 20;
    bool   has_tol      = !PG_ARGISNULL(4);
    double tol          = has_tol      ? PG_GETARG_FLOAT8(4) : 1e-7;
    const char *agraph = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    const char *nq     = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    const char *eq     = PG_ARGISNULL(7) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(7));

    PageRankConfig cfg = parse_pagerank_config(
        false, (Datum)0,
        has_damping,  damping,
        has_max_iter, max_iter,
        has_tol,      tol);

    /* Resolve age_graph name for writeback before get_or_load_graph
       (which also does SPI internally for named projections) */
    std::string wb_age_graph;
    if (gname)
        wb_age_graph = lookup_age_graph(gname);
    else if (agraph)
        wb_age_graph = agraph;
    /* else: get_or_load_graph will raise ERROR below */

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PageRank pr(pg.graph, cfg.damping_factor, cfg.tolerance);
    pr.maxIterations = (NetworKit::count)cfg.max_iterations;

    try { pr.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pageRank_write: PageRank failed: %s", ex.what())));
    }

    const auto &scores = pr.scores();

    std::unordered_map<int64_t, double> score_map;
    pg.graph.forNodes([&](NetworKit::node u) {
        score_map[pg.id_map.resolve(u)] = scores[u];
    });

    writeback_scores(wb_age_graph.c_str(), prop, score_map);

    PG_RETURN_VOID();
}

/* ------------------------------------------ betweenness_write ------------ */
// Args: 0=graph_name, 1=write_property, 2=age_graph,
//       3=node_query, 4=edge_query

Datum age_gds_betweenness_write(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *prop   = PG_ARGISNULL(1) ? "betweenness" : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *agraph = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *nq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *eq     = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));

    std::string wb_age_graph;
    if (gname)
        wb_age_graph = lookup_age_graph(gname);
    else if (agraph)
        wb_age_graph = agraph;

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::Betweenness algo(pg.graph, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("betweenness_write: Betweenness failed: %s", ex.what())));
    }

    const auto &scores = algo.scores();

    std::unordered_map<int64_t, double> score_map;
    pg.graph.forNodes([&](NetworKit::node u) {
        score_map[pg.id_map.resolve(u)] = scores[u];
    });

    writeback_scores(wb_age_graph.c_str(), prop, score_map);

    PG_RETURN_VOID();
}

} /* extern "C" */
