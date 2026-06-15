// src/algorithms/community.cpp
//
// Community detection and graph structure SQL functions:
//   louvain, leiden, labelPropagation, wcc, scc, kCore,
//   triangleCount, localClusteringCoefficient, modularity.
//
// Include order: NetworKit first, then PostgreSQL, then restore ERROR=21.

#include <networkit/community/PLM.hpp>
#include <networkit/community/ParallelLeiden.hpp>
#include <networkit/community/PLP.hpp>
#include <networkit/community/Modularity.hpp>
#include <networkit/components/ConnectedComponents.hpp>
#include <networkit/components/StronglyConnectedComponents.hpp>
#include <networkit/centrality/CoreDecomposition.hpp>
#include <networkit/centrality/LocalClusteringCoefficient.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <cmath>
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

/* helper: emit community partition rows */
static void emit_partition(SRFState &s,
                            const ProjectedGraph &pg,
                            const NetworKit::Partition &part)
{
    pg.graph.forNodes([&](NetworKit::node u) {
        int64_t comm = (int64_t)part.subsetOf(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comm);
        srf_emit(s, values, nulls);
    });
}

// Helper: resolve graph for simple 1-arg functions.
// Layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query
static GraphHandle resolve_graph_args(FunctionCallInfo fcinfo)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    return get_or_load_graph(gname, agraph, nq, eq);
}

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_louvain_stream);
PG_FUNCTION_INFO_V1(age_gds_leiden_stream);
PG_FUNCTION_INFO_V1(age_gds_labelpropagation_stream);
PG_FUNCTION_INFO_V1(age_gds_wcc_stream);
PG_FUNCTION_INFO_V1(age_gds_scc_stream);
PG_FUNCTION_INFO_V1(age_gds_kcore_stream);
PG_FUNCTION_INFO_V1(age_gds_trianglecount_stream);
PG_FUNCTION_INFO_V1(age_gds_localclusteringcoefficient_stream);
PG_FUNCTION_INFO_V1(age_gds_modularity_stats);

/* ---------------------------------------------- louvain_stream ----------- */

Datum age_gds_louvain_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PLM algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Louvain (PLM) failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    emit_partition(s, pg, algo.getPartition());
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- leiden_stream ------------ */

Datum age_gds_leiden_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::ParallelLeiden algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ParallelLeiden failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    emit_partition(s, pg, algo.getPartition());
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- labelPropagation_stream -- */

Datum age_gds_labelpropagation_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PLP algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LabelPropagation (PLP) failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    emit_partition(s, pg, algo.getPartition());
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- wcc_stream --------------- */

Datum age_gds_wcc_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::ConnectedComponents algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ConnectedComponents failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        int64_t comp = (int64_t)algo.componentOfNode(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comp);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- scc_stream --------------- */

Datum age_gds_scc_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::StronglyConnectedComponents algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("StronglyConnectedComponents failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        int64_t comp = (int64_t)algo.componentOfNode(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comp);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- kCore_stream ------------- */

Datum age_gds_kcore_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::CoreDecomposition algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("CoreDecomposition failed: %s", ex.what())));
    }

    const auto &scores = algo.scores();
    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        int64_t core = (int64_t)scores[u];
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(core);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- triangleCount_stream ----- */
// Uses LocalClusteringCoefficient: triangles_u = LCC(u) * deg*(deg-1)/2

Datum age_gds_trianglecount_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::LocalClusteringCoefficient algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LocalClusteringCoefficient failed: %s", ex.what())));
    }

    const auto &lcc = algo.scores();
    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double deg = (double)pg.graph.degree(u);
        int64_t triangles = 0;
        if (deg >= 2.0) {
            triangles = (int64_t)std::round(lcc[u] * deg * (deg - 1.0) / 2.0);
        }
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(triangles);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- localClusteringCoefficient */

Datum age_gds_localclusteringcoefficient_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::LocalClusteringCoefficient algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LocalClusteringCoefficient failed: %s", ex.what())));
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

/* ---------------------------------------------- modularity_stats --------- */

Datum age_gds_modularity_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::PLM plm(pg.graph);
    try { plm.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("PLM (modularity) failed: %s", ex.what())));
    }

    NetworKit::Modularity mod;
    double mod_val = mod.getQuality(plm.getPartition(), pg.graph);

    Oid types[1] = {FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 1, types);

    Datum values[1]; bool nulls[1] = {false};
    values[0] = Float8GetDatum(mod_val);
    srf_emit(s, values, nulls);

    return srf_end(s, fcinfo);
}

} /* extern "C" */
