// src/algorithms/pathfinding.cpp
//
// Path-finding SQL functions:
//   bfs, dfs, shortestPath.dijkstra, allShortestPaths.dijkstra,
//   deltaStepping (->Dijkstra fallback), bellmanFord (AlgebraicBellmanFord),
//   randomWalk, spanningTree, allShortestPaths, maxFlow,
//   shortestPath.astar, minCostMaxFlow.
//
// See docs/implemented-plans/03-pathfinding-parity.md for the design
// decisions behind every parameter added in this file.
//
// Include order: NetworKit first, then PostgreSQL, then restore ERROR=21.

#include <networkit/distance/BFS.hpp>
#include <networkit/distance/Dijkstra.hpp>
#include <networkit/distance/MultiTargetBFS.hpp>
#include <networkit/distance/AStar.hpp>
#include <networkit/distance/APSP.hpp>
#include <networkit/distance/SSSP.hpp>
#include <networkit/algebraic/CSRMatrix.hpp>
#include <networkit/algebraic/algorithms/AlgebraicBellmanFord.hpp>
#include <networkit/graph/SpanningForest.hpp>
#include <networkit/graph/RandomMaximumSpanningForest.hpp>
#include <networkit/graph/DFS.hpp>
#include <networkit/graph/BFS.hpp>
#include <networkit/flow/EdmondsKarp.hpp>
#include <networkit/flow/SuccessiveShortestPath.hpp>
#include <networkit/auxiliary/Random.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

// NetworKit::Embedding::BiasedRandomWalk (see CMakeLists.txt's comment on
// the extra include directory this depends on): the only NetworKit class
// that both (a) performs P/Q-biased ("return"/"in-out") random walks and
// (b) exposes the raw walks themselves, rather than an aggregated
// embedding. NetworKit::Node2Vec (the public, installed API used by
// embedding.cpp) wraps this same class internally but only exposes the
// trained embedding vectors via getFeatures() -- not the walks -- so it
// cannot serve randomWalk_stream. This is still real, vendored NetworKit
// code (compiled into the same `networkit` static library age_gds already
// links), just not re-exported under include/networkit/.
#include <AliasSampler.hpp>
#include <BiasedRandomWalk.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

#include "catalog/session_cache.h"
#include "util/srf_helper.h"
#include "util/weight_override.h"
#include "util/partition_post.h"
#include "algorithms/flow_common.h"

/* ======================================================== shared helpers */

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

/* helper: resolve a single required/optional node arg (AGE graphid) to a
 * NetworKit node id. */
static NetworKit::node get_node_arg(FunctionCallInfo fcinfo, int idx,
                                    const ProjectedGraph &pg, const char *argname,
                                    bool required = true)
{
    if (PG_ARGISNULL(idx)) {
        if (required)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("%s is required", argname)));
        return NetworKit::none;
    }
    int64_t age_id = PG_GETARG_INT64(idx);
    auto it = pg.id_map.graphid_to_nk.find(age_id);
    if (it == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s %" PRId64 " not in graph", argname, age_id)));
    return it->second;
}

/* backward-compatible alias used by pre-existing call sites below */
static NetworKit::node get_source_node(FunctionCallInfo fcinfo, const ProjectedGraph &pg)
{
    return get_node_arg(fcinfo, 1, pg, "source_node");
}

static std::vector<int64_t> parse_bigint_array(Datum arr_datum)
{
    ArrayType *arr = DatumGetArrayTypeP(arr_datum);
    int16 elmlen; bool elmbyval; char elmalign;
    get_typlenbyvalalign(INT8OID, &elmlen, &elmbyval, &elmalign);
    Datum *elems; bool *nulls; int nelems;
    deconstruct_array(arr, INT8OID, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);

    std::vector<int64_t> result;
    result.reserve((size_t)nelems);
    for (int i = 0; i < nelems; i++)
        if (!nulls[i])
            result.push_back(DatumGetInt64(elems[i]));
    return result;
}

/* Resolve a list of AGE graphids to NetworKit node ids. Raises ERROR on any
 * id not present in the graph -- silently dropping an explicitly-requested
 * node would hide a caller mistake. */
static std::vector<NetworKit::node> resolve_node_array(
    const std::vector<int64_t> &gids, const ProjectedGraph &pg, const char *argname)
{
    std::vector<NetworKit::node> out;
    out.reserve(gids.size());
    for (int64_t gid : gids) {
        auto it = pg.id_map.graphid_to_nk.find(gid);
        if (it == pg.id_map.graphid_to_nk.end())
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("%s: node %" PRId64 " not in graph", argname, gid)));
        out.push_back(it->second);
    }
    return out;
}

static ArrayType *build_bigint_array(const std::vector<int64_t> &values)
{
    int16 elmlen; bool elmbyval; char elmalign;
    get_typlenbyvalalign(INT8OID, &elmlen, &elmbyval, &elmalign);
    Datum *elems = (Datum *)palloc(values.size() * sizeof(Datum));
    for (size_t i = 0; i < values.size(); i++)
        elems[i] = Int64GetDatum(values[i]);
    return construct_array(elems, (int)values.size(), INT8OID, elmlen, elmbyval, elmalign);
}

static ArrayType *build_float8_array(const std::vector<double> &values)
{
    int16 elmlen; bool elmbyval; char elmalign;
    get_typlenbyvalalign(FLOAT8OID, &elmlen, &elmbyval, &elmalign);
    Datum *elems = (Datum *)palloc(values.size() * sizeof(Datum));
    for (size_t i = 0; i < values.size(); i++)
        elems[i] = Float8GetDatum(values[i]);
    return construct_array(elems, (int)values.size(), FLOAT8OID, elmlen, elmbyval, elmalign);
}

/* Emit a single (total_cost, node_ids, costs) row shared by
 * shortestPath_dijkstra_stream, allShortestPaths_dijkstra_stream and
 * shortestPath_astar_stream. `path` must include both endpoints (source
 * and target); `costs[i]` is the cumulative weight from source to
 * path[i]. An empty `path` means "unreachable" -> emit an all-NULL row. */
static void emit_path_row(SRFState &s, const ProjectedGraph &pg,
                          const std::vector<NetworKit::node> &path,
                          const std::vector<double> &costs,
                          double total_cost, bool reachable)
{
    Datum values[3]; bool nulls[3] = {!reachable, !reachable, !reachable};
    if (reachable) {
        values[0] = Float8GetDatum(total_cost);
        std::vector<int64_t> ids;
        ids.reserve(path.size());
        for (NetworKit::node n : path)
            ids.push_back(pg.id_map.resolve(n));
        values[1] = PointerGetDatum(build_bigint_array(ids));
        values[2] = PointerGetDatum(build_float8_array(costs));
    }
    srf_emit(s, values, nulls);
}

/* Build the (path, per-node cumulative cost) pair for a Dijkstra/BFS-family
 * SSSP algorithm (whose getPath() already includes both endpoints and whose
 * getDistances() gives the true shortest distance to every node -- no
 * manual summation needed). */
template <class SSSPAlgo>
static void sssp_path_and_costs(const SSSPAlgo &algo, NetworKit::node target,
                                std::vector<NetworKit::node> &path_out,
                                std::vector<double> &costs_out)
{
    path_out = algo.getPath(target, /* forward */ true);
    costs_out.clear();
    costs_out.reserve(path_out.size());
    for (NetworKit::node n : path_out)
        costs_out.push_back(algo.distance(n));
}

/* ======================================================== */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_bfs_stream);
PG_FUNCTION_INFO_V1(age_gds_shortestpath_dijkstra_stream);
PG_FUNCTION_INFO_V1(age_gds_allshortestpaths_dijkstra_stream);
PG_FUNCTION_INFO_V1(age_gds_deltastepping_stream);
PG_FUNCTION_INFO_V1(age_gds_bellmanford_stream);
PG_FUNCTION_INFO_V1(age_gds_bellmanford_stats);
PG_FUNCTION_INFO_V1(age_gds_dfs_stream);
PG_FUNCTION_INFO_V1(age_gds_randomwalk_stream);
PG_FUNCTION_INFO_V1(age_gds_spanningtree_stream);
PG_FUNCTION_INFO_V1(age_gds_allshortestpaths_stream);
PG_FUNCTION_INFO_V1(age_gds_maxflow_stats);
PG_FUNCTION_INFO_V1(age_gds_shortestpath_astar_stream);
PG_FUNCTION_INFO_V1(age_gds_mincostmaxflow_stats);

/* ---------------------------------------------- bfs_stream --------------- */
/* Args: 0=graph_name, 1=source_node, 2=age_graph, 3=node_query, 4=edge_query,
 *       5=max_depth, 6=target_nodes */

Datum age_gds_bfs_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    int64_t max_depth = PG_ARGISNULL(5) ? -1 : PG_GETARG_INT32(5);

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    if (!PG_ARGISNULL(6)) {
        /* target_nodes given: NetworKit::MultiTargetBFS computes distances
         * only to the requested targets (early termination is built into
         * the class), matching the plan's intent of not walking the whole
         * graph when only a handful of targets are wanted. */
        std::vector<int64_t> target_gids = parse_bigint_array(PG_GETARG_DATUM(6));
        std::vector<NetworKit::node> targets = resolve_node_array(target_gids, pg, "target_nodes");

        NetworKit::MultiTargetBFS algo(pg.graph, src, targets.begin(), targets.end());
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("MultiTargetBFS failed: %s", ex.what())));
        }

        const auto &dists    = algo.getDistances();
        const auto &tidx     = algo.getTargetIndexMap();

        for (size_t i = 0; i < targets.size(); i++) {
            NetworKit::node t = targets[i];
            auto it = tidx.find(t);
            double d = (it != tidx.end()) ? dists[it->second]
                                          : std::numeric_limits<double>::max();
            bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0) ||
                          (max_depth >= 0 && d > (double)max_depth);
            Datum values[2]; bool nulls[2] = {false, is_inf};
            values[0] = Int64GetDatum(target_gids[i]);
            if (!is_inf) values[1] = Float8GetDatum(d);
            srf_emit(s, values, nulls);
        }
    } else {
        /* No target list: plain BFS over the whole graph via NetworKit's
         * Traversal::BFSfrom (distance-aware node visitor), with max_depth
         * enforced as a post-visit guard on which nodes get emitted. */
        std::vector<int64_t> dist(pg.graph.upperNodeIdBound(), -1);
        NetworKit::Traversal::BFSfrom(pg.graph, src,
            [&](NetworKit::node u, NetworKit::count d) {
                if (max_depth < 0 || (int64_t)d <= max_depth)
                    dist[u] = (int64_t)d;
            });

        pg.graph.forNodes([&](NetworKit::node u) {
            Datum values[2]; bool nulls[2] = {false, dist[u] < 0};
            values[0] = Int64GetDatum(pg.id_map.resolve(u));
            if (dist[u] >= 0) values[1] = Float8GetDatum((double)dist[u]);
            srf_emit(s, values, nulls);
        });
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- shortestPath_dijkstra ---- */
/* Args: 0=graph_name, 1=source_node, 2=target_node, 3=age_graph,
 *       4=node_query, 5=edge_query, 6=relationship_weight_property
 *
 * BREAKING CHANGE (per plan 03): this used to be a single-source-to-all
 * function returning (nodeId, distance) rows. Neo4j GDS splits
 * shortestPath.dijkstra (single pair, path output) from
 * allShortestPaths.dijkstra (single source, all targets, path output) --
 * this function now matches the former; see
 * age_gds_allshortestpaths_dijkstra_stream for the latter. */

Datum age_gds_shortestpath_dijkstra_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 3);
    const char *wprop = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::node src = get_node_arg(fcinfo, 1, pg, "source_node");
    NetworKit::node tgt = get_node_arg(fcinfo, 2, pg, "target_node");

    NetworKit::Dijkstra algo(pg.graph, src, /* storePaths */ true,
                             /* storeNodesSortedByDistance */ false, tgt);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Dijkstra failed: %s", ex.what())));
    }

    Oid types[3] = {FLOAT8OID, INT8ARRAYOID, FLOAT8ARRAYOID};
    SRFState s = srf_begin(fcinfo, 3, types);

    double d = algo.distance(tgt);
    bool reachable = d < std::numeric_limits<double>::max() / 2.0;
    std::vector<NetworKit::node> path;
    std::vector<double> costs;
    if (reachable)
        sssp_path_and_costs(algo, tgt, path, costs);

    emit_path_row(s, pg, path, costs, d, reachable);
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- allShortestPaths_dijkstra */
/* Args: 0=graph_name, 1=source_node, 2=age_graph, 3=node_query,
 *       4=edge_query, 5=relationship_weight_property, 6=target_nodes
 *
 * NEW function (matches Neo4j's gds.allShortestPaths.dijkstra.stream):
 * single source, path output for every reachable node (or, if
 * `target_nodes` is given, just those nodes).
 *
 * Deviation from plan text: the plan suggested NetworKit::MultiTargetDijkstra
 * for the target_nodes case, but that class hardcodes storePred=false (no
 * path reconstruction) -- using it would silently drop the path output that
 * is this function's entire reason for existing. Instead, when target_nodes
 * is given, this runs one NetworKit::Dijkstra per requested target (each
 * with storePaths=true and that target passed as the early-stop `target`
 * arg) -- still 100% delegated to NetworKit::Dijkstra, just invoked once per
 * requested target rather than once for the whole graph. */

Datum age_gds_allshortestpaths_dijkstra_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const char *wprop = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::node src = get_node_arg(fcinfo, 1, pg, "source_node");

    Oid types[4] = {INT8OID, FLOAT8OID, INT8ARRAYOID, FLOAT8ARRAYOID};
    SRFState s = srf_begin(fcinfo, 4, types);

    auto emit_target = [&](NetworKit::node t, const NetworKit::Dijkstra &algo) {
        double d = algo.distance(t);
        bool reachable = d < std::numeric_limits<double>::max() / 2.0;
        Datum values[4]; bool nulls[4] = {false, !reachable, !reachable, !reachable};
        values[0] = Int64GetDatum(pg.id_map.resolve(t));
        if (reachable) {
            values[1] = Float8GetDatum(d);
            std::vector<NetworKit::node> path;
            std::vector<double> costs;
            sssp_path_and_costs(algo, t, path, costs);
            std::vector<int64_t> ids;
            ids.reserve(path.size());
            for (NetworKit::node n : path) ids.push_back(pg.id_map.resolve(n));
            values[2] = PointerGetDatum(build_bigint_array(ids));
            values[3] = PointerGetDatum(build_float8_array(costs));
        }
        srf_emit(s, values, nulls);
    };

    if (!PG_ARGISNULL(6)) {
        std::vector<int64_t> target_gids = parse_bigint_array(PG_GETARG_DATUM(6));
        std::vector<NetworKit::node> targets = resolve_node_array(target_gids, pg, "target_nodes");
        for (NetworKit::node t : targets) {
            NetworKit::Dijkstra algo(pg.graph, src, true, false, t);
            try { algo.run(); }
            catch (const std::exception &ex) {
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Dijkstra failed: %s", ex.what())));
            }
            emit_target(t, algo);
        }
    } else {
        NetworKit::Dijkstra algo(pg.graph, src, true, false);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("Dijkstra failed: %s", ex.what())));
        }
        pg.graph.forNodes([&](NetworKit::node t) { emit_target(t, algo); });
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- deltaStepping_stream ----- */
// Out of scope for plan 03 (no NetworKit::DeltaStepping class); unchanged
// Dijkstra fallback for non-negative weight graphs.

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

/* ---------------------------------------------- bellmanFord -------------- */
/* Args: 0=graph_name, 1=source_node, 2=age_graph, 3=node_query,
 *       4=edge_query, 5=relationship_weight_property
 *
 * Swaps the previous Dijkstra substitution for the real
 * NetworKit::AlgebraicBellmanFord<CSRMatrix>, which supports negative edge
 * weights and reports negative-cycle presence -- the whole reason Neo4j
 * offers Bellman-Ford instead of always using Dijkstra. */

static void run_bellman_ford(const ProjectedGraph &pg, NetworKit::node src,
                             NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix> **out)
{
    *out = new NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix>(pg.graph, src);
    try { (*out)->run(); }
    catch (const std::exception &ex) {
        delete *out;
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AlgebraicBellmanFord failed: %s", ex.what())));
    }
}

Datum age_gds_bellmanford_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const char *wprop = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix> *bf = nullptr;
    run_bellman_ford(pg, src, &bf);
    bool neg_cycle = bf->hasNegativeCycle();

    Oid types[3] = {INT8OID, FLOAT8OID, BOOLOID};
    SRFState s = srf_begin(fcinfo, 3, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        double d = bf->distance(u);
        bool is_inf = std::isinf(d);
        Datum values[3]; bool nulls[3] = {false, is_inf, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        if (!is_inf) values[1] = Float8GetDatum(d);
        values[2] = BoolGetDatum(neg_cycle);
        srf_emit(s, values, nulls);
    });
    delete bf;
    return srf_end(s, fcinfo);
}

/* bellmanFord_stats: companion single-row stats function, same args. */
Datum age_gds_bellmanford_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const char *wprop = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    auto t0 = std::chrono::steady_clock::now();
    NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix> *bf = nullptr;
    run_bellman_ford(pg, src, &bf);
    auto t1 = std::chrono::steady_clock::now();
    bool neg_cycle = bf->hasNegativeCycle();
    delete bf;

    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Oid types[2] = {BOOLOID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    Datum values[2]; bool nulls[2] = {false, false};
    values[0] = BoolGetDatum(neg_cycle);
    values[1] = Int64GetDatum(ms);
    srf_emit(s, values, nulls);
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- dfs_stream --------------- */
/* Args: 0=graph_name, 1=source_node, 2=age_graph, 3=node_query,
 *       4=edge_query, 5=max_depth, 6=target_nodes
 *
 * Uses NetworKit::Traversal::DFSEdgesFrom (already the pre-existing
 * delegate). max_depth/target_nodes are applied as post-traversal filters
 * on the already-fully-computed depth map, rather than an early-exit
 * inside the traversal -- NetworKit's DFSEdgesFrom template has no
 * early-termination hook, so honoring "stop once a target is hit" exactly
 * would require a hand-rolled DFS loop, which project rules forbid. The
 * filtered result is identical to what an early-exit traversal would
 * produce (all depths below the cut are computed the same way either way);
 * only the (here, irrelevant for correctness) work-avoidance benefit of
 * stopping early is not realized. */

Datum age_gds_dfs_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 2);
    const ProjectedGraph &pg = *gh.pg;
    NetworKit::node src = get_source_node(fcinfo, pg);

    int64_t max_depth = PG_ARGISNULL(5) ? -1 : PG_GETARG_INT32(5);

    std::unordered_set<NetworKit::node> target_set;
    bool has_targets = !PG_ARGISNULL(6);
    if (has_targets) {
        std::vector<int64_t> target_gids = parse_bigint_array(PG_GETARG_DATUM(6));
        for (NetworKit::node n : resolve_node_array(target_gids, pg, "target_nodes"))
            target_set.insert(n);
    }

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
        if (depth[u] < 0) return;
        if (max_depth >= 0 && depth[u] > max_depth) return;
        if (has_targets && target_set.find(u) == target_set.end()) return;

        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int32GetDatum((int32_t)depth[u]);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- randomWalk_stream -------- */
/* Args: 0=graph_name, 1=source_nodes, 2=walk_length, 3=age_graph,
 *       4=node_query, 5=edge_query, 6=walks_per_node, 7=in_out_factor,
 *       8=return_factor, 9=relationship_weight_property
 *
 * BREAKING CHANGE (per plan 03): source_node -> source_nodes bigint[]
 * (NULL = every node, matching Neo4j's default), walk_length default
 * 10 -> 80. Delegates to NetworKit::Embedding::BiasedRandomWalk (see the
 * file-header comment on its #include above) instead of hand-rolled
 * Aux::Random::choice, picking up in_out_factor/return_factor (Q/P) and
 * relationship_weight_property (BiasedRandomWalk's preprocessNode() reads
 * edge weights directly -- confirmed by reading
 * networkit/cpp/embedding/BiasedRandomWalk.cpp). */

Datum age_gds_randomwalk_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 3);
    const char *wprop = PG_ARGISNULL(9) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(9));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    if (pg.graph.numberOfNodes() != pg.graph.upperNodeIdBound())
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("randomWalk_stream requires contiguous node ids")));

    int walk_length    = PG_ARGISNULL(2) ? 80 : PG_GETARG_INT32(2);
    int walks_per_node = PG_ARGISNULL(6) ? 10 : PG_GETARG_INT32(6);
    double in_out_factor = PG_ARGISNULL(7) ? 1.0 : PG_GETARG_FLOAT8(7); /* Q */
    double return_factor = PG_ARGISNULL(8) ? 1.0 : PG_GETARG_FLOAT8(8); /* P */
    if (walk_length < 1) walk_length = 1;
    if (walks_per_node < 1) walks_per_node = 1;

    std::unordered_set<NetworKit::node> source_set;
    bool has_sources = !PG_ARGISNULL(1);
    if (has_sources) {
        std::vector<int64_t> gids = parse_bigint_array(PG_GETARG_DATUM(1));
        for (NetworKit::node n : resolve_node_array(gids, pg, "source_nodes"))
            source_set.insert(n);
    }

    NetworKit::Embedding::BiasedRandomWalk bw(&pg.graph);
    NetworKit::Embedding::BiasedRandomWalk::AllWalks walks;
    try {
        bw.preprocessTransitionProbs(return_factor, in_out_factor);
        walks = bw.doWalks((NetworKit::count)walk_length, (NetworKit::count)walks_per_node);
    }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("BiasedRandomWalk failed: %s", ex.what())));
    }

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    int64_t walk_id = 0;
    for (const auto &walk : walks) {
        if (walk.empty()) continue;
        if (has_sources && source_set.find(walk.front()) == source_set.end())
            continue;
        for (NetworKit::node u : walk) {
            Datum values[2]; bool nulls[2] = {false, false};
            values[0] = Int64GetDatum(walk_id);
            values[1] = Int64GetDatum(pg.id_map.resolve(u));
            srf_emit(s, values, nulls);
        }
        walk_id++;
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- spanningTree_stream ------ */
/* Args: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
 *       4=objective, 5=relationship_weight_property, 6=source_node */

Datum age_gds_spanningtree_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const char *wprop = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    std::string objective = PG_ARGISNULL(4) ? "minimum" : text_to_cstring(PG_GETARG_TEXT_PP(4));
    if (objective != "minimum" && objective != "maximum")
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("objective must be 'minimum' or 'maximum', got '%s'",
                               objective.c_str())));

    /* source_node is validation-only: NetworKit::RandomMaximumSpanningForest
     * is root-independent (Kruskal's algorithm has no notion of a starting
     * node), so this parameter cannot change which edges come back -- it
     * only lets a caller assert the node exists in the graph. */
    if (!PG_ARGISNULL(6))
        (void)get_node_arg(fcinfo, 6, pg, "source_node");

    /* indexEdges() only assigns sequential ids without altering topology;
     * const_cast is safe (same pattern as maxFlow_stats below). Force
     * re-indexing (not just "index if not already indexed") -- the
     * 'minimum' branch below sizes neg_weights by numberOfEdges() and
     * indexes it by eid, and a cached graph that was indexed by an earlier
     * call (e.g. maxFlow_stats) and later had edges added via the
     * live-index path without re-indexing could otherwise have stale,
     * non-contiguous edge ids (NetworKit::RandomMaximumSpanningForest's own
     * header documents this hazard). */
    NetworKit::Graph &g = const_cast<NetworKit::Graph &>(pg.graph);
    g.indexEdges(true);

    NetworKit::Graph forest;
    try {
        if (objective == "maximum") {
            NetworKit::RandomMaximumSpanningForest sf(pg.graph);
            sf.run();
            forest = sf.getMSF(true);
        } else {
            /* Standard MST-via-MSF trick: negate edge weights so that
             * "maximum" over -weight is "minimum" over weight. */
            std::vector<double> neg_weights(g.numberOfEdges());
            g.forEdges([&](NetworKit::node, NetworKit::node, NetworKit::edgeweight w,
                          NetworKit::edgeid eid) { neg_weights[eid] = -w; });
            NetworKit::RandomMaximumSpanningForest sf(pg.graph, neg_weights);
            sf.run();
            forest = sf.getMSF(true);
        }
    }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("RandomMaximumSpanningForest failed: %s", ex.what())));
    }

    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 3, types);

    forest.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
        Datum values[3]; bool nulls[3] = {false, false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(pg.id_map.resolve(v));
        /* Report the original (non-negated) weight regardless of
         * objective. */
        values[2] = Float8GetDatum(objective == "maximum" ? w : pg.graph.weight(u, v));
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- allShortestPaths_stream -- */
/* Args: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
 *       4=relationship_weight_property */

Datum age_gds_allshortestpaths_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const char *wprop = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    apply_weight_override(gh, wprop);
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
/* Args: 0=graph_name, 1=source_node, 2=target_node, 3=age_graph,
 *       4=node_query, 5=edge_query, 6=capacity_property, 7=source_nodes,
 *       8=target_nodes, 9=node_capacity_property */

Datum age_gds_maxflow_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 3);
    /* capacity_property reuses the exact same "named edge property ->
     * graph weight" plumbing as relationship_weight_property elsewhere
     * (plan 00) -- EdmondsKarp/Dinic already treat graph weight as
     * capacity, so there is nothing capacity-specific to add here. */
    const char *capprop = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    apply_weight_override(gh, capprop);
    const ProjectedGraph &pg = *gh.pg;

    bool has_lists = !PG_ARGISNULL(7) || !PG_ARGISNULL(8);
    bool has_node_cap = !PG_ARGISNULL(9);

    auto t0 = std::chrono::steady_clock::now();
    double flow;

    if (!has_lists && !has_node_cap) {
        /* Backward-compatible single source/sink path: unchanged
         * behavior. */
        NetworKit::node nk_src  = get_node_arg(fcinfo, 1, pg, "source_node");
        NetworKit::node nk_sink = get_node_arg(fcinfo, 2, pg, "target_node");
        if (nk_src == nk_sink)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("source_node and target_node must be different")));

        NetworKit::Graph &g = const_cast<NetworKit::Graph &>(pg.graph);
        g.indexEdges();

        NetworKit::EdmondsKarp ek(pg.graph, nk_src, nk_sink);
        try { ek.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("EdmondsKarp failed: %s", ex.what())));
        }
        flow = ek.getMaxFlow();
    } else {
        if (!pg.graph.isDirected())
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("maxFlow_stats: source_nodes/target_nodes/"
                                   "node_capacity_property require a directed graph")));

        std::vector<NetworKit::node> sources, sinks;
        if (!PG_ARGISNULL(7))
            sources = resolve_node_array(parse_bigint_array(PG_GETARG_DATUM(7)), pg, "source_nodes");
        else
            sources.push_back(get_node_arg(fcinfo, 1, pg, "source_node"));
        if (!PG_ARGISNULL(8))
            sinks = resolve_node_array(parse_bigint_array(PG_GETARG_DATUM(8)), pg, "target_nodes");
        else
            sinks.push_back(get_node_arg(fcinfo, 2, pg, "target_node"));

        if (sources.empty() || sinks.empty())
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("maxFlow_stats: source_nodes/target_nodes must be non-empty")));

        const NetworKit::Graph *working = &pg.graph;
        NetworKit::Graph split_graph;
        std::vector<NetworKit::node> eff_sources = sources, eff_sinks = sinks;

        if (has_node_cap) {
            const char *ncprop = text_to_cstring(PG_GETARG_TEXT_PP(9));
            std::vector<double> node_cap = read_node_property_as_values(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(), ncprop, -1.0);
            std::vector<NodeSplit> split;
            split_graph = split_node_capacities(pg.graph, node_cap, split);
            working = &split_graph;
            eff_sources.clear(); eff_sinks.clear();
            for (NetworKit::node s : sources) eff_sources.push_back(split[s].out_node);
            for (NetworKit::node t : sinks)   eff_sinks.push_back(split[t].in_node);
        }

        AugmentedFlowGraph aug = augment_super_source_sink(*working, eff_sources, eff_sinks);
        NetworKit::EdmondsKarp ek(aug.graph, aug.source, aug.sink);
        try { ek.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("EdmondsKarp failed: %s", ex.what())));
        }
        flow = ek.getMaxFlow();
    }

    auto t1 = std::chrono::steady_clock::now();
    int64_t compute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Oid types[2] = {FLOAT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    Datum values[2]; bool nulls[2] = {false, false};
    values[0] = Float8GetDatum(flow);
    values[1] = Int64GetDatum(compute_ms);
    srf_emit(s, values, nulls);
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- shortestPath_astar_stream */
/* Args: 0=graph_name, 1=source_node, 2=target_node, 3=latitude_property,
 *       4=longitude_property, 5=age_graph, 6=node_query, 7=edge_query,
 *       8=relationship_weight_property
 *
 * NEW function (gds.shortestPath.astar.stream). The haversine heuristic
 * assumes edge weights are in the same units as the great-circle distance
 * (kilometers) -- same modeling assumption Neo4j's own astar implementation
 * makes; it is the caller's responsibility, not something age_gds can
 * verify. */

static double haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double R = 6371.0;
    constexpr double deg2rad = M_PI / 180.0;
    double dlat = (lat2 - lat1) * deg2rad;
    double dlon = (lon2 - lon1) * deg2rad;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2)
             + std::cos(lat1 * deg2rad) * std::cos(lat2 * deg2rad)
               * std::sin(dlon / 2) * std::sin(dlon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return R * c;
}

Datum age_gds_shortestpath_astar_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 5);
    const char *wprop = PG_ARGISNULL(8) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(8));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::node src = get_node_arg(fcinfo, 1, pg, "source_node");
    NetworKit::node tgt = get_node_arg(fcinfo, 2, pg, "target_node");

    if (PG_ARGISNULL(3) || PG_ARGISNULL(4))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("latitude_property and longitude_property are required")));
    const char *lat_prop = text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *lon_prop = text_to_cstring(PG_GETARG_TEXT_PP(4));

    std::vector<double> lat, lon;
    try {
        lat = read_node_property_as_values(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.node_query.c_str(), lat_prop, 0.0);
        lon = read_node_property_as_values(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.node_query.c_str(), lon_prop, 0.0);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("shortestPath_astar_stream: %s", ex.what())));
    }

    NetworKit::count n = pg.graph.upperNodeIdBound();
    std::vector<double> heu(n, 0.0);
    for (NetworKit::node u = 0; u < n; u++)
        if (pg.graph.hasNode(u))
            heu[u] = haversine_km(lat[u], lon[u], lat[tgt], lon[tgt]);

    NetworKit::AStar astar(pg.graph, heu, src, tgt, /* storePred */ true);
    try { astar.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AStar failed: %s", ex.what())));
    }

    Oid types[3] = {FLOAT8OID, INT8ARRAYOID, FLOAT8ARRAYOID};
    SRFState s = srf_begin(fcinfo, 3, types);

    double d = astar.getDistance();
    bool reachable = d < std::numeric_limits<double>::max() / 2.0;
    std::vector<NetworKit::node> path;
    std::vector<double> costs;
    if (reachable) {
        /* STSP::getPath() excludes both endpoints (unlike SSSP::getPath()
         * used by the Dijkstra-family functions above); normalize to the
         * same inclusive-of-endpoints shape by adding them back, and
         * recompute cumulative cost by walking the path and summing real
         * edge weights (AStar does not expose per-node distances, only the
         * total). */
        std::vector<NetworKit::node> middle = astar.getPath();
        path.push_back(src);
        path.insert(path.end(), middle.begin(), middle.end());
        path.push_back(tgt);

        double cum = 0.0;
        costs.push_back(0.0);
        for (size_t i = 1; i < path.size(); i++) {
            cum += pg.graph.weight(path[i - 1], path[i]);
            costs.push_back(cum);
        }
    }
    emit_path_row(s, pg, path, costs, d, reachable);
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- minCostMaxFlow_stats ----- */
/* Args: 0=graph_name, 1=capacity_property, 2=cost_property,
 *       3=source_nodes, 4=target_nodes, 5=age_graph, 6=node_query,
 *       7=edge_query, 8=node_capacity_property
 *
 * NEW function (gds.minCostMaxFlow.stream family -> implemented as _stats,
 * a single aggregate row, since NetworKit::SuccessiveShortestPathMinCostFlow
 * does not expose a natural per-edge/per-path streaming shape beyond
 * total cost). `alpha` (cost-scaling) is not exposed by this NetworKit
 * class and is therefore unsupported, per the plan's exclusion list. */

Datum age_gds_mincostmaxflow_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 5);
    const ProjectedGraph &pg0 = *gh.pg;

    if (!pg0.graph.isDirected())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats requires a directed graph")));

    const char *cost_prop = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    apply_weight_override(gh, cost_prop);   /* graph weight now holds "cost" */
    const ProjectedGraph &pg = *gh.pg;

    if (PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("capacity_property is required")));
    const char *cap_prop = text_to_cstring(PG_GETARG_TEXT_PP(1));
    if (pg.age_graph.empty() || pg.edge_query.empty())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats: graph must have age_graph/edge_query "
                               "recorded (re-run graph_project/graph_index)")));

    NetworKit::Graph cap_graph;
    try {
        cap_graph = reweight_from_property(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.edge_query.c_str(), cap_prop);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats: capacity_property: %s", ex.what())));
    }

    if (PG_ARGISNULL(3) || PG_ARGISNULL(4))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_nodes and target_nodes are required")));
    std::vector<NetworKit::node> sources =
        resolve_node_array(parse_bigint_array(PG_GETARG_DATUM(3)), pg, "source_nodes");
    std::vector<NetworKit::node> sinks =
        resolve_node_array(parse_bigint_array(PG_GETARG_DATUM(4)), pg, "target_nodes");
    if (sources.empty() || sinks.empty())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source_nodes/target_nodes must be non-empty")));

    /* Collect (u, v, capacity) for every "real" edge before any topology
     * change, keyed by original node ids (or split in/out ids, see below),
     * so the capacity attribute can be attached once the final topology
     * (after node-splitting and super-source/sink augmentation) is known
     * and indexed. */
    struct EdgeCap { NetworKit::node u, v; double cap; };
    std::vector<EdgeCap> edge_caps;

    NetworKit::Graph work(0, /* weighted */ true, /* directed */ true);
    std::vector<NetworKit::node> eff_sources, eff_sinks;

    if (!PG_ARGISNULL(8)) {
        const char *ncprop = text_to_cstring(PG_GETARG_TEXT_PP(8));
        std::vector<double> node_cap = read_node_property_as_values(
            pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(), ncprop, -1.0);

        NetworKit::count n = pg.graph.upperNodeIdBound();
        std::vector<NodeSplit> split(n, NodeSplit{NetworKit::none, NetworKit::none});
        constexpr double unconstrained = std::numeric_limits<double>::max() / 4.0;
        for (NetworKit::node u = 0; u < n; u++) {
            if (!pg.graph.hasNode(u)) continue;
            NetworKit::node uin  = work.addNode();
            NetworKit::node uout = work.addNode();
            split[u] = NodeSplit{uin, uout};
            work.addEdge(uin, uout, /* cost */ 0.0);
            double cap = (u < node_cap.size() && node_cap[u] > 0.0) ? node_cap[u] : unconstrained;
            edge_caps.push_back({uin, uout, cap});
        }
        pg.graph.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
            work.addEdge(split[u].out_node, split[v].in_node, w);
            double cap = cap_graph.hasEdge(u, v) ? cap_graph.weight(u, v) : unconstrained;
            edge_caps.push_back({split[u].out_node, split[v].in_node, cap});
        });
        for (NetworKit::node s : sources) eff_sources.push_back(split[s].out_node);
        for (NetworKit::node t : sinks)   eff_sinks.push_back(split[t].in_node);
    } else {
        NetworKit::count n = pg.graph.upperNodeIdBound();
        for (NetworKit::node u = 0; u < n; u++)
            if (pg.graph.hasNode(u)) work.addNode();
        pg.graph.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
            work.addEdge(u, v, w);
            double cap = cap_graph.hasEdge(u, v) ? cap_graph.weight(u, v) : 0.0;
            edge_caps.push_back({u, v, cap});
        });
        eff_sources = sources;
        eff_sinks   = sinks;
    }

    /* Super-source/super-sink for the multi-source/multi-sink case; cost of
     * every synthetic edge is 0, capacity is "unconstrained" (their real
     * capacity comes entirely from the real edges they route through). */
    constexpr double unconstrained = std::numeric_limits<double>::max() / 4.0;
    NetworKit::node source_id, sink_id;
    if (eff_sources.size() != 1) {
        source_id = work.addNode();
        for (NetworKit::node s : eff_sources) {
            work.addEdge(source_id, s, 0.0);
            edge_caps.push_back({source_id, s, unconstrained});
        }
    } else {
        source_id = eff_sources[0];
    }
    if (eff_sinks.size() != 1) {
        sink_id = work.addNode();
        for (NetworKit::node t : eff_sinks) {
            work.addEdge(t, sink_id, 0.0);
            edge_caps.push_back({t, sink_id, unconstrained});
        }
    } else {
        sink_id = eff_sinks[0];
    }

    if (source_id == sink_id)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source and target must resolve to different nodes")));

    work.indexEdges(true);

    /* Determine the max flow F to route (min-cost MAX flow) by running
     * EdmondsKarp on a same-topology, capacity-weighted copy. */
    NetworKit::Graph cap_work(0, true, true);
    NetworKit::count wn = work.upperNodeIdBound();
    for (NetworKit::node u = 0; u < wn; u++)
        if (work.hasNode(u)) cap_work.addNode();
    for (const auto &ec : edge_caps)
        cap_work.addEdge(ec.u, ec.v, ec.cap);
    cap_work.indexEdges(true);

    double F = 0.0;
    try {
        NetworKit::EdmondsKarp ek(cap_work, source_id, sink_id);
        ek.run();
        F = ek.getMaxFlow();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("EdmondsKarp (max-flow prepass) failed: %s", ex.what())));
    }

    auto cap_attr = work.attachEdgeDoubleAttribute("capacity");
    for (const auto &ec : edge_caps)
        cap_attr.set(work.edgeId(ec.u, ec.v), ec.cap);

    auto supply_attr = work.attachNodeDoubleAttribute("supply");
    work.forNodes([&](NetworKit::node u) { supply_attr.set(u, 0.0); });
    supply_attr.set(source_id, F);
    supply_attr.set(sink_id, -F);

    auto t0 = std::chrono::steady_clock::now();
    double total_cost = 0.0;
    try {
        NetworKit::SuccessiveShortestPathMinCostFlow mcf(work, "capacity", "supply");
        mcf.run();
        total_cost = mcf.getTotalCost();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("SuccessiveShortestPathMinCostFlow failed: %s", ex.what())));
    }
    auto t1 = std::chrono::steady_clock::now();
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Oid types[2] = {FLOAT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    Datum values[2]; bool nulls[2] = {false, false};
    values[0] = Float8GetDatum(total_cost);
    values[1] = Int64GetDatum(ms);
    srf_emit(s, values, nulls);
    return srf_end(s, fcinfo);
}

} /* extern "C" */
