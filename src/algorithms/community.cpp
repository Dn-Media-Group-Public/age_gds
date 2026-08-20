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
#include <networkit/community/Conductance.hpp>
#include <networkit/components/ConnectedComponents.hpp>
#include <networkit/components/StronglyConnectedComponents.hpp>
#include <networkit/centrality/CoreDecomposition.hpp>
#include <networkit/centrality/LocalClusteringCoefficient.hpp>
#include <networkit/clique/MaximalCliques.hpp>
#include <networkit/structures/Partition.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
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
#include "util/weight_override.h"
#include "util/partition_post.h"
#include "util/cypher_agtype.h"

/* helper: emit community partition rows, skipping nodes the partition left
 * unassigned (NetworKit::none) -- e.g. after filter_min_community_size(). */
static void emit_partition(SRFState &s,
                            const ProjectedGraph &pg,
                            const NetworKit::Partition &part)
{
    pg.graph.forNodes([&](NetworKit::node u) {
        if (!part.contains(u))
            return;
        int64_t comm = (int64_t)part.subsetOf(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comm);
        srf_emit(s, values, nulls);
    });
}

/* Cypher-callable equivalent of emit_partition(): array of
 * {nodeId, <key_name>} maps. */
static void add_partition_rows_cypher(CypherResultBuilder &b, const ProjectedGraph &pg,
                                      const NetworKit::Partition &part, const char *key_name)
{
    pg.graph.forNodes([&](NetworKit::node u) {
        if (!part.contains(u))
            return;
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key(key_name); b.add_int64((int64_t)part.subsetOf(u));
        b.end_row();
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

// Raises a consistent ereport(ERROR) when a node-property read
// (seed_property / community_property / triangle_count_property) is
// requested against a graph whose catalog row predates age_graph/node_query
// being recorded (pre-existing rows, or live-index graphs created via
// graph_index(), which does not record node_query -- see
// docs/architecture.md "Per-call weight override" for the analogous
// edge_query limitation).
static void require_node_query(const ProjectedGraph &pg, const char *what)
{
    if (pg.age_graph.empty() || pg.node_query.empty())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s requires graph \"%s\" to have its age_graph/node_query "
                        "recorded in the catalog (re-run graph_project to populate "
                        "them; graph_index()-created live indices do not currently "
                        "record node_query)",
                        what, pg.name.c_str())));
}

/* ============================================================ C functions */

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
PG_FUNCTION_INFO_V1(age_gds_conductance_stats);
PG_FUNCTION_INFO_V1(age_gds_cliquecounting_stats);

/* ---------------------------------------------- louvain_stream ----------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=relationship_weight_property, 5=gamma, 6=max_iterations,
//             7=recurse, 8=consecutive_ids, 9=min_community_size

Datum age_gds_louvain_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *wprop  = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    double gamma       = PG_ARGISNULL(5) ? 1.0  : PG_GETARG_FLOAT8(5);
    int    max_iter    = PG_ARGISNULL(6) ? 10   : PG_GETARG_INT32(6);
    bool   recurse     = PG_ARGISNULL(7) ? true : PG_GETARG_BOOL(7);
    bool   want_consecutive = PG_ARGISNULL(8) ? false : PG_GETARG_BOOL(8);
    int    min_comm_size    = PG_ARGISNULL(9) ? 0     : PG_GETARG_INT32(9);

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    // PLM's own default maxIter is 32; louvain_stream's default of 10
    // documented in docs/api.md as a deliberate deviation.
    NetworKit::PLM algo(pg.graph, /*refine*/ false, gamma, "balanced",
                        (NetworKit::count)max_iter, /*turbo*/ true, recurse);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Louvain (PLM) failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);
    if (min_comm_size > 1)
        filter_min_community_size(part, (size_t)min_comm_size);

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    emit_partition(s, pg, part);
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- louvain_stream (Cypher) -- */
/* config: relationshipWeightProperty, gamma, maxIterations, recurse,
 * consecutiveIds, minCommunitySize */

PG_FUNCTION_INFO_V1(age_gds_louvain_stream_cypher);

Datum age_gds_louvain_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"relationshipWeightProperty", "gamma", "maxIterations", "recurse",
         "consecutiveIds", "minCommunitySize"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double gamma = 1.0;
    int max_iter = 10, min_comm_size = 0;
    bool recurse = true, want_consecutive = false;
    cfg.get_double("gamma", gamma);
    cfg.get_int("maxIterations", max_iter);
    cfg.get_bool("recurse", recurse);
    cfg.get_bool("consecutiveIds", want_consecutive);
    cfg.get_int("minCommunitySize", min_comm_size);

    NetworKit::PLM algo(pg.graph, false, gamma, "balanced",
                        (NetworKit::count)max_iter, true, recurse);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Louvain (PLM) failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);
    if (min_comm_size > 1)
        filter_min_community_size(part, (size_t)min_comm_size);

    CypherResultBuilder b(true);
    add_partition_rows_cypher(b, pg, part, "communityId");
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- leiden_stream ------------ */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=relationship_weight_property, 5=gamma, 6=iterations

Datum age_gds_leiden_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *wprop  = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    double gamma       = PG_ARGISNULL(5) ? 1.0 : PG_GETARG_FLOAT8(5);
    int    iterations  = PG_ARGISNULL(6) ? 10  : PG_GETARG_INT32(6);

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    // ParallelLeiden's own default `iterations` is 3; leiden_stream's
    // default of 10 documented in docs/api.md as a deliberate deviation.
    // Note: NetworKit's `iterations` (repeated full Leiden runs) is not the
    // same knob as Neo4j's `maxLevels` (coarsening depth) -- see
    // docs/architecture.md.
    NetworKit::ParallelLeiden algo(pg.graph, iterations, /*randomize*/ true, gamma);
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

/* ---------------------------------------------- leiden_stream (Cypher) --- */
/* config: relationshipWeightProperty, gamma, iterations */

PG_FUNCTION_INFO_V1(age_gds_leiden_stream_cypher);

Datum age_gds_leiden_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"relationshipWeightProperty", "gamma", "iterations"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double gamma = 1.0;
    int iterations = 10;
    cfg.get_double("gamma", gamma);
    cfg.get_int("iterations", iterations);

    NetworKit::ParallelLeiden algo(pg.graph, iterations, true, gamma);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ParallelLeiden failed: %s", ex.what())));
    }

    CypherResultBuilder b(true);
    add_partition_rows_cypher(b, pg, algo.getPartition(), "communityId");
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- labelPropagation_stream -- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=relationship_weight_property, 5=max_iterations,
//             6=seed_property, 7=consecutive_ids
//
// seed_property is NOT implemented -- see the ereport(ERROR) below for the
// full explanation. This is a NetworKit API gap discovered during
// implementation of this plan, beyond the ones the plan's revision already
// excluded (node_weight_property).

Datum age_gds_labelpropagation_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *wprop  = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    bool has_max_iter  = !PG_ARGISNULL(5);
    int  max_iter      = has_max_iter ? PG_GETARG_INT32(5) : 0;
    const char *seed_prop = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    bool want_consecutive = PG_ARGISNULL(7) ? false : PG_GETARG_BOOL(7);

    if (seed_prop)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("labelPropagation_stream: seed_property is not supported"),
                 errdetail("NetworKit::PLP's baseClustering constructor "
                           "(PLP(G, baseClustering, theta)) never initializes its "
                           "private `maxIterations` member -- the vendored header "
                           "(third_party/networkit/include/networkit/community/PLP.hpp) "
                           "declares it with no default member initializer, and that "
                           "constructor's initializer list only sets updateThreshold. "
                           "PLP::run()'s loop condition `nIterations < maxIterations` "
                           "therefore reads an uninitialized value on the seeded path, "
                           "and there is no public setter to fix it after construction. "
                           "This is a NetworKit API gap, not something age_gds can "
                           "safely delegate to (undefined behavior) or paper over with "
                           "hand-written label propagation (forbidden by project rules). "
                           "Passing seed_property is refused rather than risking UB.")));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    // has_max_iter == false => NetworKit::none for both theta and
    // maxIterations, matching the previous (pre-plan) unbounded behavior.
    NetworKit::PLP algo(pg.graph, /*theta*/ NetworKit::none,
                        has_max_iter ? (NetworKit::count)max_iter : NetworKit::none);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LabelPropagation (PLP) failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);
    emit_partition(s, pg, part);
    return srf_end(s, fcinfo);
}

/* ------------------------------------- labelPropagation_stream (Cypher) -- */
/* config: relationshipWeightProperty, maxIterations, seedProperty,
 * consecutiveIds. seedProperty is refused, same NetworKit gap as the SQL
 * entry point above. */

PG_FUNCTION_INFO_V1(age_gds_labelpropagation_stream_cypher);

Datum age_gds_labelpropagation_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"relationshipWeightProperty", "maxIterations", "seedProperty", "consecutiveIds"});

    if (cfg.has("seedProperty"))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("labelPropagation_stream: seedProperty is not supported"),
                 errdetail("Same NetworKit::PLP API gap as the SQL entry point's "
                           "seed_property (see src/algorithms/community.cpp).")));

    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    int max_iter = 0;
    bool has_max_iter = cfg.get_int("maxIterations", max_iter);
    bool want_consecutive = false;
    cfg.get_bool("consecutiveIds", want_consecutive);

    NetworKit::PLP algo(pg.graph, NetworKit::none,
                        has_max_iter ? (NetworKit::count)max_iter : NetworKit::none);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LabelPropagation (PLP) failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    CypherResultBuilder b(true);
    add_partition_rows_cypher(b, pg, part, "communityId");
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- wcc_stream --------------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=relationship_weight_property, 5=threshold, 6=consecutive_ids

Datum age_gds_wcc_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *wprop  = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    bool   has_threshold = !PG_ARGISNULL(5);
    double threshold     = has_threshold ? PG_GETARG_FLOAT8(5) : 0.0;
    bool   want_consecutive = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    // threshold: build a filtered *copy* excluding edges below the
    // threshold weight (same "private copy, never mutate the cache"
    // pattern as apply_weight_override's reweighted copy). On an
    // unweighted projection every edge has NetworKit::defaultEdgeWeight
    // (1.0), so a threshold > 1.0 drops every edge -- document this in
    // docs/api.md rather than special-casing it here.
    NetworKit::Graph filtered;
    const NetworKit::Graph *graph_ptr = &pg.graph;
    if (has_threshold) {
        filtered = NetworKit::Graph(pg.graph.upperNodeIdBound(), /*weighted*/ true,
                                    pg.graph.isDirected());
        pg.graph.forEdges([&](NetworKit::node u, NetworKit::node v,
                              NetworKit::edgeweight w) {
            if (w >= threshold)
                filtered.addEdge(u, v, w);
        });
        graph_ptr = &filtered;
    }

    NetworKit::ConnectedComponents algo(*graph_ptr);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ConnectedComponents failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        if (!part.contains(u))
            return;
        int64_t comp = (int64_t)part.subsetOf(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comp);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- wcc_stream (Cypher) ------ */
/* config: relationshipWeightProperty, threshold, consecutiveIds */

PG_FUNCTION_INFO_V1(age_gds_wcc_stream_cypher);

Datum age_gds_wcc_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"relationshipWeightProperty", "threshold", "consecutiveIds"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    double threshold = 0.0;
    bool has_threshold = cfg.get_double("threshold", threshold);
    bool want_consecutive = false;
    cfg.get_bool("consecutiveIds", want_consecutive);

    NetworKit::Graph filtered;
    const NetworKit::Graph *graph_ptr = &pg.graph;
    if (has_threshold) {
        filtered = NetworKit::Graph(pg.graph.upperNodeIdBound(), true, pg.graph.isDirected());
        pg.graph.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
            if (w >= threshold) filtered.addEdge(u, v, w);
        });
        graph_ptr = &filtered;
    }

    NetworKit::ConnectedComponents algo(*graph_ptr);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("ConnectedComponents failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    CypherResultBuilder b(true);
    add_partition_rows_cypher(b, pg, part, "componentId");
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- scc_stream --------------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=consecutive_ids

Datum age_gds_scc_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;
    bool want_consecutive = PG_ARGISNULL(4) ? false : PG_GETARG_BOOL(4);

    NetworKit::StronglyConnectedComponents algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("StronglyConnectedComponents failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    Oid types[2] = {INT8OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        if (!part.contains(u))
            return;
        int64_t comp = (int64_t)part.subsetOf(u);
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(comp);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- scc_stream (Cypher) ------ */
/* config: consecutiveIds */

PG_FUNCTION_INFO_V1(age_gds_scc_stream_cypher);

Datum age_gds_scc_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"consecutiveIds"});
    const ProjectedGraph &pg = *gh.pg;
    bool want_consecutive = false;
    cfg.get_bool("consecutiveIds", want_consecutive);

    NetworKit::StronglyConnectedComponents algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("StronglyConnectedComponents failed: %s", ex.what())));
    }

    NetworKit::Partition part = algo.getPartition();
    if (want_consecutive)
        consecutive_ids(part);

    CypherResultBuilder b(true);
    add_partition_rows_cypher(b, pg, part, "componentId");
    PG_RETURN_DATUM(b.finish());
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

/* ---------------------------------------------- kCore_stream (Cypher) ---- */

PG_FUNCTION_INFO_V1(age_gds_kcore_stream_cypher);

Datum age_gds_kcore_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::CoreDecomposition algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("CoreDecomposition failed: %s", ex.what())));
    }
    const auto &scores = algo.scores();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("coreNumber"); b.add_int64((int64_t)scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- triangleCount_stream ----- */
// Uses LocalClusteringCoefficient: triangles_u = LCC(u) * deg*(deg-1)/2
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=max_degree

Datum age_gds_trianglecount_stream(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;
    bool    has_max_degree = !PG_ARGISNULL(4);
    int64_t max_degree     = has_max_degree ? PG_GETARG_INT64(4) : INT64_MAX;

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
        double  deg = (double)pg.graph.degree(u);
        int64_t triangles;
        if ((int64_t)pg.graph.degree(u) > max_degree) {
            // max_degree pre-filter: skip nodes above the cap entirely,
            // signalled as -1 rather than a computed (and potentially
            // expensive-to-have-computed) count.
            triangles = -1;
        } else if (deg >= 2.0) {
            triangles = (int64_t)std::round(lcc[u] * deg * (deg - 1.0) / 2.0);
        } else {
            triangles = 0;
        }
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = Int64GetDatum(triangles);
        srf_emit(s, values, nulls);
    });
    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- triangleCount (Cypher) --- */
/* config: maxDegree */

PG_FUNCTION_INFO_V1(age_gds_trianglecount_stream_cypher);

Datum age_gds_trianglecount_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"maxDegree"});
    const ProjectedGraph &pg = *gh.pg;
    int64_t max_degree = INT64_MAX;
    cfg.get_int64("maxDegree", max_degree);

    NetworKit::LocalClusteringCoefficient algo(pg.graph);
    try { algo.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("LocalClusteringCoefficient failed: %s", ex.what())));
    }
    const auto &lcc = algo.scores();

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        double deg = (double)pg.graph.degree(u);
        int64_t triangles;
        if ((int64_t)pg.graph.degree(u) > max_degree)
            triangles = -1;
        else if (deg >= 2.0)
            triangles = (int64_t)std::round(lcc[u] * deg * (deg - 1.0) / 2.0);
        else
            triangles = 0;
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("triangleCount"); b.add_int64(triangles);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- localClusteringCoefficient */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=triangle_count_property

Datum age_gds_localclusteringcoefficient_stream(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *tc_prop = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    const ProjectedGraph &pg = *gh.pg;

    std::vector<double> scores;

    if (tc_prop) {
        // Bypass NetworKit::LocalClusteringCoefficient entirely: use the
        // precomputed triangle counts read from `tc_prop` and the standard
        // LCC formula 2*triangles(u) / (deg(u)*(deg(u)-1)) directly.
        require_node_query(pg, "localClusteringCoefficient_stream's triangle_count_property");

        std::vector<double> triangles;
        try {
            triangles = read_node_property_as_values(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
                tc_prop, 0.0);
        } catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("triangle_count_property read failed for graph \"%s\": %s",
                                   pg.name.c_str(), ex.what())));
        }

        scores.assign(pg.graph.upperNodeIdBound(), 0.0);
        pg.graph.forNodes([&](NetworKit::node u) {
            double deg = (double)pg.graph.degree(u);
            scores[u] = (deg >= 2.0)
                ? (2.0 * triangles[u]) / (deg * (deg - 1.0))
                : 0.0;
        });
    } else {
        NetworKit::LocalClusteringCoefficient algo(pg.graph);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("LocalClusteringCoefficient failed: %s", ex.what())));
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

/* --------------------------------- localClusteringCoefficient (Cypher) --- */
/* config: triangleCountProperty */

PG_FUNCTION_INFO_V1(age_gds_localclusteringcoefficient_stream_cypher);

Datum age_gds_localclusteringcoefficient_stream_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {"triangleCountProperty"});
    const ProjectedGraph &pg = *gh.pg;

    std::string tc_prop;
    bool has_tc_prop = cfg.get_string("triangleCountProperty", tc_prop);

    std::vector<double> scores;
    if (has_tc_prop) {
        require_node_query(pg, "localClusteringCoefficient_stream's triangleCountProperty");
        std::vector<double> triangles;
        try {
            triangles = read_node_property_as_values(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
                tc_prop.c_str(), 0.0);
        } catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("triangleCountProperty read failed for graph \"%s\": %s",
                                   pg.name.c_str(), ex.what())));
        }
        scores.assign(pg.graph.upperNodeIdBound(), 0.0);
        pg.graph.forNodes([&](NetworKit::node u) {
            double deg = (double)pg.graph.degree(u);
            scores[u] = (deg >= 2.0) ? (2.0 * triangles[u]) / (deg * (deg - 1.0)) : 0.0;
        });
    } else {
        NetworKit::LocalClusteringCoefficient algo(pg.graph);
        try { algo.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("LocalClusteringCoefficient failed: %s", ex.what())));
        }
        scores = algo.scores();
    }

    CypherResultBuilder b(true);
    pg.graph.forNodes([&](NetworKit::node u) {
        b.begin_row();
        b.key("nodeId"); b.add_int64(pg.id_map.resolve(u));
        b.key("coefficient"); b.add_double(scores[u]);
        b.end_row();
    });
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- modularity_stats --------- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//             4=community_property, 5=relationship_weight_property

Datum age_gds_modularity_stats(PG_FUNCTION_ARGS)
{
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *nq     = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *eq     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *comm_prop = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    const char *wprop     = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    NetworKit::Partition part;

    if (comm_prop) {
        // community_property: evaluate modularity of a caller-supplied
        // partition instead of always running an internal PLM first.
        require_node_query(pg, "modularity_stats's community_property");
        try {
            part = read_node_property_as_partition(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
                comm_prop);
        } catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("community_property read failed for graph \"%s\": %s",
                                   pg.name.c_str(), ex.what())));
        }
    } else {
        NetworKit::PLM plm(pg.graph);
        try { plm.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("PLM (modularity) failed: %s", ex.what())));
        }
        part = plm.getPartition();
    }

    NetworKit::Modularity mod;
    double mod_val = mod.getQuality(part, pg.graph);

    Oid types[1] = {FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 1, types);

    Datum values[1]; bool nulls[1] = {false};
    values[0] = Float8GetDatum(mod_val);
    srf_emit(s, values, nulls);

    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- modularity_stats (Cypher) */
/* config: communityProperty, relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_modularity_stats_cypher);

Datum age_gds_modularity_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"communityProperty", "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::string comm_prop;
    bool has_comm_prop = cfg.get_string("communityProperty", comm_prop);

    NetworKit::Partition part;
    if (has_comm_prop) {
        require_node_query(pg, "modularity_stats's communityProperty");
        try {
            part = read_node_property_as_partition(
                pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
                comm_prop.c_str());
        } catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("communityProperty read failed for graph \"%s\": %s",
                                   pg.name.c_str(), ex.what())));
        }
    } else {
        NetworKit::PLM plm(pg.graph);
        try { plm.run(); }
        catch (const std::exception &ex) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                            errmsg("PLM (modularity) failed: %s", ex.what())));
        }
        part = plm.getPartition();
    }

    NetworKit::Modularity mod;
    double mod_val = mod.getQuality(part, pg.graph);

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("modularity"); b.add_double(mod_val);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- conductance_stats -------- */
// arg layout: 0=graph_name, 1=community_property, 2=relationship_weight_property,
//             3=age_graph, 4=node_query, 5=edge_query
//
// NetworKit::Conductance::getQuality() (community/Conductance.hpp) requires
// a literal 2-partition: cluster ids must be exactly 0 or 1 for every node
// (it indexes fixed-size arrays vol[2]/vol[0..1] directly by subset id, with
// no bounds check -- an id outside {0,1} is undefined behavior, not a
// caught error). That contract is incompatible with
// read_node_property_as_partition()'s ids (plan 02's helper), which
// deliberately assigns *internal* dense subset ids offset above
// graph.upperNodeIdBound() rather than passing raw property values through
// as literal subset ids -- reusing it here would silently violate
// Conductance's precondition. Instead this reuses
// read_node_property_as_values() (also from partition_post.h) to read the
// raw numeric property, validates every node's value is exactly 0.0 or 1.0
// (erroring otherwise -- no silent coercion), and builds the
// NetworKit::Partition directly from those literal 0/1 ids.
Datum age_gds_conductance_stats(PG_FUNCTION_ARGS)
{
    const char *gname     = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *comm_prop  = PG_ARGISNULL(1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *wprop      = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    const char *agraph     = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    const char *nq         = PG_ARGISNULL(4) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(4));
    const char *eq         = PG_ARGISNULL(5) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(5));

    if (!comm_prop)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("conductance_stats requires community_property")));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    require_node_query(pg, "conductance_stats's community_property");

    std::vector<double> values;
    const double MISSING = std::numeric_limits<double>::quiet_NaN();
    try {
        values = read_node_property_as_values(
            pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
            comm_prop, MISSING);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("community_property read failed for graph \"%s\": %s",
                               pg.name.c_str(), ex.what())));
    }

    std::vector<NetworKit::index> ids(pg.graph.upperNodeIdBound(), NetworKit::none);
    bool saw0 = false, saw1 = false;
    pg.graph.forNodes([&](NetworKit::node u) {
        double v = values[u];
        if (v != 0.0 && v != 1.0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("conductance_stats: community_property \"%s\" must be "
                            "exactly 0 or 1 for every node (Conductance is only "
                            "defined for 2-partitions); node %" PRId64 " has %s",
                            comm_prop, (int64_t)pg.id_map.resolve(u),
                            std::isnan(v) ? "no value" : "a different value")));
        ids[u] = (NetworKit::index)v;
        if (v == 0.0) saw0 = true; else saw1 = true;
    });
    if (!saw0 || !saw1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("conductance_stats: community_property \"%s\" must assign "
                        "at least one node to each of community 0 and community 1",
                        comm_prop)));

    NetworKit::Partition part(ids);
    // Partition(vector) sets omega = max_elem(data), which is 0 (not 1)
    // whenever any slot is still NetworKit::none -- e.g. graph.upperNodeIdBound()
    // > graph.numberOfNodes() (removed/tombstoned node ids on an incremental
    // or live projection). Conductance::getQuality() then indexes fixed-size
    // 2-element arrays by upperBound() (a heap OOB write for anything other
    // than literal 0/1), so omega must be pinned explicitly regardless of
    // gaps in the node-id space.
    part.setUpperBound(2);
    NetworKit::Conductance cond;
    double cond_val = cond.getQuality(part, pg.graph);

    Oid types[1] = {FLOAT8OID};
    SRFState s = srf_begin(fcinfo, 1, types);
    Datum out_values[1]; bool out_nulls[1] = {false};
    out_values[0] = Float8GetDatum(cond_val);
    srf_emit(s, out_values, out_nulls);

    return srf_end(s, fcinfo);
}

/* ---------------------------------------------- conductance_stats (Cypher) */
/* config: communityProperty (required), relationshipWeightProperty */

PG_FUNCTION_INFO_V1(age_gds_conductance_stats_cypher);

Datum age_gds_conductance_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg,
        {"communityProperty", "relationshipWeightProperty"});
    std::string wprop;
    bool has_wprop = cfg.get_string("relationshipWeightProperty", wprop);
    apply_weight_override(gh, has_wprop ? wprop.c_str() : nullptr);
    const ProjectedGraph &pg = *gh.pg;

    std::string comm_prop;
    if (!cfg.get_string("communityProperty", comm_prop))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("conductance_stats requires config.communityProperty")));

    require_node_query(pg, "conductance_stats's communityProperty");

    std::vector<double> values;
    const double MISSING = std::numeric_limits<double>::quiet_NaN();
    try {
        values = read_node_property_as_values(
            pg.graph, pg.id_map, pg.age_graph.c_str(), pg.node_query.c_str(),
            comm_prop.c_str(), MISSING);
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("communityProperty read failed for graph \"%s\": %s",
                               pg.name.c_str(), ex.what())));
    }

    std::vector<NetworKit::index> ids(pg.graph.upperNodeIdBound(), NetworKit::none);
    bool saw0 = false, saw1 = false;
    pg.graph.forNodes([&](NetworKit::node u) {
        double v = values[u];
        if (v != 0.0 && v != 1.0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("conductance_stats: communityProperty \"%s\" must be "
                            "exactly 0 or 1 for every node (Conductance is only "
                            "defined for 2-partitions); node %" PRId64 " has %s",
                            comm_prop.c_str(), (int64_t)pg.id_map.resolve(u),
                            std::isnan(v) ? "no value" : "a different value")));
        ids[u] = (NetworKit::index)v;
        if (v == 0.0) saw0 = true; else saw1 = true;
    });
    if (!saw0 || !saw1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("conductance_stats: communityProperty \"%s\" must assign "
                        "at least one node to each of community 0 and community 1",
                        comm_prop.c_str())));

    NetworKit::Partition part(ids);
    part.setUpperBound(2);
    NetworKit::Conductance cond;
    double cond_val = cond.getQuality(part, pg.graph);

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("conductance"); b.add_double(cond_val);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
}

/* ---------------------------------------------- cliqueCounting_stats ----- */
// arg layout: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query
//
// Backed by NetworKit::MaximalCliques(G, callback) (clique/MaximalCliques.hpp):
// counts are accumulated, bucketed by clique size, as each maximal clique is
// emitted by the callback -- no materialization of the full clique list.
// Note: this counts *maximal* cliques (NetworKit has no "list every clique
// of every size, maximal or not" primitive), bucketed by size; the total
// clique count across all sizes is SUM(count) over the result rows.
Datum age_gds_cliquecounting_stats(PG_FUNCTION_ARGS)
{
    GraphHandle gh = resolve_graph_args(fcinfo);
    const ProjectedGraph &pg = *gh.pg;

    // Cliques are an undirected-graph concept; unlike BiconnectedComponents
    // (used by bridges_stream/articulationPoints_stream), MaximalCliques'
    // constructor does not itself reject directed input -- it would just
    // silently treat edge direction as irrelevant, producing a plausible
    // but wrong clique count. Reject explicitly rather than paper over it.
    if (pg.graph.isDirected())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cliqueCounting_stats requires an undirected graph "
                               "(cliques are not defined for directed edges); "
                               "graph \"%s\" is directed", pg.name.c_str())));

    std::map<size_t, int64_t> counts_by_size;
    std::function<void(const std::vector<NetworKit::node> &)> cb =
        [&](const std::vector<NetworKit::node> &clique) {
            counts_by_size[clique.size()]++;
        };

    try {
        NetworKit::MaximalCliques mc(pg.graph, cb);
        mc.run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("MaximalCliques failed: %s", ex.what())));
    }

    Oid types[2] = {INT4OID, INT8OID};
    SRFState s = srf_begin(fcinfo, 2, types);

    for (const auto &[size, count] : counts_by_size) {
        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int32GetDatum((int32_t)size);
        values[1] = Int64GetDatum(count);
        srf_emit(s, values, nulls);
    }
    return srf_end(s, fcinfo);
}

/* --------------------------------------- cliqueCounting_stats (Cypher) --- */
/* No per-algorithm config keys. Despite the "_stats" name, the SQL entry
 * point already returns multiple rows (bucketed by clique size); the
 * Cypher overload follows that actual shape (array of maps), not the
 * name-suffix convention. */

PG_FUNCTION_INFO_V1(age_gds_cliquecounting_stats_cypher);

Datum age_gds_cliquecounting_stats_cypher(PG_FUNCTION_ARGS)
{
    CypherConfig cfg;
    GraphHandle gh = resolve_cypher_graph(fcinfo, 0, 1, cfg, {});
    const ProjectedGraph &pg = *gh.pg;

    if (pg.graph.isDirected())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("cliqueCounting_stats requires an undirected graph "
                               "(cliques are not defined for directed edges); "
                               "graph \"%s\" is directed", pg.name.c_str())));

    std::map<size_t, int64_t> counts_by_size;
    std::function<void(const std::vector<NetworKit::node> &)> cb =
        [&](const std::vector<NetworKit::node> &clique) {
            counts_by_size[clique.size()]++;
        };

    try {
        NetworKit::MaximalCliques mc(pg.graph, cb);
        mc.run();
    } catch (const std::exception &ex) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("MaximalCliques failed: %s", ex.what())));
    }

    CypherResultBuilder b(true);
    for (const auto &[size, count] : counts_by_size) {
        b.begin_row();
        b.key("cliqueSize"); b.add_int64((int64_t)size);
        b.key("count"); b.add_int64(count);
        b.end_row();
    }
    PG_RETURN_DATUM(b.finish());
}

} /* extern "C" */
