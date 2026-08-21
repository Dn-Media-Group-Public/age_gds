// src/algorithms/pathfinding.cpp
//
// Path-finding SQL functions:
//   bfs, dfs, shortestPath.dijkstra, allShortestPaths.dijkstra,
//   deltaStepping (->Dijkstra fallback), bellmanFord (AlgebraicBellmanFord),
//   randomWalk, spanningTree, allShortestPaths, maxFlow,
//   shortestPath.astar, minCostMaxFlow.
//
// See docs/implemented-plans/03-pathfinding-parity.md for the design
// decisions behind every parameter added in this file, and
// docs/implemented-plans/06-new-graph-procedures.md for topologicalSort,
// longestPath and bridges.
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
#include <networkit/graph/TopologicalSort.hpp>
#include <networkit/components/BiconnectedComponents.hpp>
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
#include <memory>
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
#include "util/cypher_agtype.h"
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

/* Thrown from inside a NetworKit::Traversal::BFSfrom handle to abort the
 * traversal early once nothing further needs visiting (all targets found,
 * or -- since BFS visits nodes in non-decreasing depth order -- once depth
 * exceeds max_depth, meaning every remaining queued node is too deep too).
 * Shared by age_gds_bfs_stream and age_gds_bfs_stream_cypher. */
struct BfsStopException {};

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
PG_FUNCTION_INFO_V1(age_gds_topologicalsort_stream);
PG_FUNCTION_INFO_V1(age_gds_longestpath_stream);
PG_FUNCTION_INFO_V1(age_gds_bridges_stream);

/* Resolve a single AGE graphid (from a Cypher config value) to a NetworKit
 * node id. Shared by every Cypher-callable wrapper below. */
static NetworKit::node get_node_from_gid(const ProjectedGraph &pg, int64_t gid, const char *argname)
{
    auto it = pg.id_map.graphid_to_nk.find(gid);
    if (it == pg.id_map.graphid_to_nk.end())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s %" PRId64 " not in graph", argname, gid)));
    return it->second;
}

/* Cypher-callable equivalent of emit_path_row(): add {totalCost, nodeIds,
 * costs} to the current row (all null when !reachable). */
static void add_path_row_cypher(CypherResultBuilder &b, const ProjectedGraph &pg,
                                const std::vector<NetworKit::node> &path,
                                const std::vector<double> &costs,
                                double total_cost, bool reachable)
{
    b.key("totalCost");
    if (reachable) b.add_double(total_cost); else b.add_null();
    b.key("nodeIds");
    if (reachable) {
        std::vector<int64_t> ids;
        ids.reserve(path.size());
        for (NetworKit::node n : path) ids.push_back(pg.id_map.resolve(n));
        b.add_int64_array(ids);
    } else b.add_null();
    b.key("costs");
    if (reachable) b.add_double_array(costs); else b.add_null();
}

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
         * Traversal::BFSfrom, bounded by max_depth via early termination
         * (BFS visits nodes in non-decreasing depth order, so once d
         * exceeds max_depth every node still queued is too deep as well --
         * see BfsStopException's comment) and emitting only nodes actually
         * visited, rather than every node in the graph.
         *
         * BREAKING CHANGE (per docs/implemented-plans/bfs-stream-depth-bound-and-sparse-emit.md):
         * previously emitted one row per graph node, with `distance` NULL
         * for unreached nodes; now only reached nodes are emitted and
         * `distance` is never NULL. */
        std::vector<std::pair<NetworKit::node, int64_t>> visited;
        try {
            NetworKit::Traversal::BFSfrom(pg.graph, src,
                [&](NetworKit::node u, NetworKit::count d) {
                    if (max_depth >= 0 && (int64_t)d > max_depth)
                        throw BfsStopException{};
                    visited.emplace_back(u, (int64_t)d);
                });
        } catch (const BfsStopException &) {
            /* intentional early stop: max_depth exceeded */
        }

        for (const auto &[u, d] : visited) {
            Datum values[2]; bool nulls[2] = {false, false};
            values[0] = Int64GetDatum(pg.id_map.resolve(u));
            values[1] = Float8GetDatum((double)d);
            srf_emit(s, values, nulls);
        }
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- bfs_stream (Cypher) ------ */
/* age_gds.bfs_stream(graphName agtype, config agtype) RETURNS agtype --
 * JSON array with one element: {sourceNode, nodeIds}. No `path` field (see
 * docs/api.md: fabricated NEXT relationships carry no extra information).
 * config.targetNodes triggers early termination once every target has been
 * visited (Neo4j's real semantics) -- nodeIds is then the (possibly
 * reduced) visited-node list, unlike the SQL bfs_stream's per-target
 * distance output (that stays SQL-only under its `target_nodes` param). */

PG_FUNCTION_INFO_V1(age_gds_bfs_stream_cypher);

Datum age_gds_bfs_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"sourceNode", "maxDepth", "targetNodes"});
    const ProjectedGraph &pg = *gh.pg;

    int64_t source_gid;
    if (!cfg.get_int64("sourceNode", source_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode is required")));
    NetworKit::node src = get_node_from_gid(pg, source_gid, "sourceNode");

    int max_depth = -1;
    cfg.get_int("maxDepth", max_depth);

    std::vector<int64_t> target_gids;
    bool has_targets = cfg.get_int64_array("targetNodes", target_gids);
    std::unordered_set<NetworKit::node> targets;
    if (has_targets)
        for (NetworKit::node n : resolve_node_array(target_gids, pg, "config.targetNodes"))
            targets.insert(n);

    std::vector<int64_t> visited;
    size_t remaining = targets.size();
    try {
        NetworKit::Traversal::BFSfrom(pg.graph, src,
            [&](NetworKit::node u, NetworKit::count d) {
                if (max_depth >= 0 && (int64_t)d > max_depth)
                    throw BfsStopException{};
                visited.push_back(pg.id_map.resolve(u));
                if (has_targets && targets.count(u) && --remaining == 0)
                    throw BfsStopException{};
            });
    } catch (const BfsStopException &) {
        /* intentional early stop: every target visited */
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("bfs_stream failed: %s", ex.what())));
    }

    CypherResultBuilder b(/* as_array */ true);
    b.begin_row();
    b.key("sourceNode"); b.add_int64(source_gid);
    b.key("nodeIds");    b.add_int64_array(visited);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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

/* ------------------------------------ shortestPath_dijkstra (Cypher) ----- */
/* config: sourceNode, targetNode, relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_shortestpath_dijkstra_stream_cypher);

Datum age_gds_shortestpath_dijkstra_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"sourceNode", "targetNode", "relationshipWeightProperty"});

    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    int64_t src_gid, tgt_gid;
    if (!cfg.get_int64("sourceNode", src_gid) || !cfg.get_int64("targetNode", tgt_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode and config.targetNode are required")));
    NetworKit::node src = get_node_from_gid(pg, src_gid, "sourceNode");
    NetworKit::node tgt = get_node_from_gid(pg, tgt_gid, "targetNode");

    NetworKit::Dijkstra algo(pg.graph, src, true, false, tgt);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Dijkstra failed: %s", ex.what())));
    }

    double d = algo.distance(tgt);
    bool reachable = d < std::numeric_limits<double>::max() / 2.0;
    std::vector<NetworKit::node> path;
    std::vector<double> costs;
    if (reachable)
        sssp_path_and_costs(algo, tgt, path, costs);

    CypherResultBuilder b(true);
    b.begin_row();
    add_path_row_cypher(b, pg, path, costs, d, reachable);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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

/* ------------------------------- allShortestPaths_dijkstra (Cypher) ------ */
/* config: sourceNode, relationshipWeightProperty, targetNodes */

PG_FUNCTION_INFO_V1(age_gds_allshortestpaths_dijkstra_stream_cypher);

Datum age_gds_allshortestpaths_dijkstra_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"sourceNode", "relationshipWeightProperty", "targetNodes"});

    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    int64_t src_gid;
    if (!cfg.get_int64("sourceNode", src_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode is required")));
    NetworKit::node src = get_node_from_gid(pg, src_gid, "sourceNode");

    CypherResultBuilder b(true);
    auto emit_target = [&](NetworKit::node t, const NetworKit::Dijkstra &algo) {
        double d = algo.distance(t);
        bool reachable = d < std::numeric_limits<double>::max() / 2.0;
        std::vector<NetworKit::node> path;
        std::vector<double> costs;
        if (reachable) sssp_path_and_costs(algo, t, path, costs);
        b.begin_row();
        b.key("targetNodeId"); b.add_int64(pg.id_map.resolve(t));
        add_path_row_cypher(b, pg, path, costs, d, reachable);
        b.end_row();
    };

    std::vector<int64_t> target_gids;
    if (cfg.get_int64_array("targetNodes", target_gids)) {
        for (NetworKit::node t : resolve_node_array(target_gids, pg, "config.targetNodes")) {
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
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- deltaStepping (Cypher) --- */
/* config: sourceNode */

PG_FUNCTION_INFO_V1(age_gds_deltastepping_stream_cypher);

Datum age_gds_deltastepping_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"sourceNode"});
    const ProjectedGraph &pg = *gh.pg;

    int64_t src_gid;
    if (!cfg.get_int64("sourceNode", src_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode is required")));
    NetworKit::node src = get_node_from_gid(pg, src_gid, "sourceNode");

    NetworKit::Dijkstra algo(pg.graph, src, false);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("deltaStepping (Dijkstra fallback) failed: %s", ex.what())));
    }
    const auto &dists = algo.getDistances();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        double d = dists[u];
        bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("distance"); if (is_inf) b.add_null(); else b.add_double(d);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
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

/* Shared core for bellmanFord's two Cypher wrappers: resolve source, run
 * AlgebraicBellmanFord. Caller owns/deletes the returned pointer. */
static NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix> *bellman_ford_cypher_core(
    FunctionCallInfo fcinfo, GraphHandle &gh, const ProjectedGraph *&pg_out, int64_t &src_gid_out)
{
    CypherConfig cfg;
    gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"sourceNode", "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    pg_out = gh.pg;

    if (!cfg.get_int64("sourceNode", src_gid_out))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode is required")));
    NetworKit::node src = get_node_from_gid(*pg_out, src_gid_out, "sourceNode");

    NetworKit::AlgebraicBellmanFord<NetworKit::CSRMatrix> *bf = nullptr;
    run_bellman_ford(*pg_out, src, &bf);
    return bf;
}

/* ---------------------------------------------- bellmanFord_stream (Cypher) */

PG_FUNCTION_INFO_V1(age_gds_bellmanford_stream_cypher);

Datum age_gds_bellmanford_stream_cypher(PG_FUNCTION_ARGS)
{
    GraphHandle gh;
    const ProjectedGraph *pg;
    int64_t src_gid;
    auto *bf = bellman_ford_cypher_core(fcinfo, gh, pg, src_gid);
    bool neg_cycle = bf->hasNegativeCycle();

    CypherResultBuilder b(true);
    pg->graph.forNodes([&](NetworKit::node u) {
        double d = bf->distance(u);
        bool is_inf = std::isinf(d);
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg->id_map.resolve(u));
        b.key("distance"); if (is_inf) b.add_null(); else b.add_double(d);
        b.key("isNegativeCycle"); b.add_bool(neg_cycle);
        b.end_row();
    });
    delete bf;
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- bellmanFord_stats (Cypher) */

PG_FUNCTION_INFO_V1(age_gds_bellmanford_stats_cypher);

Datum age_gds_bellmanford_stats_cypher(PG_FUNCTION_ARGS)
{
    auto t0 = std::chrono::steady_clock::now();
    GraphHandle gh;
    const ProjectedGraph *pg;
    int64_t src_gid;
    auto *bf = bellman_ford_cypher_core(fcinfo, gh, pg, src_gid);
    auto t1 = std::chrono::steady_clock::now();
    bool neg_cycle = bf->hasNegativeCycle();
    delete bf;
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("containsNegativeCycle"); b.add_bool(neg_cycle);
    b.key("computeMillis"); b.add_int64(ms);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- dfs_stream (Cypher) ------ */
/* config: sourceNode, maxDepth, targetNodes */

PG_FUNCTION_INFO_V1(age_gds_dfs_stream_cypher);

Datum age_gds_dfs_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"sourceNode", "maxDepth", "targetNodes"});
    const ProjectedGraph &pg = *gh.pg;

    int64_t src_gid;
    if (!cfg.get_int64("sourceNode", src_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode is required")));
    NetworKit::node src = get_node_from_gid(pg, src_gid, "sourceNode");

    int max_depth = -1;
    cfg.get_int("maxDepth", max_depth);

    std::vector<int64_t> target_gids;
    bool has_targets = cfg.get_int64_array("targetNodes", target_gids);
    std::unordered_set<NetworKit::node> target_set;
    if (has_targets)
        for (NetworKit::node n : resolve_node_array(target_gids, pg, "config.targetNodes"))
            target_set.insert(n);

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

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        if (depth[u] < 0) return;
        if (max_depth >= 0 && depth[u] > max_depth) return;
        if (has_targets && target_set.find(u) == target_set.end()) return;
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("depth"); b.add_int64(depth[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- randomWalk_stream (Cypher) */
/* config: sourceNodes, walkLength, walksPerNode, inOutFactor, returnFactor,
 * relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_randomwalk_stream_cypher);

Datum age_gds_randomwalk_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"sourceNodes", "walkLength", "walksPerNode", "inOutFactor", "returnFactor",
         "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    if (pg.graph.numberOfNodes() != pg.graph.upperNodeIdBound())
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("randomWalk_stream requires contiguous node ids")));

    int walk_length = 80, walks_per_node = 10;
    double in_out_factor = 1.0, return_factor = 1.0;
    cfg.get_int("walkLength", walk_length);
    cfg.get_int("walksPerNode", walks_per_node);
    cfg.get_double("inOutFactor", in_out_factor);
    cfg.get_double("returnFactor", return_factor);
    if (walk_length < 1) walk_length = 1;
    if (walks_per_node < 1) walks_per_node = 1;

    std::vector<int64_t> source_gids;
    bool has_sources = cfg.get_int64_array("sourceNodes", source_gids);
    std::unordered_set<NetworKit::node> source_set;
    if (has_sources)
        for (NetworKit::node n : resolve_node_array(source_gids, pg, "config.sourceNodes"))
            source_set.insert(n);

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

    CypherResultBuilder b(true);
    int64_t walk_id = 0;
    for (const auto &walk : walks) {
        if (walk.empty()) continue;
        if (has_sources && source_set.find(walk.front()) == source_set.end())
            continue;
        for (NetworKit::node u : walk) {
            b.begin_row();
            b.key("walkId"); b.add_int64(walk_id);
            b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
            b.end_row();
        }
        walk_id++;
    }
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- spanningTree_stream (Cypher) */
/* config: objective, relationshipWeightProperty, sourceNode */

PG_FUNCTION_INFO_V1(age_gds_spanningtree_stream_cypher);

Datum age_gds_spanningtree_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"objective", "relationshipWeightProperty", "sourceNode"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::string objective = "minimum";
    cfg.get_string("objective", objective);
    if (objective != "minimum" && objective != "maximum")
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("objective must be 'minimum' or 'maximum', got '%s'",
                               objective.c_str())));

    int64_t src_gid;
    if (cfg.get_int64("sourceNode", src_gid))
        (void)get_node_from_gid(pg, src_gid, "sourceNode");

    NetworKit::Graph &g = const_cast<NetworKit::Graph &>(pg.graph);
    g.indexEdges(true);

    NetworKit::Graph forest;
    try {
        if (objective == "maximum") {
            NetworKit::RandomMaximumSpanningForest sf(pg.graph);
            sf.run();
            forest = sf.getMSF(true);
        } else {
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

    CypherResultBuilder b(true);
    forest.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
        b.begin_row();
        b.key("src"); b.add_int64(pg.id_map.resolve(u));
        b.key("dst"); b.add_int64(pg.id_map.resolve(v));
        b.key("cost"); b.add_double(objective == "maximum" ? w : pg.graph.weight(u, v));
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- allShortestPaths (Cypher) */
/* config: relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_allshortestpaths_stream_cypher);

Datum age_gds_allshortestpaths_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::APSP algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("APSP failed: %s", ex.what())));
    }

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        pg.graph.forNodes([&](NetworKit::node v) {
            double d = algo.getDistance(u, v);
            bool is_inf = (d >= std::numeric_limits<double>::max() / 2.0);
            b.begin_row();
            b.key("sourceNodeId"); b.add_int64(pg.id_map.resolve(u));
            b.key("targetNodeId"); b.add_int64(pg.id_map.resolve(v));
            b.key("distance"); if (is_inf) b.add_null(); else b.add_double(d);
            b.end_row();
        });
    });
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- maxFlow_stats (Cypher) --- */
/* config: sourceNode, targetNode, capacityProperty, sourceNodes,
 * targetNodes, nodeCapacityProperty */

PG_FUNCTION_INFO_V1(age_gds_maxflow_stats_cypher);

Datum age_gds_maxflow_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"sourceNode", "targetNode", "capacityProperty", "sourceNodes",
         "targetNodes", "nodeCapacityProperty"});
    std::string capprop;
    bool has_capprop = cfg.get_string("capacityProperty", capprop);
    apply_weight_override(gh, has_capprop ? capprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::vector<int64_t> source_gids, target_gids;
    bool has_sources = cfg.get_int64_array("sourceNodes", source_gids);
    bool has_targets = cfg.get_int64_array("targetNodes", target_gids);
    bool has_lists = has_sources || has_targets;
    std::string ncprop;
    bool has_node_cap = cfg.get_string("nodeCapacityProperty", ncprop);

    int64_t src_gid = 0, tgt_gid = 0;
    bool has_src = cfg.get_int64("sourceNode", src_gid);
    bool has_tgt = cfg.get_int64("targetNode", tgt_gid);

    auto t0 = std::chrono::steady_clock::now();
    double flow;

    if (!has_lists && !has_node_cap) {
        if (!has_src || !has_tgt)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("config.sourceNode and config.targetNode are required")));
        NetworKit::node nk_src = get_node_from_gid(pg, src_gid, "sourceNode");
        NetworKit::node nk_sink = get_node_from_gid(pg, tgt_gid, "targetNode");
        if (nk_src == nk_sink)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("sourceNode and targetNode must be different")));

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
                            errmsg("maxFlow_stats: sourceNodes/targetNodes/"
                                   "nodeCapacityProperty require a directed graph")));

        std::vector<NetworKit::node> sources, sinks;
        if (has_sources)
            sources = resolve_node_array(source_gids, pg, "config.sourceNodes");
        else if (has_src)
            sources.push_back(get_node_from_gid(pg, src_gid, "sourceNode"));
        if (has_targets)
            sinks = resolve_node_array(target_gids, pg, "config.targetNodes");
        else if (has_tgt)
            sinks.push_back(get_node_from_gid(pg, tgt_gid, "targetNode"));

        if (sources.empty() || sinks.empty())
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("maxFlow_stats: sourceNodes/targetNodes must be non-empty")));

        const NetworKit::Graph *working = &pg.graph;
        NetworKit::Graph split_graph;
        std::vector<NetworKit::node> eff_sources = sources, eff_sinks = sinks;

        if (has_node_cap) {
            std::vector<double> node_cap = read_node_property_as_values(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
                ncprop.c_str(), -1.0);
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

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("flow"); b.add_double(flow);
    b.key("computeMillis"); b.add_int64(compute_ms);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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

/* --------------------------------------- shortestPath_astar_stream (Cypher) */
/* config: sourceNode, targetNode, latitudeProperty, longitudeProperty,
 * relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_shortestpath_astar_stream_cypher);

Datum age_gds_shortestpath_astar_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"sourceNode", "targetNode", "latitudeProperty", "longitudeProperty",
         "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    int64_t src_gid, tgt_gid;
    if (!cfg.get_int64("sourceNode", src_gid) || !cfg.get_int64("targetNode", tgt_gid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNode and config.targetNode are required")));
    NetworKit::node src = get_node_from_gid(pg, src_gid, "sourceNode");
    NetworKit::node tgt = get_node_from_gid(pg, tgt_gid, "targetNode");

    std::string lat_prop, lon_prop;
    if (!cfg.get_string("latitudeProperty", lat_prop) || !cfg.get_string("longitudeProperty", lon_prop))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.latitudeProperty and config.longitudeProperty are required")));

    std::vector<double> lat, lon;
    try {
        lat = read_node_property_as_values(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.node_query.c_str(), lat_prop.c_str(), 0.0);
        lon = read_node_property_as_values(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.node_query.c_str(), lon_prop.c_str(), 0.0);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("shortestPath_astar_stream: %s", ex.what())));
    }

    NetworKit::count n = pg.graph.upperNodeIdBound();
    std::vector<double> heu(n, 0.0);
    for (NetworKit::node u = 0; u < n; u++)
        if (pg.graph.hasNode(u))
            heu[u] = haversine_km(lat[u], lon[u], lat[tgt], lon[tgt]);

    NetworKit::AStar astar(pg.graph, heu, src, tgt, true);
    try { astar.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("AStar failed: %s", ex.what())));
    }

    double d = astar.getDistance();
    bool reachable = d < std::numeric_limits<double>::max() / 2.0;
    std::vector<NetworKit::node> path;
    std::vector<double> costs;
    if (reachable) {
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

    CypherResultBuilder b(true);
    b.begin_row();
    add_path_row_cypher(b, pg, path, costs, d, reachable);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- minCostMaxFlow_stats (Cypher) */
/* config: capacityProperty, costProperty, sourceNodes, targetNodes,
 * nodeCapacityProperty */

PG_FUNCTION_INFO_V1(age_gds_mincostmaxflow_stats_cypher);

Datum age_gds_mincostmaxflow_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"capacityProperty", "costProperty", "sourceNodes", "targetNodes",
         "nodeCapacityProperty"});
    const ProjectedGraph &pg0 = *gh.pg;

    if (!pg0.graph.isDirected())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats requires a directed graph")));

    std::string cost_prop;
    bool has_cost = cfg.get_string("costProperty", cost_prop);
    apply_weight_override(gh, has_cost ? cost_prop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::string cap_prop;
    if (!cfg.get_string("capacityProperty", cap_prop))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.capacityProperty is required")));
    if (pg.age_graph.empty() || pg.edge_query.empty())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats: graph must have age_graph/edge_query "
                               "recorded (re-run graph_project/graph_index)")));

    NetworKit::Graph cap_graph;
    try {
        cap_graph = reweight_from_property(pg.graph, pg.id_map, pg.age_graph.c_str(),
                                           pg.edge_query.c_str(), cap_prop.c_str());
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("minCostMaxFlow_stats: capacityProperty: %s", ex.what())));
    }

    std::vector<int64_t> source_gids, target_gids;
    if (!cfg.get_int64_array("sourceNodes", source_gids) ||
        !cfg.get_int64_array("targetNodes", target_gids))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config.sourceNodes and config.targetNodes are required")));
    std::vector<NetworKit::node> sources = resolve_node_array(source_gids, pg, "config.sourceNodes");
    std::vector<NetworKit::node> sinks   = resolve_node_array(target_gids, pg, "config.targetNodes");
    if (sources.empty() || sinks.empty())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("sourceNodes/targetNodes must be non-empty")));

    struct EdgeCap { NetworKit::node u, v; double cap; };
    std::vector<EdgeCap> edge_caps;

    NetworKit::Graph work(0, true, true);
    std::vector<NetworKit::node> eff_sources, eff_sinks;

    std::string ncprop;
    if (cfg.get_string("nodeCapacityProperty", ncprop)) {
        std::vector<double> node_cap = read_node_property_as_values(
            pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(), ncprop.c_str(), -1.0);

        NetworKit::count n = pg.graph.upperNodeIdBound();
        std::vector<NodeSplit> split(n, NodeSplit{NetworKit::none, NetworKit::none});
        constexpr double unconstrained = std::numeric_limits<double>::max() / 4.0;
        for (NetworKit::node u = 0; u < n; u++) {
            if (!pg.graph.hasNode(u)) continue;
            NetworKit::node uin  = work.addNode();
            NetworKit::node uout = work.addNode();
            split[u] = NodeSplit{uin, uout};
            work.addEdge(uin, uout, 0.0);
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

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("totalCost"); b.add_double(total_cost);
    b.key("computeMillis"); b.add_int64(ms);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- topologicalSort_stream --- */
// arg layout: 0=graph_name, 1=compute_max_distance_from_source,
//             2=age_graph, 3=node_query, 4=edge_query
//
// Backed directly by NetworKit::TopologicalSort(G). Its constructor throws
// if G is not directed, and run() throws "the input graph has cycles" if G
// is not a DAG -- both are input-shape errors, surfaced as
// ERRCODE_INVALID_PARAMETER_VALUE rather than silently producing a partial
// or wrong order.
//
// max_distance (when requested) is a plain linear DP pass over the
// already-computed topological order: dist[v] = max(dist[u]) + 1 over
// predecessors u, computed by walking the order once and pushing each
// node's distance forward to its successors. This is array reduction over
// TopologicalSort's output, not a hand-rolled traversal.

Datum age_gds_topologicalsort_stream(PG_FUNCTION_ARGS)
{
    const char *gname = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool want_dist     = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
    const char *agraph = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *nq      = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *eq      = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    const ProjectedGraph &pg = *gh.pg;

    std::unique_ptr<NetworKit::TopologicalSort> ts;
    try {
        ts = std::make_unique<NetworKit::TopologicalSort>(pg.graph);
        ts->run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("topologicalSort_stream: %s (graph \"%s\" must be "
                               "a directed acyclic graph)",
                               ex.what(), pg.name.c_str())));
    }

    const std::vector<NetworKit::node> &topo = ts->getResult();

    std::vector<int64_t> dist;
    if (want_dist) {
        dist.assign(pg.graph.upperNodeIdBound(), 0);
        for (NetworKit::node u : topo) {
            pg.graph.forNeighborsOf(u, [&](NetworKit::node v) {
                if (dist[u] + 1 > dist[v])
                    dist[v] = dist[u] + 1;
            });
        }
    }

    Oid types[3] = {INT8OID, INT4OID, INT4OID};
    SRFState s = srf_begin(fcinfo, 3, types);

    for (size_t i = 0; i < topo.size(); i++) {
        NetworKit::node u = topo[i];
        Datum values[3]; bool nulls[3] = {false, false, !want_dist};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int32GetDatum((int32_t)i);
        values[2] = want_dist ? Int32GetDatum((int32_t)dist[u]) : (Datum)0;
        srf_emit(s, values, nulls);
    }
    return srf_end(s, fcinfo);
}

/* ------------------------------------ topologicalSort_stream (Cypher) ---- */
/* config: computeMaxDistanceFromSource */

PG_FUNCTION_INFO_V1(age_gds_topologicalsort_stream_cypher);

Datum age_gds_topologicalsort_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"computeMaxDistanceFromSource"});
    const ProjectedGraph &pg = *gh.pg;

    bool want_dist = false;
    cfg.get_bool("computeMaxDistanceFromSource", want_dist);

    std::unique_ptr<NetworKit::TopologicalSort> ts;
    try {
        ts = std::make_unique<NetworKit::TopologicalSort>(pg.graph);
        ts->run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("topologicalSort_stream: %s (graph \"%s\" must be "
                               "a directed acyclic graph)",
                               ex.what(), pg.name.c_str())));
    }
    const std::vector<NetworKit::node> &topo = ts->getResult();

    std::vector<int64_t> dist;
    if (want_dist) {
        dist.assign(pg.graph.upperNodeIdBound(), 0);
        for (NetworKit::node u : topo) {
            pg.graph.forNeighborsOf(u, [&](NetworKit::node v) {
                if (dist[u] + 1 > dist[v]) dist[v] = dist[u] + 1;
            });
        }
    }

    CypherResultBuilder b(true);
    for (size_t i = 0; i < topo.size(); i++) {
        NetworKit::node u = topo[i];
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("sortIndex"); b.add_int64((int64_t)i);
        b.key("maxDistance"); if (want_dist) b.add_int64(dist[u]); else b.add_null();
        b.end_row();
    }
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- longestPath_stream ------- */
// arg layout: 0=graph_name, 1=relationship_weight_property,
//             2=age_graph, 3=node_query, 4=edge_query
//
// Weighted longest-path-in-a-DAG: same TopologicalSort + DP pattern as
// topologicalSort_stream's max_distance column, but weighted:
// dist[v] = max over predecessors u of dist[u] + weight(u,v). Nodes with no
// predecessors get dist 0 (they anchor the longest paths passing through
// them). Errors out if the graph is not a DAG, same as above.

Datum age_gds_longestpath_stream(PG_FUNCTION_ARGS)
{
    const char *gname = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *wprop  = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *agraph = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *nq      = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *eq      = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    std::unique_ptr<NetworKit::TopologicalSort> ts;
    try {
        ts = std::make_unique<NetworKit::TopologicalSort>(pg.graph);
        ts->run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("longestPath_stream: %s (graph \"%s\" must be "
                               "a directed acyclic graph)",
                               ex.what(), pg.name.c_str())));
    }

    const std::vector<NetworKit::node> &topo = ts->getResult();

    std::vector<double> dist(pg.graph.upperNodeIdBound(), 0.0);
    for (NetworKit::node u : topo) {
        pg.graph.forNeighborsOf(u, [&](NetworKit::node v, NetworKit::edgeweight w) {
            double cand = dist[u] + w;
            if (cand > dist[v])
                dist[v] = cand;
        });
    }

    Oid types[2] = {INT8OID, FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    for (NetworKit::node u : topo) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Float8GetDatum(dist[u]);
        srf_emit(s, values, nulls);
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- longestPath_stream (Cypher) */
/* config: relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_longestpath_stream_cypher);

Datum age_gds_longestpath_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::unique_ptr<NetworKit::TopologicalSort> ts;
    try {
        ts = std::make_unique<NetworKit::TopologicalSort>(pg.graph);
        ts->run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("longestPath_stream: %s (graph \"%s\" must be "
                               "a directed acyclic graph)",
                               ex.what(), pg.name.c_str())));
    }
    const std::vector<NetworKit::node> &topo = ts->getResult();

    std::vector<double> dist(pg.graph.upperNodeIdBound(), 0.0);
    for (NetworKit::node u : topo) {
        pg.graph.forNeighborsOf(u, [&](NetworKit::node v, NetworKit::edgeweight w) {
            double cand = dist[u] + w;
            if (cand > dist[v]) dist[v] = cand;
        });
    }

    CypherResultBuilder b(true);
    for (NetworKit::node u : topo) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("distance"); b.add_double(dist[u]);
        b.end_row();
    }
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- bridges_stream ----------- */
// No dedicated NetworKit `Bridges` class. Derived from
// NetworKit::BiconnectedComponents(G) (already used for
// articulationPoints_stream in centrality.cpp): an edge is a bridge iff it
// is the sole edge of its biconnected component, i.e. the component's node
// set has exactly 2 members. One-pass filter over BiconnectedComponents's
// output -- no new traversal algorithm.
//
// Caveat: "component has exactly 2 nodes" and "component has exactly 1
// edge" only coincide for simple graphs. age_loader.cpp's addEdge() does
// not deduplicate parallel AGE relationships between the same node pair
// (NetworKit::Graph allows multi-edges), so two parallel edges between u
// and v would form a 2-node biconnected component that is *not* actually
// a bridge (removing either parallel edge leaves the graph connected).
// This is a documented, deliberate approximation (see docs/api.md's
// bridges_stream entry), not a silent gap: it is exact for the common
// case (no parallel relationships between the same node pair) and only
// diverges on projections with duplicate relationships.

/* Shared core: both the SQL SRF and the Cypher-callable agtype overload
 * call this directly (no SPI round-trip between the two wrappers). */
static std::vector<std::pair<int64_t, int64_t>> bridges_core(const ProjectedGraph &pg)
{
    NetworKit::BiconnectedComponents bcc(pg.graph);
    try { bcc.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("BiconnectedComponents failed: %s", ex.what())));
    }

    std::vector<std::pair<int64_t, int64_t>> result;
    for (const auto &comp : bcc.getComponents()) {
        if (comp.size() != 2)
            continue;
        result.emplace_back(pg.id_map.resolve(comp[0]), pg.id_map.resolve(comp[1]));
    }
    return result;
}

Datum age_gds_bridges_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo, 1);
    const ProjectedGraph &pg = *gh.pg;
    auto bridges = bridges_core(pg);

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    for (const auto &e : bridges) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(e.first);
        values[1] = Int64GetDatum(e.second);
        srf_emit(s, values, nulls);
    }
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- bridges_stream (Cypher) -- */
/* age_gds.bridges_stream(graphName agtype, config agtype DEFAULT NULL)
 * RETURNS agtype -- JSON array of {"src", "dst"} maps.
 * config: {ageGraph, nodeQuery, edgeQuery} for anonymous-mode projection
 * (graphName NULL); no other keys. */

PG_FUNCTION_INFO_V1(age_gds_bridges_stream_cypher);

Datum age_gds_bridges_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;
    auto bridges = bridges_core(pg);

    CypherResultBuilder b(/* as_array */ true);
    for (const auto &e : bridges) {
        b.begin_row();
        b.key("src"); b.add_int64(e.first);
        b.key("dst"); b.add_int64(e.second);
        b.end_row();
    }
    PG_RETURN_DATUM(b.finish());
}

} /* extern "C" */
