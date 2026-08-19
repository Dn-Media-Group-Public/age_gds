// src/algorithms/embedding.cpp
//
// Node embeddings: node2vec_stream.
//
// Delegates to the same two internal NetworKit primitives
// NetworKit::Node2Vec::run() itself calls (see
// third_party/networkit/networkit/cpp/embedding/Node2Vec.cpp):
//   NetworKit::Embedding::BiasedRandomWalk   -- P/Q-biased random walks
//   NetworKit::Embedding::learnEmbeddings    -- skip-gram trainer
//
// We call these directly instead of going through the public
// NetworKit::Node2Vec wrapper because Node2Vec::run() hardcodes
// winSize=10, iterations=1 when it calls learnEmbeddings() and does not
// expose them as constructor parameters (Node2Vec is also `final`, so it
// cannot be subclassed to override this). Both are real, vendored
// NetworKit functions -- compiled into the same `networkit` static
// library age_gds already links -- just not re-exported under the public
// include/networkit/ Node2Vec.hpp API. This mirrors the precedent set by
// randomWalk_stream in pathfinding.cpp (see docs/implemented-plans/03-
// pathfinding-parity.md and the CMakeLists.txt comment on the extra
// include directory this requires).
//
// relationship_weight_property is supported: BiasedRandomWalk::preprocessNode()
// reads edge weights directly from graph->forNeighborsOf() when computing
// transition probabilities (confirmed by reading
// networkit/cpp/embedding/BiasedRandomWalk.cpp) -- so weight overrides via
// util/weight_override.h flow through correctly.
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <AliasSampler.hpp>
#include <BiasedRandomWalk.hpp>
#include <LearnEmbedding.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <stdexcept>
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

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_node2vec_stream);

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

/* ------------------------------------------ node2vec_stream -------------- */
// Args: 0=graph_name, 1=embedding_dim, 2=walk_length, 3=walks_per_node,
//       4=p, 5=q, 6=age_graph, 7=node_query, 8=edge_query,
//       9=relationship_weight_property, 10=window_size, 11=iterations

Datum age_gds_node2vec_stream(PG_FUNCTION_ARGS)
{
    int    dim   = PG_ARGISNULL(1) ? 128  : PG_GETARG_INT32(1);
    int    wlen  = PG_ARGISNULL(2) ? 80   : PG_GETARG_INT32(2);
    int    walks = PG_ARGISNULL(3) ? 10   : PG_GETARG_INT32(3);
    double p     = PG_ARGISNULL(4) ? 1.0  : PG_GETARG_FLOAT8(4);
    double q     = PG_ARGISNULL(5) ? 1.0  : PG_GETARG_FLOAT8(5);
    int    win_size    = PG_ARGISNULL(10) ? 10 : PG_GETARG_INT32(10);
    int    iterations  = PG_ARGISNULL(11) ? 1  : PG_GETARG_INT32(11);

    if (dim   <= 0) dim   = 128;
    if (wlen  <= 0) wlen  = 80;
    if (walks <= 0) walks = 10;
    if (win_size   <= 0) win_size   = 10;
    if (iterations <= 0) iterations = 1;

    GraphHandle gh = resolve_graph_args(fcinfo, 6);
    const char *wprop = PG_ARGISNULL(9) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(9));
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    /* Node2Vec (and, transitively, the primitives below) requires
     * contiguous node ids and no isolated nodes. NetworKit::Node2Vec's
     * constructor validates this itself, but since we call
     * BiasedRandomWalk/learnEmbeddings directly (see file header), we
     * replicate the same checks here. */
    if (pg.graph.numberOfNodes() != pg.graph.upperNodeIdBound())
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("node2vec_stream requires contiguous node ids")));

    bool has_isolated = false;
    pg.graph.forNodes([&](NetworKit::node u) {
        if (pg.graph.isIsolated(u))
            has_isolated = true;
    });
    if (has_isolated)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("node2vec_stream does not support graphs with isolated nodes")));

    NetworKit::Embedding::BiasedRandomWalk::AllWalks walks_data;
    NetworKit::Embedding::Embeddings features;
    try {
        NetworKit::Embedding::BiasedRandomWalk brw(&pg.graph);
        brw.preprocessTransitionProbs(p, q);
        walks_data = brw.doWalks((NetworKit::count)wlen, (NetworKit::count)walks);
        features = NetworKit::Embedding::learnEmbeddings(
            walks_data, pg.graph.numberOfNodes(), (NetworKit::count)dim,
            (NetworKit::count)win_size, (NetworKit::count)iterations);
    }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("Node2Vec failed: %s", ex.what())));
    }

    /* Get float8 type info for array construction */
    int16  elmlen;
    bool   elmbyval;
    char   elmalign;
    get_typlenbyvalalign(FLOAT8OID, &elmlen, &elmbyval, &elmalign);

    /* Columns: nodeId bigint, embedding float8[] */
    Oid types[2] = {INT8OID, FLOAT8ARRAYOID};
    SRFState s   = srf_begin(fcinfo, 2, types);

    pg.graph.forNodes([&](NetworKit::node u) {
        if (u >= (NetworKit::count)features.size())
            return;

        const auto &vec = features[u];
        int actual_dim  = (int)vec.size();

        /* Build Datum array for array elements */
        Datum *elems = (Datum *)palloc(actual_dim * sizeof(Datum));
        for (int i = 0; i < actual_dim; i++)
            elems[i] = Float8GetDatum((double)vec[i]);

        ArrayType *arr = construct_array(elems, actual_dim,
                                         FLOAT8OID,
                                         elmlen, elmbyval, elmalign);

        Datum values[2]; bool nulls[2] = {false, false};
        values[0] = Int64GetDatum(pg.id_map.resolve(u));
        values[1] = PointerGetDatum(arr);
        srf_emit(s, values, nulls);
    });

    return srf_end(s, fcinfo);
}

} /* extern "C" */
