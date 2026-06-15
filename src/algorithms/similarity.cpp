// src/algorithms/similarity.cpp
//
// Node similarity and KNN: nodeSimilarity_stream, knn_stream.
// Uses NetworKit::JaccardIndex for neighborhood-overlap similarity.
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <networkit/linkprediction/JaccardIndex.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <algorithm>
#include <vector>
#include <unordered_map>
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

static GraphHandle resolve_sim_graph(FunctionCallInfo fcinfo, int age_graph_idx)
{
    const char *gname  = PG_ARGISNULL(0)                  ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(age_graph_idx)     ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx));
    const char *nq     = PG_ARGISNULL(age_graph_idx + 1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 1));
    const char *eq     = PG_ARGISNULL(age_graph_idx + 2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 2));
    return get_or_load_graph(gname, agraph, nq, eq);
}

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_nodesimilarity_stream);
PG_FUNCTION_INFO_V1(age_gds_knn_stream);

/* ------------------------------------------ nodeSimilarity_stream -------- */
// Args: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query

Datum age_gds_nodesimilarity_stream(PG_FUNCTION_ARGS)
{
    /* age_graph_idx = 1 */
    GraphHandle gh = resolve_sim_graph(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    /* Build all pairs (u, v) where u < v */
    std::vector<std::pair<NetworKit::node, NetworKit::node>> all_pairs;
    pg.graph.forNodes([&](NetworKit::node u) {
        pg.graph.forNodes([&](NetworKit::node v) {
            if (u < v)
                all_pairs.emplace_back(u, v);
        });
    });

    NetworKit::JaccardIndex ji(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> preds;
    try {
        preds = ji.runOn(all_pairs);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("nodeSimilarity: JaccardIndex failed: %s", ex.what())));
    }

    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s   = srf_begin(fcinfo, 3, types);

    for (auto &pred : preds) {
        NetworKit::node u = pred.first.first;
        NetworKit::node v = pred.first.second;
        double sim        = pred.second;

        Datum values[3]; bool nulls[3] = {false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(pg.id_map.resolve(v));
        values[2] = Float8GetDatum(sim);
        srf_emit(s, values, nulls);
    }

    return srf_end(s, fcinfo);
}

/* ------------------------------------------ knn_stream ------------------- */
// Args: 0=graph_name, 1=top_k, 2=age_graph, 3=node_query, 4=edge_query

Datum age_gds_knn_stream(PG_FUNCTION_ARGS)
{
    int top_k = PG_ARGISNULL(1) ? 5 : PG_GETARG_INT32(1);
    if (top_k <= 0) top_k = 5;

    /* age_graph_idx = 2 */
    GraphHandle gh = resolve_sim_graph(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;

    /* Build all pairs (u, v) where u < v */
    std::vector<std::pair<NetworKit::node, NetworKit::node>> all_pairs;
    pg.graph.forNodes([&](NetworKit::node u) {
        pg.graph.forNodes([&](NetworKit::node v) {
            if (u < v)
                all_pairs.emplace_back(u, v);
        });
    });

    NetworKit::JaccardIndex ji(pg.graph);
    std::vector<NetworKit::LinkPredictor::prediction> preds;
    try {
        preds = ji.runOn(all_pairs);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("knn: JaccardIndex failed: %s", ex.what())));
    }

    /* Per-node neighbor lists: node -> [(score, other_node)] */
    NetworKit::count bound = pg.graph.upperNodeIdBound();
    std::vector<std::vector<std::pair<double, NetworKit::node>>> nbrs(bound);

    for (auto &pred : preds) {
        NetworKit::node u = pred.first.first;
        NetworKit::node v = pred.first.second;
        double          s = pred.second;
        if (u < bound) nbrs[u].emplace_back(s, v);
        if (v < bound) nbrs[v].emplace_back(s, u);
    }

    /* Columns: sourceNodeId bigint, targetNodeId bigint, similarity float8, rank int */
    Oid types[4] = {INT8OID, INT8OID, FLOAT8OID, INT4OID};
    SRFState state = srf_begin(fcinfo, 4, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        auto &nlist = nbrs[u];
        /* Sort descending by score */
        std::sort(nlist.begin(), nlist.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });

        int limit = std::min((int)nlist.size(), top_k);
        int64_t src_id = pg.id_map.resolve(u);

        for (int r = 0; r < limit; r++) {
            int64_t dst_id = pg.id_map.resolve(nlist[r].second);
            double  sim    = nlist[r].first;

            Datum values[4]; bool nulls[4] = {false, false, false, false};
            values[0] = Int64GetDatum(src_id);
            values[1] = Int64GetDatum(dst_id);
            values[2] = Float8GetDatum(sim);
            values[3] = Int32GetDatum(r + 1);
            srf_emit(state, values, nulls);
        }
    });

    return srf_end(state, fcinfo);
}

} /* extern "C" */
