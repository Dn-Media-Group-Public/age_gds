// src/algorithms/embedding.cpp
//
// Node embeddings: node2vec_stream using NetworKit::Node2Vec.
// Returns (nodeId bigint, embedding float8[]) for each node.
//
// NetworKit::Node2Vec: Node2Vec(G, P, Q, L, N, D)
//   P = return param, Q = in-out param, L = walk length,
//   N = walks per node, D = embedding dimension.
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <networkit/embedding/Node2Vec.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include <vector>
#include <stdexcept>

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

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_node2vec_stream);

/* ------------------------------------------ node2vec_stream -------------- */
// Args: 0=graph_name, 1=embedding_dim, 2=walk_length, 3=walks_per_node,
//       4=p, 5=q, 6=age_graph, 7=node_query, 8=edge_query

Datum age_gds_node2vec_stream(PG_FUNCTION_ARGS)
{
    int    dim   = PG_ARGISNULL(1) ? 64   : PG_GETARG_INT32(1);
    int    wlen  = PG_ARGISNULL(2) ? 80   : PG_GETARG_INT32(2);
    int    walks = PG_ARGISNULL(3) ? 10   : PG_GETARG_INT32(3);
    double p     = PG_ARGISNULL(4) ? 1.0  : PG_GETARG_FLOAT8(4);
    double q     = PG_ARGISNULL(5) ? 1.0  : PG_GETARG_FLOAT8(5);

    if (dim   <= 0) dim   = 64;
    if (wlen  <= 0) wlen  = 80;
    if (walks <= 0) walks = 10;

    /* age_graph_idx = 6: gname=0, agraph=6, nq=7, eq=8 */
    const char *gname  = PG_ARGISNULL(0) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(6) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(6));
    const char *nq     = PG_ARGISNULL(7) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(7));
    const char *eq     = PG_ARGISNULL(8) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(8));

    GraphHandle gh = get_or_load_graph(gname, agraph, nq, eq);
    const ProjectedGraph &pg = *gh.pg;

    /* Node2Vec requires no isolated nodes; validated by NetworKit internally */
    NetworKit::Node2Vec n2v(
        pg.graph,
        p, q,
        (NetworKit::count)wlen,
        (NetworKit::count)walks,
        (NetworKit::count)dim);

    try { n2v.run(); }
    catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("Node2Vec failed: %s", ex.what())));
    }

    const auto &features = n2v.getFeatures();
    NetworKit::count bound = pg.graph.upperNodeIdBound();

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
