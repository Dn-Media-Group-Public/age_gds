// src/util/weight_override.cpp
//
// Include order: NetworKit first (its Log.hpp redefines ERROR as a variadic
// macro), then PostgreSQL headers, then restore ERROR to PG's integer 21.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "util/weight_override.h"
#include "bridge/age_loader.h"

#include <stdexcept>
#include <utility>

extern "C" {
#include "postgres.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

void apply_weight_override(GraphHandle &gh, const char *weight_property)
{
    if (!weight_property)
        return;

    /* Anonymous execution: the caller already controls the weight by
     * writing the desired property into their own edge_query's "weight"
     * column, so there is nothing to override here. */
    if (gh.owned.has_value())
        return;

    const ProjectedGraph &pg = *gh.pg;

    if (pg.age_graph.empty() || pg.edge_query.empty())
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("weight_property override requires graph \"%s\" to have "
                        "its age_graph/edge_query recorded in the catalog "
                        "(re-run graph_project or graph_index to populate them)",
                        pg.name.c_str())));

    // reweight_from_property() throws std::runtime_error (e.g. when it
    // cannot locate the "... AS weight" projection to substitute into) —
    // that is a plain C++ exception, and letting it cross the C/C++
    // boundary uncaught would abort the whole backend process instead of
    // returning a SQL-level error. Catch and re-raise via ereport(ERROR).
    NetworKit::Graph reweighted;
    try {
        reweighted = reweight_from_property(
            pg.graph, pg.id_map,
            pg.age_graph.c_str(), pg.edge_query.c_str(), weight_property);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("weight_property override failed for graph \"%s\": %s",
                        pg.name.c_str(), ex.what())));
    }

    ProjectedGraph copy;
    copy.name           = pg.name;
    copy.graph          = std::move(reweighted);
    copy.id_map         = pg.id_map;
    copy.directed       = pg.directed;
    copy.weighted       = true;
    copy.approx_bytes   = pg.approx_bytes;
    copy.last_used       = pg.last_used;
    copy.live           = pg.live;
    copy.age_graph      = pg.age_graph;
    copy.node_query     = pg.node_query;
    copy.edge_query     = pg.edge_query;

    gh.owned.emplace(std::move(copy));
    gh.pg = &*gh.owned;
}
