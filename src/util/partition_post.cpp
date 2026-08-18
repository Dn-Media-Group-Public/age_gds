// src/util/partition_post.cpp
//
// Shared partition post-processing helpers for community-detection SQL
// wrappers (src/algorithms/community.cpp): consecutive id renumbering,
// Neo4j GDS minCommunitySize-style filtering, and reading a named AGE node
// property into a NetworKit::Partition or a raw per-node value vector.
//
// Include order: NetworKit first (its Log.hpp redefines ERROR as a variadic
// macro), then PostgreSQL headers, then restore ERROR to PG's integer 21.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "util/partition_post.h"

#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

void consecutive_ids(NetworKit::Partition &p)
{
    p.compact(true);
}

void filter_min_community_size(NetworKit::Partition &p, size_t min_size)
{
    if (min_size <= 1)
        return;

    auto sizes = p.subsetSizeMap();

    std::vector<NetworKit::index> to_remove;
    p.forEntries([&](NetworKit::index e, NetworKit::index s) {
        if (s == NetworKit::none)
            return;
        auto it = sizes.find(s);
        if (it != sizes.end() && it->second < (NetworKit::count)min_size)
            to_remove.push_back(e);
    });

    for (NetworKit::index e : to_remove)
        p.remove(e);
}

namespace {

// Re-run `node_query`, substituting its "RETURN id(<alias>) AS id"
// projection for "RETURN id(<alias>) AS id, <alias>.<property> AS value",
// and return the (graphid, value-text) pairs from the resulting two-column
// query. Throws std::runtime_error if the convention isn't matched.
std::vector<std::pair<int64_t, std::string>> run_node_property_query(
    const char *age_graph_name,
    const char *node_query,
    const char *property)
{
    std::string q(node_query);

    std::regex id_re(R"(id\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s+AS\s+id\b)",
                      std::regex::icase);
    std::smatch m;
    if (!std::regex_search(q, m, id_re))
        throw std::runtime_error(
            "run_node_property_query: could not locate a "
            "\"RETURN id(<alias>) AS id\" projection in the stored "
            "node_query; reading a node property for this call requires "
            "the projection's node_query to follow the documented "
            "convention (see docs/architecture.md)");

    std::string alias = m[1].str();
    std::string replacement =
        m[0].str() + ", " + alias + "." + std::string(property) + " AS value";
    std::string new_q = q.substr(0, (size_t)m.position(0))
                       + replacement
                       + q.substr((size_t)m.position(0) + (size_t)m.length(0));

    SPI_connect();

    std::string sql =
        std::string("SELECT * FROM ag_catalog.cypher('")
        + age_graph_name
        + "', $$ " + new_q
        + " $$) AS (id agtype, value agtype)";

    int ret = SPI_execute(sql.c_str(), true, 0);
    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        throw std::runtime_error(
            "run_node_property_query: node query failed (SPI code "
            + std::to_string(ret) + ")");
    }

    std::vector<std::pair<int64_t, std::string>> result;
    uint64_t nrows = SPI_processed;
    result.reserve(nrows);

    for (uint64_t i = 0; i < nrows; i++) {
        HeapTuple tup  = SPI_tuptable->vals[i];
        TupleDesc desc = SPI_tuptable->tupdesc;

        char *id_txt = SPI_getvalue(tup, desc, 1);
        if (!id_txt)
            continue;
        int64_t gid = (int64_t) strtoll(id_txt, nullptr, 10);

        char *val_txt = SPI_getvalue(tup, desc, 2);
        result.emplace_back(gid, val_txt ? std::string(val_txt) : std::string());
    }

    SPI_finish();
    return result;
}

} // namespace

NetworKit::Partition read_node_property_as_partition(
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *node_query,
    const char             *property)
{
    NetworKit::index z = graph.upperNodeIdBound();

    std::vector<NetworKit::index> ids(z);
    for (NetworKit::index i = 0; i < z; i++)
        ids[i] = i;   // default: every node its own singleton community

    auto rows = run_node_property_query(age_graph_name, node_query, property);

    std::unordered_map<std::string, NetworKit::index> value_to_id;
    NetworKit::index next_id = z;   // strictly above every default singleton id

    for (auto &[gid, value] : rows) {
        auto it = id_map.graphid_to_nk.find(gid);
        if (it == id_map.graphid_to_nk.end())
            continue;   // node outside the cached topology

        auto ins = value_to_id.try_emplace(value, next_id);
        if (ins.second)
            ++next_id;

        ids[it->second] = ins.first->second;
    }

    return NetworKit::Partition(ids);
}

std::vector<double> read_node_property_as_values(
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    const char             *age_graph_name,
    const char             *node_query,
    const char             *property,
    double                  default_value)
{
    std::vector<double> values(graph.upperNodeIdBound(), default_value);

    auto rows = run_node_property_query(age_graph_name, node_query, property);
    for (auto &[gid, value_text] : rows) {
        auto it = id_map.graphid_to_nk.find(gid);
        if (it == id_map.graphid_to_nk.end())
            continue;
        if (value_text.empty())
            continue;
        values[it->second] = strtod(value_text.c_str(), nullptr);
    }

    return values;
}
