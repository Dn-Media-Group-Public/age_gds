// src/bridge/node_id_map.h
#pragma once

#include <networkit/Globals.hpp>   // NetworKit::node

#include <cstdint>
#include <unordered_map>
#include <stdexcept>
#include <vector>

struct NodeIdMap {
    std::unordered_map<int64_t,           NetworKit::node> graphid_to_nk;
    std::unordered_map<NetworKit::node,   int64_t>         nk_to_graphid;

    NetworKit::node intern(int64_t gid) {
        auto [it, inserted] = graphid_to_nk.emplace(
            gid, (NetworKit::node) graphid_to_nk.size());
        if (inserted) nk_to_graphid[it->second] = gid;
        return it->second;
    }

    int64_t resolve(NetworKit::node n) const { return nk_to_graphid.at(n); }

    std::vector<int64_t> to_vector() const {
        std::vector<int64_t> v(nk_to_graphid.size());
        for (auto &[nkid, gid] : nk_to_graphid) v[nkid] = gid;
        return v;
    }
};
