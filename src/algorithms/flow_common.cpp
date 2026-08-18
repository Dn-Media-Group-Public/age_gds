// src/algorithms/flow_common.cpp
//
// Include order: NetworKit first (Log.hpp's ERROR macro), then PostgreSQL
// headers if any, restore ERROR afterwards. This file has no PostgreSQL
// dependency at all, but keep the convention for consistency with the rest
// of src/algorithms/.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "algorithms/flow_common.h"

#include <limits>

AugmentedFlowGraph augment_super_source_sink(
    const NetworKit::Graph            &g,
    const std::vector<NetworKit::node> &sources,
    const std::vector<NetworKit::node> &sinks)
{
    double total = 0.0;
    g.forEdges([&](NetworKit::node, NetworKit::node, NetworKit::edgeweight w) {
        total += w;
    });
    double big = total > 0.0 ? total * 2.0 : 1.0;

    bool need_super_source = sources.size() != 1;
    bool need_super_sink   = sinks.size()   != 1;

    NetworKit::Graph out(g, /* weighted */ true, g.isDirected(), /* edgesIndexed */ false);

    NetworKit::node src_node;
    if (need_super_source) {
        src_node = out.addNode();
        for (NetworKit::node s : sources)
            out.addEdge(src_node, s, big);
    } else {
        src_node = sources[0];
    }

    NetworKit::node sink_node;
    if (need_super_sink) {
        sink_node = out.addNode();
        for (NetworKit::node t : sinks)
            out.addEdge(t, sink_node, big);
    } else {
        sink_node = sinks[0];
    }

    out.indexEdges(true);
    return AugmentedFlowGraph{std::move(out), src_node, sink_node};
}

NetworKit::Graph split_node_capacities(
    const NetworKit::Graph    &g,
    const std::vector<double> &node_capacity,
    std::vector<NodeSplit>    &split_out)
{
    const NetworKit::index z = g.upperNodeIdBound();
    constexpr double unconstrained = std::numeric_limits<double>::max() / 4.0;

    NetworKit::Graph out(0, /* weighted */ true, /* directed */ true);
    split_out.assign(z, NodeSplit{NetworKit::none, NetworKit::none});

    for (NetworKit::node u = 0; u < z; u++) {
        if (!g.hasNode(u))
            continue;
        NetworKit::node uin  = out.addNode();
        NetworKit::node uout = out.addNode();
        split_out[u] = NodeSplit{uin, uout};

        double cap = (u < node_capacity.size() && node_capacity[u] > 0.0)
                         ? node_capacity[u]
                         : unconstrained;
        out.addEdge(uin, uout, cap);
    }

    g.forEdges([&](NetworKit::node u, NetworKit::node v, NetworKit::edgeweight w) {
        out.addEdge(split_out[u].out_node, split_out[v].in_node, w);
    });

    out.indexEdges(true);
    return out;
}
