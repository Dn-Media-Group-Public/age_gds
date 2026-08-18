// src/algorithms/similarity.cpp
//
// Node similarity and KNN: nodeSimilarity_stream, knn_stream.
// Uses NetworKit::JaccardIndex for neighborhood-overlap similarity.
//
// See docs/implemented-plans/04-similarity-linkprediction-parity.md for the
// design decisions behind every parameter added in this file.
//
// Include order: NetworKit first, then PG, then restore ERROR.

#include <networkit/linkprediction/JaccardIndex.hpp>
#include <networkit/linkprediction/NeighborhoodUtility.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/graph/GraphTools.hpp>
#include <networkit/components/ConnectedComponents.hpp>
#include <networkit/Globals.hpp>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <optional>
#include <queue>
#include <string>
#include <unordered_set>
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
#include "util/weight_override.h"

/* ============================================================ helpers === */

static GraphHandle resolve_sim_graph(FunctionCallInfo fcinfo, int age_graph_idx)
{
    const char *gname  = PG_ARGISNULL(0)                  ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *agraph = PG_ARGISNULL(age_graph_idx)     ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx));
    const char *nq     = PG_ARGISNULL(age_graph_idx + 1) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 1));
    const char *eq     = PG_ARGISNULL(age_graph_idx + 2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(age_graph_idx + 2));
    return get_or_load_graph(gname, agraph, nq, eq);
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
 * node would hide a caller mistake (same policy as pathfinding.cpp's
 * resolve_node_array). */
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

enum class SimMetric { JACCARD, OVERLAP, COSINE };

static SimMetric parse_sim_metric(const char *s)
{
    if (!s)
        return SimMetric::JACCARD;
    std::string m(s);
    for (auto &c : m) c = (char)toupper((unsigned char)c);
    if (m == "JACCARD") return SimMetric::JACCARD;
    if (m == "OVERLAP") return SimMetric::OVERLAP;
    if (m == "COSINE")  return SimMetric::COSINE;
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("similarity_metric must be one of 'JACCARD', 'OVERLAP', 'COSINE' (got \"%s\")", s)));
    return SimMetric::JACCARD; /* unreachable */
}

/* NetworKit::LinkPredictor::run() (used by JaccardIndex for the unweighted
 * JACCARD path below) unconditionally throws std::invalid_argument on a
 * directed graph (linkprediction/LinkPredictor.cpp: "else if
 * (G->isDirected()) throw std::invalid_argument(...)"), while the manual
 * OVERLAP/COSINE/weighted-Jaccard formulas below use Graph::degree() /
 * NeighborhoodUtility directly and would silently follow out-edges only on
 * a directed graph without erroring. Apply one uniform policy up front for
 * every metric -- convert to an undirected view via
 * NetworKit::GraphTools::toUndirected() -- so results don't vary by which
 * similarity_metric happens to be requested. */
static const NetworKit::Graph &to_undirected_if_needed(const NetworKit::Graph &g,
                                                        NetworKit::Graph &storage)
{
    if (!g.isDirected())
        return g;
    storage = NetworKit::GraphTools::toUndirected(g);
    return storage;
}

static bool degree_ok(const NetworKit::Graph &g, NetworKit::node u,
                      int64_t lo, int64_t hi)
{
    int64_t d = (int64_t)g.degree(u);
    return d >= lo && d <= hi;
}

/* OVERLAP = |A ∩ B| / min(|A|, |B|); COSINE = |A ∩ B| / sqrt(|A|*|B|), both
 * over 0/1 neighbor-membership vectors -- closed-form set arithmetic per
 * docs/plans/04-similarity-linkprediction-parity.md, not a new NetworKit
 * class (NeighborhoodUtility is the same helper NetworKit's own
 * JaccardIndex uses internally). */
static double score_unweighted(const NetworKit::Graph &g, NetworKit::node u, NetworKit::node v,
                               SimMetric metric)
{
    size_t common = NetworKit::NeighborhoodUtility::getCommonNeighbors(g, u, v).size();
    switch (metric) {
    case SimMetric::OVERLAP: {
        NetworKit::count mn = std::min(g.degree(u), g.degree(v));
        return mn == 0 ? 0.0 : (double)common / (double)mn;
    }
    case SimMetric::COSINE: {
        double denom = std::sqrt((double)g.degree(u) * (double)g.degree(v));
        return denom == 0.0 ? 0.0 : (double)common / denom;
    }
    default:
        return 0.0; /* JACCARD is handled via NetworKit::JaccardIndex, not here */
    }
}

/* Weighted-Jaccard: sum(min(w_u,w_v)) / sum(max(w_u,w_v)) over the union of
 * u's and v's neighbor weights -- closed-form arithmetic computed directly
 * from the graph's edge weights, per the plan (NetworKit's JaccardIndex has
 * no weighted variant). */
static double score_weighted_jaccard(const NetworKit::Graph &g, NetworKit::node u, NetworKit::node v)
{
    auto uni = NetworKit::NeighborhoodUtility::getNeighborsUnion(g, u, v);
    double summin = 0.0, summax = 0.0;
    for (NetworKit::node w : uni) {
        double wu = g.hasEdge(u, w) ? g.weight(u, w) : 0.0;
        double wv = g.hasEdge(v, w) ? g.weight(v, w) : 0.0;
        summin += std::min(wu, wv);
        summax += std::max(wu, wv);
    }
    return summax > 0.0 ? summin / summax : 0.0;
}

struct SimEntry { double score; NetworKit::node other; };

/* Min-heap by score: top() is the smallest score currently kept -- used to
 * maintain a bounded "top K highest scores" set. */
struct TopCmp {
    bool operator()(const SimEntry &a, const SimEntry &b) const { return a.score > b.score; }
};
/* Max-heap by score: top() is the largest score currently kept -- used to
 * maintain a bounded "bottom K lowest scores" set. */
struct BottomCmp {
    bool operator()(const SimEntry &a, const SimEntry &b) const { return a.score < b.score; }
};

using TopHeap    = std::priority_queue<SimEntry, std::vector<SimEntry>, TopCmp>;
using BottomHeap = std::priority_queue<SimEntry, std::vector<SimEntry>, BottomCmp>;

struct SimRow { int64_t n1, n2; double score; };

/* ============================================================ C functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_nodesimilarity_stream);
PG_FUNCTION_INFO_V1(age_gds_knn_stream);

/* ------------------------------------------ nodeSimilarity_stream -------- */
// Args: 0=graph_name, 1=age_graph, 2=node_query, 3=edge_query,
//       4=similarity_cutoff, 5=degree_cutoff, 6=top_k, 7=top_n,
//       8=relationship_weight_property, 9=upper_degree_cutoff,
//       10=bottom_k, 11=bottom_n, 12=source_node_filter,
//       13=target_node_filter, 14=use_components, 15=similarity_metric

Datum age_gds_nodesimilarity_stream(PG_FUNCTION_ARGS)
{
    double similarity_cutoff = PG_ARGISNULL(4) ? 1e-42 : PG_GETARG_FLOAT8(4);
    int64_t degree_cutoff    = PG_ARGISNULL(5) ? 1     : (int64_t)PG_GETARG_INT32(5);
    int top_k                = PG_ARGISNULL(6) ? 10    : PG_GETARG_INT32(6);
    int top_n                = PG_ARGISNULL(7) ? 0     : PG_GETARG_INT32(7);
    const char *wprop        = PG_ARGISNULL(8) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(8));
    int64_t upper_degree_cutoff = PG_ARGISNULL(9) ? INT64_MAX : PG_GETARG_INT64(9);
    int bottom_k              = PG_ARGISNULL(10) ? 10 : PG_GETARG_INT32(10);
    int bottom_n              = PG_ARGISNULL(11) ? 0  : PG_GETARG_INT32(11);
    bool has_source_filter    = !PG_ARGISNULL(12);
    std::vector<int64_t> source_gids = has_source_filter ? parse_bigint_array(PG_GETARG_DATUM(12))
                                                          : std::vector<int64_t>();
    bool has_target_filter    = !PG_ARGISNULL(13);
    std::vector<int64_t> target_gids = has_target_filter ? parse_bigint_array(PG_GETARG_DATUM(13))
                                                          : std::vector<int64_t>();
    bool use_components       = PG_ARGISNULL(14) ? false : PG_GETARG_BOOL(14);
    const char *metric_str    = PG_ARGISNULL(15) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(15));

    if (top_k < 0) top_k = 0;
    if (bottom_k < 0) bottom_k = 0;

    SimMetric metric = parse_sim_metric(metric_str);
    bool weighted = (wprop != nullptr);
    if (weighted && metric != SimMetric::JACCARD)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("nodeSimilarity_stream: relationship_weight_property is only "
                        "supported with similarity_metric = 'JACCARD'")));

    /* age_graph_idx = 1 */
    GraphHandle gh = resolve_sim_graph(fcinfo, 1);
    apply_weight_override(gh, wprop);
    const ProjectedGraph &pg = *gh.pg;

    if (weighted && !pg.graph.isWeighted())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("nodeSimilarity_stream: relationship_weight_property was given but "
                        "the resulting graph is not weighted (Graph::isWeighted() == false)"),
                 errdetail("For named graphs (graph_name), this means apply_weight_override "
                           "did not produce a weighted graph. For anonymous graphs "
                           "(age_graph/node_query/edge_query), relationship_weight_property is "
                           "a no-op by design -- write the desired property into edge_query's "
                           "\"weight\" column yourself and pass weighted := true; see "
                           "docs/api.md's \"Relationship weight override\" section.")));

    NetworKit::Graph undirected_storage;
    const NetworKit::Graph &g = to_undirected_if_needed(pg.graph, undirected_storage);

    std::optional<NetworKit::ConnectedComponents> cc;
    if (use_components) {
        cc.emplace(g);
        cc->run();
    }

    NetworKit::JaccardIndex ji(g);
    auto compute_score = [&](NetworKit::node a, NetworKit::node b) -> double {
        if (metric == SimMetric::JACCARD) {
            if (weighted) return score_weighted_jaccard(g, a, b);
            return ji.run(a, b);
        }
        return score_unweighted(g, a, b, metric);
    };

    NetworKit::count bound = g.upperNodeIdBound();
    std::vector<TopHeap>    top_heaps(top_k > 0 ? bound : 0);
    std::vector<BottomHeap> bottom_heaps(bottom_k > 0 ? bound : 0);

    auto push_top = [&](NetworKit::node u, NetworKit::node other, double score) {
        if (top_k <= 0) return;
        TopHeap &h = top_heaps[u];
        if ((int)h.size() < top_k) h.push({score, other});
        else if (score > h.top().score) { h.pop(); h.push({score, other}); }
    };
    auto push_bottom = [&](NetworKit::node u, NetworKit::node other, double score) {
        if (bottom_k <= 0) return;
        BottomHeap &h = bottom_heaps[u];
        if ((int)h.size() < bottom_k) h.push({score, other});
        else if (score < h.top().score) { h.pop(); h.push({score, other}); }
    };
    auto components_ok = [&](NetworKit::node a, NetworKit::node b) {
        return !use_components || cc->componentOfNode(a) == cc->componentOfNode(b);
    };

    if (!has_source_filter && !has_target_filter) {
        g.forNodes([&](NetworKit::node a) {
            if (!degree_ok(g, a, degree_cutoff, upper_degree_cutoff)) return;
            g.forNodes([&](NetworKit::node b) {
                if (b <= a) return;
                if (!degree_ok(g, b, degree_cutoff, upper_degree_cutoff)) return;
                if (!components_ok(a, b)) return;
                double score = compute_score(a, b);
                if (score < similarity_cutoff) return;
                push_top(a, b, score);    push_top(b, a, score);
                push_bottom(a, b, score); push_bottom(b, a, score);
            });
        });
    } else {
        std::vector<NetworKit::node> src_list, tgt_list;
        if (has_source_filter)
            src_list = resolve_node_array(source_gids, pg, "source_node_filter");
        else
            g.forNodes([&](NetworKit::node n) {
                if (degree_ok(g, n, degree_cutoff, upper_degree_cutoff)) src_list.push_back(n);
            });
        if (has_target_filter)
            tgt_list = resolve_node_array(target_gids, pg, "target_node_filter");
        else
            g.forNodes([&](NetworKit::node n) {
                if (degree_ok(g, n, degree_cutoff, upper_degree_cutoff)) tgt_list.push_back(n);
            });

        for (NetworKit::node a : src_list) {
            if (!degree_ok(g, a, degree_cutoff, upper_degree_cutoff)) continue;
            for (NetworKit::node b : tgt_list) {
                if (a == b) continue;
                if (!degree_ok(g, b, degree_cutoff, upper_degree_cutoff)) continue;
                if (!components_ok(a, b)) continue;
                double score = compute_score(a, b);
                if (score < similarity_cutoff) continue;
                push_top(a, b, score);
                push_bottom(a, b, score);
            }
        }
    }

    std::vector<SimRow> top_rows, bottom_rows;
    g.forNodes([&](NetworKit::node u) {
        std::unordered_set<NetworKit::node> top_partners;
        if (top_k > 0) {
            TopHeap &h = top_heaps[u];
            while (!h.empty()) {
                SimEntry e = h.top(); h.pop();
                top_partners.insert(e.other);
                top_rows.push_back({pg.id_map.resolve(u), pg.id_map.resolve(e.other), e.score});
            }
        }
        if (bottom_k > 0) {
            BottomHeap &h = bottom_heaps[u];
            while (!h.empty()) {
                SimEntry e = h.top(); h.pop();
                /* Dedup against the top-K set for this node: with few
                 * candidates, the same partner can land in both bounded
                 * heaps. */
                if (top_partners.count(e.other)) continue;
                bottom_rows.push_back({pg.id_map.resolve(u), pg.id_map.resolve(e.other), e.score});
            }
        }
    });

    if (top_n > 0 && (int)top_rows.size() > top_n) {
        std::sort(top_rows.begin(), top_rows.end(),
                  [](const SimRow &a, const SimRow &b) { return a.score > b.score; });
        top_rows.resize(top_n);
    }
    if (bottom_n > 0 && (int)bottom_rows.size() > bottom_n) {
        std::sort(bottom_rows.begin(), bottom_rows.end(),
                  [](const SimRow &a, const SimRow &b) { return a.score < b.score; });
        bottom_rows.resize(bottom_n);
    }

    Oid types[3] = {INT8OID, INT8OID, FLOAT8OID};
    SRFState s   = srf_begin(fcinfo, 3, types);

    auto emit_rows = [&](const std::vector<SimRow> &rows) {
        for (const SimRow &r : rows) {
            Datum values[3]; bool nulls[3] = {false, false, false};
            values[0] = Int64GetDatum(r.n1);
            values[1] = Int64GetDatum(r.n2);
            values[2] = Float8GetDatum(r.score);
            srf_emit(s, values, nulls);
        }
    };
    emit_rows(top_rows);
    emit_rows(bottom_rows);

    return srf_end(s, fcinfo);
}

/* ------------------------------------------ knn_stream ------------------- */
// Args: 0=graph_name, 1=top_k, 2=age_graph, 3=node_query, 4=edge_query,
//       5=similarity_cutoff
//
// Topology-Jaccard interpretation only (current behavior) -- this remains a
// Jaccard-based substitution for Neo4j's actual property-vector KNN. See
// docs/neo4j-gds-parity-external-libs.md for the genuine NetworKit gap
// (no property-vector / approximate-NN class) this does not attempt to
// close.

Datum age_gds_knn_stream(PG_FUNCTION_ARGS)
{
    int top_k = PG_ARGISNULL(1) ? 10 : PG_GETARG_INT32(1);
    if (top_k <= 0) top_k = 10;
    double similarity_cutoff = PG_ARGISNULL(5) ? 0.0 : PG_GETARG_FLOAT8(5);

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
        if (s < similarity_cutoff) continue;
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
