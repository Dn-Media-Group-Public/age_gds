// src/algorithms/linkprediction.cpp
//
// Link prediction SQL functions using NetworKit link predictors.
// All use runAll() which scores all currently unconnected node pairs.
//
// Predictors: AdamicAdar, CommonNeighbors, PreferentialAttachment,
//             ResourceAllocation.
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <networkit/linkprediction/AdamicAdarIndex.hpp>
#include <networkit/linkprediction/CommonNeighborsIndex.hpp>
#include <networkit/linkprediction/PreferentialAttachmentIndex.hpp>
#include <networkit/linkprediction/ResourceAllocationIndex.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <vector>
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
#include "util/srf_helper.h"

/* ============================================================ helpers === */

static GraphHandle resolve_lp_graph(FunctionCallInfo fcinfo)
{
    /* Args: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query */
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    return get_or_load_graph(gname, agraph, nq, eq);
}

/* Emit results from a runAll() prediction vector */
static Datum emit_lp_results(
    FunctionCallInfo fcinfo,
    const ProjectedGraph &pg,
    std::vector<NetworKit::LinkPredictor::prediction> &preds)
{
    /* Columns: node1 bigint, node2 bigint, score float8 */
    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s   = srf_begin(fcinfo, 3, types);

    for (auto &pred : preds) {
        NetworKit::node u = pred.first.first;
        NetworKit::node v = pred.first.second;
        double          sc = pred.second;

        Datum values[3]; bool nulls[3] = {false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(pg.id_map.resolve(v));
        values[2] = Float8GetDatum(sc);
        srf_emit(s, values, nulls);
    }

    return srf_end(s, fcinfo);
}

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_linkpred_adamicadar_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_commonneighbors_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_preferentialattachment_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_resourceallocation_stream);

/* ---------------------------------------- adamicAdar_stream -------------- */

Datum age_gds_linkpred_adamicadar_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_lp_graph(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::AdamicAdarIndex pred(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> results;
    try { results = pred.runAll(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AdamicAdarIndex failed: %s", ex.what())));
    }

    return emit_lp_results(fcinfo, pg, results);
}

/* ---------------------------------------- commonNeighbors_stream --------- */

Datum age_gds_linkpred_commonneighbors_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_lp_graph(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::CommonNeighborsIndex pred(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> results;
    try { results = pred.runAll(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("CommonNeighborsIndex failed: %s", ex.what())));
    }

    return emit_lp_results(fcinfo, pg, results);
}

/* ---------------------------------------- preferentialAttachment_stream -- */

Datum age_gds_linkpred_preferentialattachment_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_lp_graph(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PreferentialAttachmentIndex pred(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> results;
    try { results = pred.runAll(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("PreferentialAttachmentIndex failed: %s", ex.what())));
    }

    return emit_lp_results(fcinfo, pg, results);
}

/* ---------------------------------------- resourceAllocation_stream ------ */

Datum age_gds_linkpred_resourceallocation_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_lp_graph(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::ResourceAllocationIndex pred(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> results;
    try { results = pred.runAll(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ResourceAllocationIndex failed: %s", ex.what())));
    }

    return emit_lp_results(fcinfo, pg, results);
}

} /* extern "C" */
