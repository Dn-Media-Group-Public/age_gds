// src/algorithms/linkprediction.cpp
//
// Link prediction SQL functions using NetworKit link predictors.
//
// Two shapes per predictor (AdamicAdar, CommonNeighbors,
// PreferentialAttachment, ResourceAllocation):
//   - `*_stream`: runAll(), scores all currently unconnected node pairs
//     (kept unchanged, per plan 04's "extend, don't remove" precedent).
//   - single-pair scalar function (new, plan 04): matches Neo4j GDS's
//     `gds.alpha.linkprediction.<name>(node1, node2, {...})` shape, uses the
//     base LinkPredictor::run(u, v) single-pair method.
//
// See docs/implemented-plans/04-similarity-linkprediction-parity.md for the
// design decisions, including the `direction` NetworKit gap (documented at
// validate_direction_both_only() below).
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <networkit/linkprediction/AdamicAdarIndex.hpp>
#include <networkit/linkprediction/CommonNeighborsIndex.hpp>
#include <networkit/linkprediction/PreferentialAttachmentIndex.hpp>
#include <networkit/linkprediction/ResourceAllocationIndex.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/graph/GraphTools.hpp>
#include <networkit/Globals.hpp>

#include <cctype>
#include <cinttypes>
#include <string>
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

/* ---------------------------------------- single-pair scalar functions --- */
//
// Args (all four): 0=node1, 1=node2, 2=graph_name, 3=relationship_query,
//                  4=direction
//
// Load the final Graph* for a single-pair call. graph_name is required (no
// age_graph anonymous mode -- this is a spot-check function operating on an
// already-projected graph, matching Neo4j's model where
// gds.alpha.linkprediction.* always runs against the graph catalog).
// relationship_query, when given, re-projects the named graph's topology
// against a *different* edge_query -- not a weight substitution (unlike
// relationship_weight_property elsewhere), an actual different edge set --
// reusing the exact same anonymous-load path get_or_load_graph() already
// provides for edge_query, per the plan's "threads through as edge_query
// already does elsewhere".
//
// This helper never move-assigns a GraphHandle whose `owned` is engaged
// (only ever direct-initializes one via `return get_or_load_graph(...)`,
// which is guaranteed-elided): GraphHandle::pg can point at
// `&*owned`, and a subsequent move/assignment of the whole GraphHandle
// object would relocate `owned`'s storage out from under that pointer.
static GraphHandle load_final_graph(const char *gname, const char *rel_q, const char *funcname)
{
    if (!rel_q)
        return get_or_load_graph(gname, nullptr, nullptr, nullptr);

    const ProjectedGraph &cached = session_cache_get(gname);
    if (cached.age_graph.empty() || cached.node_query.empty())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: relationship_query requires graph \"%s\" to have its "
                        "age_graph/node_query recorded in the catalog (re-run "
                        "graph_project to populate them)",
                        funcname, cached.name.c_str())));
    return get_or_load_graph(nullptr, cached.age_graph.c_str(), cached.node_query.c_str(),
                             rel_q, cached.directed, cached.weighted);
}

/* Validates `direction`, and -- for anything other than 'BOTH' -- raises a
 * documented gap instead of computing something undefined or hand-rolling
 * directed neighbor-set arithmetic (forbidden by project rules). See the
 * comment body for the exact NetworKit behavior this is based on. */
static void validate_direction_both_only(const char *dir_raw, const char *funcname)
{
    std::string dir(dir_raw);
    for (auto &c : dir) c = (char)toupper((unsigned char)c);
    if (dir != "BOTH" && dir != "OUTGOING" && dir != "INCOMING")
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: direction must be one of 'BOTH', 'OUTGOING', 'INCOMING' (got \"%s\")",
                        funcname, dir_raw)));
    if (dir != "BOTH")
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("%s: direction = '%s' is not supported", funcname, dir_raw),
                 errdetail("NetworKit::LinkPredictor::run() "
                          "(third_party/networkit/include/networkit/linkprediction/"
                          "LinkPredictor.cpp) unconditionally throws "
                          "std::invalid_argument(\"Only undirected graphs accepted.\") "
                          "whenever Graph::isDirected() is true, and none of "
                          "AdamicAdarIndex/CommonNeighborsIndex/PreferentialAttachmentIndex/"
                          "ResourceAllocationIndex offer a directed-aware alternative -- "
                          "there is no NetworKit class to delegate an "
                          "OUTGOING/INCOMING-restricted score to. Computing it ourselves "
                          "would mean hand-rolling directed neighbor-set arithmetic, "
                          "forbidden by project rules. Only direction = 'BOTH' "
                          "(the graph, converted to undirected if needed) is supported.")));
}

/* Shared body for all four single-pair scalar functions. */
template <class Predictor>
static Datum run_linkpred_single(FunctionCallInfo fcinfo, const char *funcname)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s: node1 and node2 are required", funcname)));
    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s: graph_name is required", funcname)));

    int64_t n1 = PG_GETARG_INT64(0);
    int64_t n2 = PG_GETARG_INT64(1);
    const char *gname   = text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *rel_q   = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *dir_raw = PG_ARGISNULL(4) ? "BOTH"  : text_to_cstring(PG_GETARG_TEXT_PP(4));

    validate_direction_both_only(dir_raw, funcname);

    GraphHandle gh = load_final_graph(gname, rel_q, funcname);
    const ProjectedGraph &pg = *gh.pg;

    auto it1 = pg.id_map.graphid_to_nk.find(n1);
    if (it1 == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s: node1 %" PRId64 " not in graph", funcname, n1)));
    auto it2 = pg.id_map.graphid_to_nk.find(n2);
    if (it2 == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s: node2 %" PRId64 " not in graph", funcname, n2)));
    NetworKit::node u = it1->second;
    NetworKit::node v = it2->second;

    NetworKit::Graph undirected_storage;
    const NetworKit::Graph *g = &pg.graph;
    if (g->isDirected()) {
        undirected_storage = NetworKit::GraphTools::toUndirected(*g);
        g = &undirected_storage;
    }

    Predictor pred(*g);
    double score;
    try {
        score = pred.run(u, v);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("%s failed: %s", funcname, ex.what())));
    }

    PG_RETURN_FLOAT8(score);
}

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_linkpred_adamicadar_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_commonneighbors_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_preferentialattachment_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_resourceallocation_stream);
PG_FUNCTION_INFO_V1(age_gds_linkpred_adamicadar);
PG_FUNCTION_INFO_V1(age_gds_linkpred_commonneighbors);
PG_FUNCTION_INFO_V1(age_gds_linkpred_preferentialattachment);
PG_FUNCTION_INFO_V1(age_gds_linkpred_resourceallocation);

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

/* ---------------------------------------- single-pair scalar functions --- */

Datum age_gds_linkpred_adamicadar(PG_FUNCTION_ARGS)
{
    return run_linkpred_single<NetworKit::AdamicAdarIndex>(fcinfo, "linkprediction_adamicAdar");
}

Datum age_gds_linkpred_commonneighbors(PG_FUNCTION_ARGS)
{
    return run_linkpred_single<NetworKit::CommonNeighborsIndex>(fcinfo, "linkprediction_commonNeighbors");
}

Datum age_gds_linkpred_preferentialattachment(PG_FUNCTION_ARGS)
{
    return run_linkpred_single<NetworKit::PreferentialAttachmentIndex>(fcinfo, "linkprediction_preferentialAttachment");
}

Datum age_gds_linkpred_resourceallocation(PG_FUNCTION_ARGS)
{
    return run_linkpred_single<NetworKit::ResourceAllocationIndex>(fcinfo, "linkprediction_resourceAllocation");
}

} /* extern "C" */
