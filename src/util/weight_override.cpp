// src/util/weight_override.cpp
//
// Include order: NetworKit first (its Log.hpp redefines ERROR as a variadic
// macro), then PostgreSQL headers, then restore ERROR to PG's integer 21.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "util/weight_override.h"
#include "bridge/age_loader.h"

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

    NetworKit::Graph reweighted = reweight_from_property(
        pg.graph, pg.id_map,
        pg.age_graph.c_str(), pg.edge_query.c_str(), weight_property);

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
    copy.edge_query     = pg.edge_query;

    gh.owned.emplace(std::move(copy));
    gh.pg = &*gh.owned;
}
