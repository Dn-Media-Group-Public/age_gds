// src/util/pair_hash.h
// Hash for std::pair<T1, T2> — used by edge_file_index in ProjectedGraph.
#pragma once

#include <utility>
#include <cstddef>
#include <functional>

struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2> &p) const noexcept
    {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        // Mix: Boost-style hash_combine
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL
                     + 0x6c62272e07bb0142ULL
                     + (h1 << 6) + (h1 >> 2));
    }
};
