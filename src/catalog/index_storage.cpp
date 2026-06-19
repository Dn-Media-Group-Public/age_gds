// src/catalog/index_storage.cpp
//
// Binary file I/O for live graph index files (.nodes, .edges).
// Pure C++ — no PostgreSQL headers.  Callers manage LWLock coordination.

#include "catalog/index_storage.h"

#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
}

// ---------------------------------------------------------------------------
// On-disk struct layouts (must match header comment)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct NodesFileHeader {
    uint64_t magic;            // 0   = INDEX_NODES_MAGIC
    uint64_t version_counter;  // 8   incremented on every mutation
    uint64_t node_capacity;    // 16  total slots (including tombstones)
    uint64_t live_node_count;  // 24  non-tombstoned slots
    uint32_t flags;            // 32  bit0=directed, bit1=weighted
    uint8_t  reserved[28];     // 36
};                             // total: 64 bytes
static_assert(sizeof(NodesFileHeader) == 64, "NodesFileHeader size mismatch");

struct NodeRecord {
    int64_t graphid;           // 0   AGE graphid; -1 = tombstoned
    uint8_t dead;              // 8   1 = tombstoned
    uint8_t padding[7];        // 9
};                             // total: 16 bytes
static_assert(sizeof(NodeRecord) == 16, "NodeRecord size mismatch");

struct EdgesFileHeader {
    uint64_t magic;            // 0   = INDEX_EDGES_MAGIC
    uint64_t edge_capacity;    // 8   total slots (including tombstones)
    uint64_t live_edge_count;  // 16
    uint8_t  reserved[8];      // 24
};                             // total: 32 bytes
static_assert(sizeof(EdgesFileHeader) == 32, "EdgesFileHeader size mismatch");

struct EdgeRecord {
    uint64_t src_nk;           // 0   nk::node index of source
    uint64_t dst_nk;           // 8   nk::node index of destination
    double   weight;           // 16  1.0 if unweighted
    uint8_t  dead;             // 24  1 = tombstoned
    uint8_t  padding[7];       // 25
};                             // total: 32 bytes
static_assert(sizeof(EdgeRecord) == 32, "EdgeRecord size mismatch");

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static int open_rdwr(const char *path)
{
    int fd = ::open(path, O_RDWR);
    if (fd < 0)
        throw std::runtime_error(
            std::string("index_storage: cannot open ") + path + ": " + strerror(errno));
    return fd;
}

static int open_create(const char *path)
{
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        throw std::runtime_error(
            std::string("index_storage: cannot create ") + path + ": " + strerror(errno));
    return fd;
}

static void pw_checked(int fd, const void *buf, size_t n, off_t off, const char *path)
{
    ssize_t res = pwrite(fd, buf, n, off);
    if (res < 0 || static_cast<size_t>(res) != n)
        throw std::runtime_error(
            std::string("index_storage: pwrite error on ") + path + ": " + strerror(errno));
}

static void pr_checked(int fd, void *buf, size_t n, off_t off, const char *path)
{
    ssize_t res = pread(fd, buf, n, off);
    if (res < 0 || static_cast<size_t>(res) != n)
        throw std::runtime_error(
            std::string("index_storage: pread error on ") + path + ": " + strerror(errno));
}

static NodesFileHeader read_nodes_hdr(int fd, const char *path)
{
    NodesFileHeader hdr{};
    pr_checked(fd, &hdr, sizeof(hdr), 0, path);
    if (hdr.magic != INDEX_NODES_MAGIC)
        throw std::runtime_error(std::string(path) + ": invalid nodes file magic");
    return hdr;
}

static EdgesFileHeader read_edges_hdr(int fd, const char *path)
{
    EdgesFileHeader hdr{};
    pr_checked(fd, &hdr, sizeof(hdr), 0, path);
    if (hdr.magic != INDEX_EDGES_MAGIC)
        throw std::runtime_error(std::string(path) + ": invalid edges file magic");
    return hdr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void index_storage_create(
    const char *nodes_path,
    const char *edges_path,
    bool directed,
    bool weighted)
{
    // Nodes file
    {
        int fd = open_create(nodes_path);
        NodesFileHeader hdr{};
        hdr.magic            = INDEX_NODES_MAGIC;
        hdr.version_counter  = 0;
        hdr.node_capacity    = 0;
        hdr.live_node_count  = 0;
        hdr.flags            = (directed ? 1u : 0u) | (weighted ? 2u : 0u);
        pw_checked(fd, &hdr, sizeof(hdr), 0, nodes_path);
        ::close(fd);
    }
    // Edges file
    {
        int fd = open_create(edges_path);
        EdgesFileHeader hdr{};
        hdr.magic           = INDEX_EDGES_MAGIC;
        hdr.edge_capacity   = 0;
        hdr.live_edge_count = 0;
        pw_checked(fd, &hdr, sizeof(hdr), 0, edges_path);
        ::close(fd);
    }
}

void index_storage_write_initial(
    const char             *nodes_path,
    const char             *edges_path,
    const NetworKit::Graph &graph,
    const NodeIdMap        &id_map,
    bool                    directed,
    bool                    weighted)
{
    uint64_t nc = graph.numberOfNodes();
    uint64_t ec = graph.numberOfEdges();

    // Nodes file
    {
        int fd = open_create(nodes_path);
        try {
            NodesFileHeader hdr{};
            hdr.magic           = INDEX_NODES_MAGIC;
            hdr.version_counter = 1;
            hdr.node_capacity   = nc;
            hdr.live_node_count = nc;
            hdr.flags           = (directed ? 1u : 0u) | (weighted ? 2u : 0u);
            pw_checked(fd, &hdr, sizeof(hdr), 0, nodes_path);

            auto graphids = id_map.to_vector();
            for (uint64_t i = 0; i < nc; i++) {
                NodeRecord rec{};
                rec.graphid = graphids[i];
                rec.dead    = 0;
                off_t off   = static_cast<off_t>(sizeof(hdr) + i * sizeof(NodeRecord));
                pw_checked(fd, &rec, sizeof(rec), off, nodes_path);
            }
            ::close(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    // Edges file
    {
        int fd = open_create(edges_path);
        try {
            EdgesFileHeader hdr{};
            hdr.magic           = INDEX_EDGES_MAGIC;
            hdr.edge_capacity   = ec;
            hdr.live_edge_count = ec;
            pw_checked(fd, &hdr, sizeof(hdr), 0, edges_path);

            uint64_t slot = 0;
            graph.forEdges([&](NetworKit::node u, NetworKit::node v,
                               NetworKit::edgeweight w) {
                EdgeRecord rec{};
                rec.src_nk = static_cast<uint64_t>(u);
                rec.dst_nk = static_cast<uint64_t>(v);
                rec.weight = static_cast<double>(w);
                rec.dead   = 0;
                off_t off  = static_cast<off_t>(sizeof(hdr) + slot * sizeof(EdgeRecord));
                pw_checked(fd, &rec, sizeof(rec), off, edges_path);
                ++slot;
            });
            ::close(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
    }
}

uint64_t index_storage_read_version(const char *nodes_path)
{
    int fd = ::open(nodes_path, O_RDONLY);
    if (fd < 0)
        throw std::runtime_error(
            std::string("index_storage: cannot open ") + nodes_path + ": " + strerror(errno));
    uint64_t version = 0;
    try {
        // version_counter is at byte offset 8 in the header
        pr_checked(fd, &version, sizeof(version),
                   static_cast<off_t>(offsetof(NodesFileHeader, version_counter)),
                   nodes_path);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return version;
}

uint64_t index_storage_add_node(const char *nodes_path, int64_t graphid)
{
    int fd = open_rdwr(nodes_path);
    try {
        NodesFileHeader hdr = read_nodes_hdr(fd, nodes_path);
        uint64_t slot       = hdr.node_capacity;

        NodeRecord rec{};
        rec.graphid = graphid;
        rec.dead    = 0;
        off_t off   = static_cast<off_t>(sizeof(hdr) + slot * sizeof(NodeRecord));
        pw_checked(fd, &rec, sizeof(rec), off, nodes_path);

        hdr.node_capacity++;
        hdr.live_node_count++;
        pw_checked(fd, &hdr, sizeof(hdr), 0, nodes_path);

        ::close(fd);
        return slot;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

uint64_t index_storage_find_node(const char *nodes_path, int64_t graphid)
{
    int fd = ::open(nodes_path, O_RDONLY);
    if (fd < 0)
        throw std::runtime_error(
            std::string("index_storage: cannot open ") + nodes_path + ": " + strerror(errno));
    try {
        NodesFileHeader hdr = read_nodes_hdr(fd, nodes_path);
        for (uint64_t i = 0; i < hdr.node_capacity; i++) {
            NodeRecord rec{};
            pr_checked(fd, &rec, sizeof(rec),
                       static_cast<off_t>(sizeof(hdr) + i * sizeof(NodeRecord)),
                       nodes_path);
            if (!rec.dead && rec.graphid == graphid) {
                ::close(fd);
                return i;
            }
        }
        ::close(fd);
        return UINT64_MAX;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void index_storage_kill_node(const char *nodes_path, uint64_t slot)
{
    int fd = open_rdwr(nodes_path);
    try {
        NodesFileHeader hdr = read_nodes_hdr(fd, nodes_path);

        NodeRecord rec{};
        off_t rec_off = static_cast<off_t>(sizeof(hdr) + slot * sizeof(NodeRecord));
        pr_checked(fd, &rec, sizeof(rec), rec_off, nodes_path);
        rec.dead    = 1;
        rec.graphid = -1;
        pw_checked(fd, &rec, sizeof(rec), rec_off, nodes_path);

        if (hdr.live_node_count > 0) hdr.live_node_count--;
        pw_checked(fd, &hdr, sizeof(hdr), 0, nodes_path);

        ::close(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

uint64_t index_storage_add_edge(
    const char *edges_path,
    uint64_t    src_nk,
    uint64_t    dst_nk,
    double      weight)
{
    int fd = open_rdwr(edges_path);
    try {
        EdgesFileHeader hdr = read_edges_hdr(fd, edges_path);
        uint64_t slot       = hdr.edge_capacity;

        EdgeRecord rec{};
        rec.src_nk = src_nk;
        rec.dst_nk = dst_nk;
        rec.weight = weight;
        rec.dead   = 0;
        off_t off  = static_cast<off_t>(sizeof(hdr) + slot * sizeof(EdgeRecord));
        pw_checked(fd, &rec, sizeof(rec), off, edges_path);

        hdr.edge_capacity++;
        hdr.live_edge_count++;
        pw_checked(fd, &hdr, sizeof(hdr), 0, edges_path);

        ::close(fd);
        return slot;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

uint64_t index_storage_find_edge(
    const char *edges_path,
    uint64_t    src_nk,
    uint64_t    dst_nk)
{
    int fd = ::open(edges_path, O_RDONLY);
    if (fd < 0)
        throw std::runtime_error(
            std::string("index_storage: cannot open ") + edges_path + ": " + strerror(errno));
    try {
        EdgesFileHeader hdr = read_edges_hdr(fd, edges_path);
        for (uint64_t i = 0; i < hdr.edge_capacity; i++) {
            EdgeRecord rec{};
            pr_checked(fd, &rec, sizeof(rec),
                       static_cast<off_t>(sizeof(hdr) + i * sizeof(EdgeRecord)),
                       edges_path);
            if (!rec.dead && rec.src_nk == src_nk && rec.dst_nk == dst_nk) {
                ::close(fd);
                return i;
            }
        }
        ::close(fd);
        return UINT64_MAX;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void index_storage_kill_edge(const char *edges_path, uint64_t slot)
{
    int fd = open_rdwr(edges_path);
    try {
        EdgesFileHeader hdr = read_edges_hdr(fd, edges_path);

        EdgeRecord rec{};
        off_t rec_off = static_cast<off_t>(sizeof(hdr) + slot * sizeof(EdgeRecord));
        pr_checked(fd, &rec, sizeof(rec), rec_off, edges_path);
        rec.dead = 1;
        pw_checked(fd, &rec, sizeof(rec), rec_off, edges_path);

        if (hdr.live_edge_count > 0) hdr.live_edge_count--;
        pw_checked(fd, &hdr, sizeof(hdr), 0, edges_path);

        ::close(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void index_storage_bump_version(const char *nodes_path)
{
    int fd = open_rdwr(nodes_path);
    try {
        NodesFileHeader hdr = read_nodes_hdr(fd, nodes_path);
        hdr.version_counter++;
        pw_checked(fd, &hdr, sizeof(hdr), 0, nodes_path);
        ::close(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

IndexDeserializedGraph index_storage_deserialize(
    const char *nodes_path,
    const char *edges_path)
{
    IndexDeserializedGraph result;

    // --- Pass 1: read node records, build compact graph + id_map ---
    //
    // slot_to_nk[i] = nk::node for slot i, or NetworKit::none if dead/unused.
    std::vector<NetworKit::node> slot_to_nk;

    {
        int fd = ::open(nodes_path, O_RDONLY);
        if (fd < 0)
            throw std::runtime_error(
                std::string("index_storage: cannot open ") + nodes_path + ": " + strerror(errno));
        try {
            NodesFileHeader hdr = read_nodes_hdr(fd, nodes_path);
            result.directed       = (hdr.flags & 1u) != 0;
            result.weighted       = (hdr.flags & 2u) != 0;
            result.version_counter = hdr.version_counter;

            // Create empty graph (0 nodes); nodes are added one by one.
            result.graph = NetworKit::Graph(0, result.weighted, result.directed);

            slot_to_nk.resize(hdr.node_capacity, NetworKit::none);

            for (uint64_t i = 0; i < hdr.node_capacity; i++) {
                NodeRecord rec{};
                pr_checked(fd, &rec, sizeof(rec),
                           static_cast<off_t>(sizeof(hdr) + i * sizeof(NodeRecord)),
                           nodes_path);
                if (rec.dead || rec.graphid == -1)
                    continue;

                NetworKit::node nk = result.graph.addNode();
                result.id_map.graphid_to_nk[rec.graphid] = nk;
                result.id_map.nk_to_graphid[nk]          = rec.graphid;
                slot_to_nk[i] = nk;
            }
            ::close(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    // --- Pass 2: read edge records ---
    {
        int fd = ::open(edges_path, O_RDONLY);
        if (fd < 0)
            throw std::runtime_error(
                std::string("index_storage: cannot open ") + edges_path + ": " + strerror(errno));
        try {
            EdgesFileHeader hdr = read_edges_hdr(fd, edges_path);

            for (uint64_t i = 0; i < hdr.edge_capacity; i++) {
                EdgeRecord rec{};
                pr_checked(fd, &rec, sizeof(rec),
                           static_cast<off_t>(sizeof(hdr) + i * sizeof(EdgeRecord)),
                           edges_path);
                if (rec.dead)
                    continue;
                // Skip if either endpoint slot is dead / out-of-range
                if (rec.src_nk >= slot_to_nk.size()) continue;
                if (rec.dst_nk >= slot_to_nk.size()) continue;
                NetworKit::node sn = slot_to_nk[rec.src_nk];
                NetworKit::node dn = slot_to_nk[rec.dst_nk];
                if (sn == NetworKit::none || dn == NetworKit::none)
                    continue;

                result.graph.addEdge(sn, dn,
                                     static_cast<NetworKit::edgeweight>(rec.weight));
                result.edge_file_index[{sn, dn}] = i;
            }
            ::close(fd);
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    return result;
}

void index_storage_compact(
    const char *nodes_path,
    const char *edges_path)
{
    std::string n_tmp = std::string(nodes_path) + ".tmp";
    std::string e_tmp = std::string(edges_path) + ".tmp";

    // Deserialise current state into compact graph
    IndexDeserializedGraph dg = index_storage_deserialize(nodes_path, edges_path);

    // Write compact files to .tmp locations
    index_storage_write_initial(
        n_tmp.c_str(), e_tmp.c_str(),
        dg.graph, dg.id_map,
        dg.directed, dg.weighted);

    // Atomic rename
    if (std::rename(n_tmp.c_str(), nodes_path) != 0) {
        std::remove(n_tmp.c_str());
        throw std::runtime_error(
            std::string("index_storage_compact: rename nodes failed: ") + strerror(errno));
    }
    if (std::rename(e_tmp.c_str(), edges_path) != 0) {
        std::remove(e_tmp.c_str());
        throw std::runtime_error(
            std::string("index_storage_compact: rename edges failed: ") + strerror(errno));
    }
}
