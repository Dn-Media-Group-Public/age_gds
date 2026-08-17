// src/algorithms/pathfinding.cpp
//
// Path-finding SQL functions:
//   bfs, shortestPath_dijkstra, deltaStepping (→Dijkstra fallback),
//   bellmanFord (→Dijkstra fallback), dfs, randomWalk,
//   spanningTree, allShortestPaths, maxFlow.
//
// Missing NetworKit classes documented below.
// Include order: NetworKit first, then PostgreSQL, then restore ERROR=21.

#include <networkit/distance/BFS.hpp>
#include <networkit/distance/Dijkstra.hpp>
#include <networkit/distance/APSP.hpp>
#include <networkit/distance/SSSP.hpp>
#include <networkit/graph/SpanningForest.hpp>
#include <networkit/graph/DFS.hpp>
#include <networkit/flow/EdmondsKarp.hpp>
#include <networkit/auxiliary/Random.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <limits>
#include <stdexcept>
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
#include "util/srf_helper.h"

/* helper: resolve graph for functions with varying arg layout.
 * age_graph_idx = position of the age_graph argument. */
static GraphHandle resolve_graph_args(FunctionCallInfo fcinfo, int age_graph_idx)
{
    const char *gname  = PG_ARGISNULL(0)                  ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(age_graph_idx)     ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx));
    const char *nq     = PG_ARGISNULL(age_graph_idx + 1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 1));
    const char *eq     = PG_ARGISNULL(age_graph_idx + 2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 2));
    return get_or_load_graph(gname, agraph, nq, eq);
}

/* helper: get source NetworKit node from AGE graphid arg (arg index 1) */
static NetworKit::node get_source_node(FunctionCallInfo fcinfo,
                                       const ProjectedGraph &pg)
{
    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_node is required")));
    int64_t age_src = PG_GETARG_INT64(1);
    auto it = pg.id_map.graphid_to_nk.find(age_src);
    if (it == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_node %" PRId64 " not in graph", age_src)));
    return it->second;
}

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_bfs_stream);
PG_FUNCTION_INFO_V1(age_gds_shortestpath_dijkstra_stream);
PG_FUNCTION_INFO_V1(age_gds_deltastepping_stream);
PG_FUNCTION_INFO_V1(age_gds_bellmanford_stream);
PG_FUNCTION_INFO_V1(age_gds_dfs_stream);
PG_FUNCTION_INFO_V1(age_gds_randomwalk_stream);
PG_FUNCTION_INFO_V1(age_gds_spanningtree_stream);
PG_FUNCTION_INFO_V1(age_gds_allshortestpaths_stream);
PG_FUNCTION_INFO_V1(age_gds_maxflow_stats);

/* ---------------------------------------------- bfs_stream --------------- */

Datum age_gds_bfs_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    NetworKit::BFS algo(pg.graph, src, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("BFS failed: %s", ex.what())));
    }

    const auto &dists = algo.getDistances();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double d = dists[u];
        bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
        Datum values[2]; bool nulls[2] = {false, is_inf};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        if (!is_inf) values[1] = Float8GetDatum(d);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- shortestPath_dijkstra ---- */

Datum age_gds_shortestpath_dijkstra_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    NetworKit::Dijkstra algo(pg.graph, src, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Dijkstra failed: %s", ex.what())));
    }

    const auto &dists = algo.getDistances();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double d = dists[u];
        bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
        Datum values[2]; bool nulls[2] = {false, is_inf};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        if (!is_inf) values[1] = Float8GetDatum(d);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- deltaStepping_stream ----- */
// NetworKit has no DeltaStepping class. Dijkstra used as equivalent for
// non-negative weight graphs.

Datum age_gds_deltastepping_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    NetworKit::Dijkstra algo(pg.graph, src, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("deltaStepping (Dijkstra fallback) failed: %s", ex.what())));
    }

    const auto &dists = algo.getDistances();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double d = dists[u];
        bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
        Datum values[2]; bool nulls[2] = {false, is_inf};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        if (!is_inf) values[1] = Float8GetDatum(d);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- bellmanFord_stream ------- */
// NetworKit has no standalone BellmanFord class (AlgebraicBellmanFord<Matrix>
// is a template that requires a matrix instantiation and MinPlusSemiring).
// Dijkstra used as equivalent for non-negative weight graphs.

Datum age_gds_bellmanford_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    NetworKit::Dijkstra algo(pg.graph, src, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("bellmanFord (Dijkstra fallback) failed: %s", ex.what())));
    }

    const auto &dists = algo.getDistances();
    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double d = dists[u];
        bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
        Datum values[2]; bool nulls[2] = {false, is_inf};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        if (!is_inf) values[1] = Float8GetDatum(d);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- dfs_stream --------------- */
// Uses NetworKit::Traversal::DFSEdgesFrom (from <networkit/graph/DFS.hpp>).
// Depth tracked via auxiliary map (DFS traversal itself is NetworKit's).

Datum age_gds_dfs_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    std::vector<int64_t> depth(pg.graph.upperNodeIdBound(), -1);
    depth[src] = 0;

    try {
        NetworKit::Traversal::DFSEdgesFrom(pg.graph, src,
            [&](NetworKit::node u, NetworKit::node v,
                NetworKit::edgeweight, NetworKit::edgeid) {
                depth[v] = depth[u] + 1;
            });
    }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("DFS failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT4OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        if (depth[u] >= 0) {
            Datum values[2]; bool nulls[2] = {false, false};
            values[0] = Int64GetDatum(pg.id_map.resolve(u));
            values[1] = Int32GetDatum((int32_t)depth[u]);
            srf_emit(s, values, nulls);
        }
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- randomWalk_stream -------- */
// NetworKit has no RandomWalk class. Implemented using Aux::Random::choice
// (NetworKit random utility) + NetworKit::Graph::forNeighborsOf.
// Returns visited nodes in walk order (with repetition).

Datum age_gds_randomwalk_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 3);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    int walk_length = PG_ARGISNULL(2) ? 10 : PG_GETARG_INT32(2);
    if (walk_length < 0) walk_length = 0;

    std::vector<NetworKit::node> walk;
    walk.reserve((size_t)(walk_length + 1));
    walk.push_back(src);

    NetworKit::node current = src;
    for (int i = 0; i < walk_length; ++i) {
        std::vector<NetworKit::node> neighbors;
        pg.graph.forNeighborsOf(current, [&](NetworKit::node v) {
            neighbors.push_back(v);
        });
        if (neighbors.empty()) break;
        current = Aux::Random::choice(neighbors);
        walk.push_back(current);
    }

    Oid types[1] = {INT8OID};
    SRFState s = srf_begin(fcinfo, 1, types);

    for (NetworKit::node u : walk) {
        Datum values[1]; bool nulls[1] = {false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        srf_emit(s, values, nulls);
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- spanningTree_stream ------ */

Datum age_gds_spanningtree_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::SpanningForest sf(pg.graph);
    try { sf.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("SpanningForest failed: %s", ex.what())));
    }

    const NetworKit::Graph &forest = sf.getForest();

    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 3, types);

    forest.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
        Datum values[3]; bool nulls[3] = {false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(pg.id_map.resolve(v));
        values[2] = Float8GetDatum(w);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- allShortestPaths_stream -- */

Datum age_gds_allshortestpaths_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::APSP algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("APSP failed: %s", ex.what())));
    }

    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 3, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        pg.graph.forNodes([&](NetworKit::node v) {
            double d = algo.getDistance(u, v);
            bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
            Datum values[3]; bool nulls[3] = {false, false, is_inf};
            values[0] = Int64GetDatum(pg.id_map.resolve(u));
            values[1] = Int64GetDatum(pg.id_map.resolve(v));
            if (!is_inf) values[2] = Float8GetDatum(d);
            srf_emit(s, values, nulls);
        });
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- maxFlow_stats ------------ */

Datum age_gds_maxflow_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 3);
    const ProjectedGraph &pg = *gh.pg;

    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_node is required")));
    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("target_node is required")));

    int64_t age_src  = PG_GETARG_INT64(1);
    int64_t age_sink = PG_GETARG_INT64(2);

    auto src_it = pg.id_map.graphid_to_nk.find(age_src);
    auto snk_it = pg.id_map.graphid_to_nk.find(age_sink);

    if (src_it == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_node not in graph")));
    if (snk_it == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("target_node not in graph")));

    NetworKit::node nk_src  = src_it->second;
    NetworKit::node nk_sink = snk_it->second;

    if (nk_src == nk_sink)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_node and target_node must be different")));

    auto t0 = std::chrono::steady_clock::now();

    // EdmondsKarp requires edge IDs to be indexed.  indexEdges() only assigns
    // sequential IDs to edges without altering topology, so const_cast is safe.
    const_cast<NetworKit::Graph &>(pg.graph).indexEdges();

    NetworKit::EdmondsKarp ek(pg.graph, nk_src, nk_sink);
    try { ek.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EdmondsKarp failed: %s", ex.what())));
    }

    auto    t1         = std::chrono::steady_clock::now();
    int64_t compute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             t1 - t0).count();

    double flow = ek.getMaxFlow();

    Oid types[2] = {FLOAT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    Datum values[2]; bool nulls[2] = {false, false};
    values[0] = Float8GetDatum(flow);
    values[1] = Int64GetDatum(compute_ms);
    srf_emit(s, values, nulls);

    return srf_end(s, fcinfo);
}

} /* extern "C" */
