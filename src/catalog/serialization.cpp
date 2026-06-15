// src/catalog/serialization.cpp
#include "serialization.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <cstdint>

/* ------------------------------------------------------------------ helpers */

static void checked_write(FILE *f, const void *buf, size_t n, const std::string &path)
{
    if (std::fwrite(buf, 1, n, f) != n)
        throw std::runtime_error("write error on " + path + ": " + std::strerror(errno));
}

static void checked_read(FILE *f, void *buf, size_t n, const std::string &path)
{
    if (std::fread(buf, 1, n, f) != n)
        throw std::runtime_error("read error on " + path + ": " + std::strerror(errno));
}

/* --------------------------------------------------------------- serialize */

void gds_serialize(
    const char             *path,
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    bool                    directed,
    bool                    weighted)
{
    std::string tmp_path = std::string(path) + ".tmp";

    FILE *f = std::fopen(tmp_path.c_str(), "wb");
    if (!f)
        throw std::runtime_error(
            std::string("cannot open ") + tmp_path + ": " + std::strerror(errno));

    try {
        /* Header */
        uint64_t magic      = GDS_MAGIC;
        uint32_t version    = GDS_VERSION;
        uint32_t flags      = (directed ? 1u : 0u) | (weighted ? 2u : 0u);
        uint64_t node_count = graph.numberOfNodes();
        uint64_t edge_count = graph.numberOfEdges();
        uint8_t  reserved[24] = {};

        checked_write(f, &magic,      8, tmp_path);
        checked_write(f, &version,    4, tmp_path);
        checked_write(f, &flags,      4, tmp_path);
        checked_write(f, &node_count, 8, tmp_path);
        checked_write(f, &edge_count, 8, tmp_path);
        checked_write(f, reserved,   24, tmp_path);

        /* Node table */
        auto graphids = id_map.to_vector();
        for (uint64_t i = 0; i < node_count; i++) {
            int64_t gid = graphids[i];
            checked_write(f, &gid, 8, tmp_path);
        }

        /* Edge table */
        graph.forEdges([&](NetworKit::node u, NetworKit::node v,
                           NetworKit::edgeweight w) {
            uint64_t s = u;
            uint64_t d = v;
            double   ww = static_cast<double>(w);
            checked_write(f, &s,  8, tmp_path);
            checked_write(f, &d,  8, tmp_path);
            checked_write(f, &ww, 8, tmp_path);
        });

        std::fclose(f);
        f = nullptr;
    } catch (...) {
        if (f) std::fclose(f);
        std::remove(tmp_path.c_str());
        throw;
    }

    /* Atomic rename */
    if (std::rename(tmp_path.c_str(), path) != 0) {
        std::remove(tmp_path.c_str());
        throw std::runtime_error(
            std::string("cannot rename ") + tmp_path + " -> " + path
            + ": " + std::strerror(errno));
    }
}

/* ------------------------------------------------------------- deserialize */

DeserializedGraph gds_deserialize(const char *path)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        throw std::runtime_error(
            std::string("cannot open ") + path + ": " + std::strerror(errno));

    try {
        /* Header */
        uint64_t magic;
        uint32_t version, flags;
        uint64_t node_count, edge_count;
        uint8_t  reserved[24];

        checked_read(f, &magic,      8, path);
        checked_read(f, &version,    4, path);
        checked_read(f, &flags,      4, path);
        checked_read(f, &node_count, 8, path);
        checked_read(f, &edge_count, 8, path);
        checked_read(f, reserved,   24, path);

        if (magic != GDS_MAGIC)
            throw std::runtime_error(std::string(path) + ": invalid magic");
        if (version != GDS_VERSION)
            throw std::runtime_error(std::string(path) + ": unsupported version");

        bool directed = (flags & 1u) != 0;
        bool weighted = (flags & 2u) != 0;

        /* Node table — build id_map in nk::node order */
        DeserializedGraph result;
        result.directed = directed;
        result.weighted = weighted;

        for (uint64_t i = 0; i < node_count; i++) {
            int64_t gid;
            checked_read(f, &gid, 8, path);
            result.id_map.intern(gid);
        }

        /* Graph */
        result.graph = NetworKit::Graph(node_count, weighted, directed);

        /* Edge table */
        for (uint64_t i = 0; i < edge_count; i++) {
            uint64_t s, d;
            double   w;
            checked_read(f, &s, 8, path);
            checked_read(f, &d, 8, path);
            checked_read(f, &w, 8, path);
            result.graph.addEdge(static_cast<NetworKit::node>(s),
                                 static_cast<NetworKit::node>(d),
                                 static_cast<NetworKit::edgeweight>(w));
        }

        std::fclose(f);
        return result;

    } catch (...) {
        std::fclose(f);
        throw;
    }
}
