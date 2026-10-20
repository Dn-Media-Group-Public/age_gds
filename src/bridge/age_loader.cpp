// src/bridge/age_loader.cpp
//
// Load an AGE Cypher graph into a NetworKit::Graph via SPI.
//
// Include order: NetworKit first, then PostgreSQL headers, then #undef ERROR.
// NetworKit's Log.hpp redefines ERROR as a variadic macro that shadows the
// PostgreSQL integer constant.  We restore it after all includes.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "bridge/age_loader.h"

#include <string>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <stdexcept>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ------------------------------------------------------------------ */

/* A NULL/empty query means "the whole AGE graph", read with plain SQL from
 * the label-table inheritance parents instead of through Cypher (whose
 * planner joins every edge to both endpoint vertices). */
static bool is_unset(const char *q) { return !q || !*q; }

/* Weight text from SQL or Cypher: missing / NULL / non-numeric -> 1.0. */
static double parse_weight(const char *txt)
{
    if (!txt) return 1.0;
    char  *end;
    double w = strtod(txt, &end);
    return end == txt ? 1.0 : w;
}

/* JSON string literal of `s`, passed as a bound parameter and cast to agtype. */
static std::string json_quote(const char *s)
{
    std::string out = "\"";
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') { out += '\\'; out += (char) c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char) c;
    }
    return out + "\"";
}

/* "<schema>._ag_label_vertex" / "_ag_label_edge", schema quoted as identifier. */
static std::string label_parent(const char *age_graph_name, const char *parent)
{
    return std::string(quote_identifier(age_graph_name)) + "." + parent;
}

/* SELECT <key>, <agtype property> FROM <parent>; property name is bound. */
static int exec_property_select(const char *key_cols, const std::string &parent,
                                const char *property)
{
    std::string sql = std::string("SELECT ") + key_cols
        + ", ag_catalog.agtype_access_operator(properties, $1::ag_catalog.agtype) FROM "
        + parent;
    std::string prop = json_quote(property);
    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(prop.c_str())};
    return SPI_execute_with_args(sql.c_str(), 1, argtypes, args, nullptr, true, 0);
}

/* Cursor-free SPI helper shared by the property readers (see header). */
int age_spi_read_node_property(const char *age_graph_name, const char *property)
{
    return exec_property_select("id", label_parent(age_graph_name, "_ag_label_vertex"),
                                property);
}

LoadedGraph age_load_graph(
    const char *age_graph_name,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted)
{
    LoadedGraph result;
    result.graph = NetworKit::Graph(0, weighted, directed);

    if (is_unset(node_query) != is_unset(edge_query))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("node_query and edge_query must both be given or both "
                        "omitted (omitted = the whole graph)")));
    const bool whole = is_unset(node_query);

    SPI_connect();

    /* ---- 1. Verify the AGE graph exists ---- */
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(age_graph_name)};

        int ret = SPI_execute_with_args(
            "SELECT 1 FROM ag_catalog.ag_graph WHERE name = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: graph existence check failed"
                            " (SPI code %d)", ret)));
        }

        if (SPI_processed == 0) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("AGE graph \"%s\" does not exist",
                            age_graph_name)));
        }
    }

    /* ---- 2. Load nodes ---- */
    {
        std::string sql = whole
            ? "SELECT id FROM " + label_parent(age_graph_name, "_ag_label_vertex")
            : std::string("SELECT * FROM ag_catalog.cypher('")
              + age_graph_name
              + "', $$ " + node_query + " $$) AS (id agtype)";

        int ret = SPI_execute(sql.c_str(), true, 0);
        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: node query failed"
                            " (SPI code %d)", ret)));
        }

        uint64_t nrows = SPI_processed;
        for (uint64_t i = 0; i < nrows; i++) {
            char *txt = SPI_getvalue(SPI_tuptable->vals[i],
                                     SPI_tuptable->tupdesc, 1);
            if (!txt)
                continue;
            int64_t gid = (int64_t) strtoll(txt, nullptr, 10);
            result.id_map.intern(gid);
            result.graph.addNode();
        }
    }

    /* ---- 3. Load edges ---- */
    {
        /* Unweighted whole-graph reads never touch `properties`. */
        std::string sql;
        if (whole) {
            std::string parent = label_parent(age_graph_name, "_ag_label_edge");
            sql = weighted
                ? "SELECT start_id, end_id, "
                  "ag_catalog.agtype_access_operator(properties, '\"weight\"'::ag_catalog.agtype) FROM "
                  + parent
                : "SELECT start_id, end_id FROM " + parent;
        } else {
            sql = std::string("SELECT * FROM ag_catalog.cypher('")
                + age_graph_name
                + "', $$ " + edge_query
                + " $$) AS (src agtype, dst agtype, weight agtype)";
        }

        int ret = SPI_execute(sql.c_str(), true, 0);
        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: edge query failed"
                            " (SPI code %d)", ret)));
        }

        int      ncols = SPI_tuptable->tupdesc->natts;
        uint64_t nrows = SPI_processed;

        for (uint64_t i = 0; i < nrows; i++) {
            HeapTuple tup  = SPI_tuptable->vals[i];
            TupleDesc desc = SPI_tuptable->tupdesc;

            char *src_txt = SPI_getvalue(tup, desc, 1);
            char *dst_txt = SPI_getvalue(tup, desc, 2);
            if (!src_txt || !dst_txt)
                continue;

            int64_t src_gid = (int64_t) strtoll(src_txt, nullptr, 10);
            int64_t dst_gid = (int64_t) strtoll(dst_txt, nullptr, 10);

            auto sit = result.id_map.graphid_to_nk.find(src_gid);
            auto dit = result.id_map.graphid_to_nk.find(dst_gid);
            if (sit == result.id_map.graphid_to_nk.end() ||
                dit == result.id_map.graphid_to_nk.end())
                continue;   /* edge references an unknown node — skip */

            NetworKit::node src_nk = sit->second;
            NetworKit::node dst_nk = dit->second;

            double w = 1.0;
            if (ncols >= 3)
                w = parse_weight(SPI_getvalue(tup, desc, 3));

            if (weighted)
                result.graph.addEdge(src_nk, dst_nk,
                                     static_cast<NetworKit::edgeweight>(w));
            else
                result.graph.addEdge(src_nk, dst_nk);
        }
    }

    SPI_finish();
    return result;
}

/* ------------------------------------------------------------------ */

NetworKit::Graph reweight_from_property(
    const NetworKit::Graph &g,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *edge_query,
    const char             *weight_property)
{
    /* Weighted copy of the cached topology; edges not touched below keep
     * NetworKit's defaultEdgeWeight (1.0), matching age_load_graph's
     * fallback when a row's weight expression evaluates to NULL. */
    NetworKit::Graph result(g, /* weighted */ true, g.isDirected(),
                            /* edgesIndexed */ false);

    SPI_connect();
    int ret;

    if (is_unset(edge_query)) {
        /* Whole-graph projection: read the property straight from the edge
         * label parent (restricted to edges already in `g` below). */
        ret = exec_property_select("start_id, end_id",
                                   label_parent(age_graph_name, "_ag_label_edge"),
                                   weight_property);
    } else {
        /* Find the relationship alias bound in the MATCH clause, e.g.
         * "-[r:KNOWS]->" or "-[r]->", so the substituted projection reads
         * `<alias>.<weight_property>` rather than assuming the alias "r". */
        std::string q(edge_query);
        std::regex  alias_re(R"(-\[\s*([A-Za-z_][A-Za-z0-9_]*))");
        std::smatch alias_m;
        std::string alias = "r";
        if (std::regex_search(q, alias_m, alias_re))
            alias = alias_m[1].str();

        /* Replace the third RETURN projection (originally aliased "AS weight")
         * with `<alias>.<weight_property>`, leaving src/dst untouched. */
        std::regex  weight_re(R"((,\s*)([^,]+?)(\s+AS\s+weight\b))",
                              std::regex::icase);
        std::string replacement = "$1" + alias + "." + std::string(weight_property) + "$3";
        std::string new_q = std::regex_replace(q, weight_re, replacement,
                                                std::regex_constants::format_first_only);

        if (new_q == q) {
            SPI_finish();
            throw std::runtime_error(
                "reweight_from_property: could not locate a \"... AS weight\" "
                "projection in the stored edge_query; weight_property override "
                "requires the projection's edge_query to follow the documented "
                "convention (see docs/architecture.md)");
        }

        std::string sql =
            std::string("SELECT * FROM ag_catalog.cypher('")
            + age_graph_name
            + "', $$ " + new_q
            + " $$) AS (src agtype, dst agtype, weight agtype)";
        ret = SPI_execute(sql.c_str(), true, 0);
    }

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("reweight_from_property: edge query failed"
                        " (SPI code %d)", ret)));
    }

    uint64_t nrows = SPI_processed;
    for (uint64_t i = 0; i < nrows; i++) {
        HeapTuple tup  = SPI_tuptable->vals[i];
        TupleDesc desc = SPI_tuptable->tupdesc;

        char *src_txt = SPI_getvalue(tup, desc, 1);
        char *dst_txt = SPI_getvalue(tup, desc, 2);
        if (!src_txt || !dst_txt)
            continue;

        int64_t src_gid = (int64_t) strtoll(src_txt, nullptr, 10);
        int64_t dst_gid = (int64_t) strtoll(dst_txt, nullptr, 10);

        auto sit = id_map.graphid_to_nk.find(src_gid);
        auto dit = id_map.graphid_to_nk.find(dst_gid);
        if (sit == id_map.graphid_to_nk.end() ||
            dit == id_map.graphid_to_nk.end())
            continue;   /* edge references a node outside the cached topology */

        NetworKit::node src_nk = sit->second;
        NetworKit::node dst_nk = dit->second;

        /* Never let this call introduce edges the cached projection
         * filtered out — setWeight() on a nonexistent pair would insert
         * one and silently inflate the edge count. */
        if (!result.hasEdge(src_nk, dst_nk))
            continue;

        double w = parse_weight(SPI_getvalue(tup, desc, 3));

        result.setWeight(src_nk, dst_nk, static_cast<NetworKit::edgeweight>(w));
    }

    SPI_finish();
    return result;
}
