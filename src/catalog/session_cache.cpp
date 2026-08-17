// src/catalog/session_cache.cpp
//
// Per-session LRU cache of deserialized NetworKit::Graph objects.
//
// Include order: NetworKit first (its Log.hpp redefines ERROR as a variadic
// macro), then PostgreSQL headers, then restore ERROR to PG's integer 21.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/session_cache.h"
#include "catalog/serialization.h"
#include "catalog/index_storage.h"

#include <unordered_map>
#include <string>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "miscadmin.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ---------------------------------------------------------------- helpers */

/* ---------------------------------------------------------------- cache storage */

static std::unordered_map<std::string, ProjectedGraph> g_cache;
static size_t g_total_bytes = 0;

/* ---------------------------------------------------------------- helpers */

static void evict_lru()
{
    if (g_cache.empty())
        return;

    auto oldest = g_cache.begin();
    for (auto it = std::next(g_cache.begin()); it != g_cache.end(); ++it) {
        if (it->second.last_used < oldest->second.last_used)
            oldest = it;
    }
    g_total_bytes -= oldest->second.approx_bytes;
    g_cache.erase(oldest);
}

/* ---------------------------------------------------------------- live index load */

static void load_live_into_cache(const std::string &name,
                                 const char *nodes_path,
                                 const char *edges_path,
                                 bool        live)
{
    IndexDeserializedGraph idg;
    try {
        idg = index_storage_deserialize(nodes_path, edges_path);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("session_cache: live index deserialize failed: %s", ex.what())));
    }

    uint64_t nc     = idg.graph.numberOfNodes();
    uint64_t ec     = idg.graph.numberOfEdges();
    size_t   approx = (size_t)(nc * 8ULL + ec * 20ULL);

    const char *mb_str     = GetConfigOption("age_gds.session_cache_mb", false, false);
    int64_t     limit_bytes = mb_str ? (int64_t)atoi(mb_str) * 1024LL * 1024LL
                                     : 512LL * 1024LL * 1024LL;

    while (!g_cache.empty() && (int64_t)(g_total_bytes + approx) > limit_bytes)
        evict_lru();

    // Remove any existing stale entry first
    auto old_it = g_cache.find(name);
    if (old_it != g_cache.end()) {
        g_total_bytes -= old_it->second.approx_bytes;
        g_cache.erase(old_it);
    }

    ProjectedGraph pg;
    pg.name             = name;
    pg.graph            = std::move(idg.graph);
    pg.id_map           = std::move(idg.id_map);
    pg.directed         = idg.directed;
    pg.weighted         = idg.weighted;
    pg.approx_bytes     = approx;
    pg.last_used        = std::chrono::steady_clock::now();
    pg.live             = live;
    pg.incremental      = true;
    pg.content_version  = idg.version_counter;
    pg.nodes_file_path  = nodes_path;
    pg.edges_file_path  = edges_path;
    pg.edge_file_index  = std::move(idg.edge_file_index);

    auto [ins_it, ok] = g_cache.emplace(name, std::move(pg));
    (void) ok;
    g_total_bytes += ins_it->second.approx_bytes;
}

/* ---------------------------------------------------------------- public API */

const ProjectedGraph& session_cache_get(const char *graph_name)
{
    std::string name(graph_name);

    /* Check if we have a cached entry */
    {
        auto it = g_cache.find(name);
        if (it != g_cache.end()) {
            ProjectedGraph &pg = it->second;

            if (pg.incremental) {
                /* Slot/tombstone storage (live index or graph_project(incremental
                 * := true)): check version_counter (no cross-backend lock
                 * needed — worst case is a spurious reload). */
                uint64_t current_version = 0;
                try {
                    current_version = index_storage_read_version(
                        pg.nodes_file_path.c_str());
                } catch (...) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("session_cache: failed to read version from \"%s\"",
                                    pg.nodes_file_path.c_str())));
                }

                if (current_version == pg.content_version) {
                    /* Up to date */
                    pg.last_used = std::chrono::steady_clock::now();
                    return pg;
                }

                /* Version mismatch: reload from files */
                std::string nfp = pg.nodes_file_path;
                std::string efp = pg.edges_file_path;
                bool        was_live = pg.live;
                load_live_into_cache(name, nfp.c_str(), efp.c_str(), was_live);
                return g_cache.at(name);
            } else {
                /* Flat storage: check content_version in the .bin header
                 * (cheap pread, no full deserialize). */
                uint64_t current_version = 0;
                try {
                    current_version = gds_read_version(pg.file_path.c_str());
                } catch (...) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("session_cache: failed to read version from \"%s\"",
                                    pg.file_path.c_str())));
                }

                if (current_version == pg.content_version) {
                    pg.last_used = std::chrono::steady_clock::now();
                    return pg;
                }

                /* Version mismatch: reload from file */
                std::string fp = pg.file_path;

                DeserializedGraph dg;
                try {
                    dg = gds_deserialize(fp.c_str());
                } catch (const std::exception &ex) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("session_cache: reload deserialize failed: %s",
                                    ex.what())));
                }

                uint64_t nc     = dg.graph.numberOfNodes();
                uint64_t ec     = dg.graph.numberOfEdges();
                size_t   approx = (size_t)(nc * 8ULL + ec * 20ULL);

                g_total_bytes -= pg.approx_bytes;

                ProjectedGraph new_pg;
                new_pg.name            = name;
                new_pg.graph           = std::move(dg.graph);
                new_pg.id_map          = std::move(dg.id_map);
                new_pg.directed        = dg.directed;
                new_pg.weighted        = dg.weighted;
                new_pg.approx_bytes    = approx;
                new_pg.last_used       = std::chrono::steady_clock::now();
                new_pg.live            = false;
                new_pg.incremental     = false;
                new_pg.content_version = dg.content_version;
                new_pg.file_path       = fp;

                g_cache.erase(it);
                auto [ins_it, ok] = g_cache.emplace(name, std::move(new_pg));
                (void) ok;
                g_total_bytes += ins_it->second.approx_bytes;
                return ins_it->second;
            }
        }
    }

    /* Cache miss — catalog lookup via SPI */
    char     file_path_buf[4096]       = {0};
    char     nodes_file_path_buf[4096] = {0};
    char     edges_file_path_buf[4096] = {0};
    bool     is_live                   = false;
    bool     is_incremental            = false;

    SPI_connect();

    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(graph_name)};

    int ret = SPI_execute_with_args(
        "SELECT file_path, live, nodes_file_path, edges_file_path, incremental "
        "FROM age_gds.graphs WHERE name = $1",
        1, argtypes, args, nullptr, true, 1);

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("session_cache_get: catalog lookup failed (SPI %d)", ret)));
    }

    if (SPI_processed == 0) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("projected graph \"%s\" does not exist", graph_name)));
    }

    {
        bool  isnull;
        /* col 1: file_path */
        Datum d1 = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            strlcpy(file_path_buf, TextDatumGetCString(d1), sizeof(file_path_buf));

        /* col 2: live */
        Datum d2 = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 2, &isnull);
        if (!isnull)
            is_live = DatumGetBool(d2);

        /* col 3: nodes_file_path */
        Datum d3 = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 3, &isnull);
        if (!isnull)
            strlcpy(nodes_file_path_buf, TextDatumGetCString(d3),
                    sizeof(nodes_file_path_buf));

        /* col 4: edges_file_path */
        Datum d4 = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 4, &isnull);
        if (!isnull)
            strlcpy(edges_file_path_buf, TextDatumGetCString(d4),
                    sizeof(edges_file_path_buf));

        /* col 5: incremental */
        Datum d5 = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 5, &isnull);
        if (!isnull)
            is_incremental = DatumGetBool(d5);
    }

    SPI_finish();

    /* ---- Incremental (slot/tombstone) storage path ---- */
    if (is_incremental) {
        if (nodes_file_path_buf[0] == '\0' || edges_file_path_buf[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("incremental graph \"%s\" has no file paths in catalog",
                            graph_name)));

        load_live_into_cache(name, nodes_file_path_buf, edges_file_path_buf, is_live);
        return g_cache.at(name);
    }

    /* ---- Flat storage path ---- */
    if (file_path_buf[0] == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph \"%s\" has no file_path in catalog", graph_name)));

    DeserializedGraph dg;
    try {
        dg = gds_deserialize(file_path_buf);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("session_cache_get: deserialize failed: %s", ex.what())));
    }

    uint64_t nc     = dg.graph.numberOfNodes();
    uint64_t ec     = dg.graph.numberOfEdges();
    size_t   approx = (size_t)(nc * 8ULL + ec * 20ULL);

    const char *mb_str     = GetConfigOption("age_gds.session_cache_mb", false, false);
    int64_t     limit_bytes = mb_str ? (int64_t)atoi(mb_str) * 1024LL * 1024LL
                                     : 512LL * 1024LL * 1024LL;

    while (!g_cache.empty() && (int64_t)(g_total_bytes + approx) > limit_bytes)
        evict_lru();

    ProjectedGraph pg;
    pg.name            = name;
    pg.graph           = std::move(dg.graph);
    pg.id_map          = std::move(dg.id_map);
    pg.directed        = dg.directed;
    pg.weighted        = dg.weighted;
    pg.approx_bytes    = approx;
    pg.last_used       = std::chrono::steady_clock::now();
    pg.live            = false;
    pg.incremental     = false;
    pg.content_version = dg.content_version;
    pg.file_path       = file_path_buf;

    auto [ins_it, ok] = g_cache.emplace(name, std::move(pg));
    (void) ok;
    g_total_bytes += ins_it->second.approx_bytes;

    return ins_it->second;
}

CacheStats session_cache_stats()
{
    const char *mb_str      = GetConfigOption("age_gds.session_cache_mb", false, false);
    int64_t     limit_bytes = mb_str ? (int64_t)atoi(mb_str) * 1024LL * 1024LL
                                     : 512LL * 1024LL * 1024LL;

    return CacheStats{
        (int) g_cache.size(),
        (int64_t) g_total_bytes,
        limit_bytes
    };
}

void session_cache_clear()
{
    g_cache.clear();
    g_total_bytes = 0;
}

void session_cache_invalidate(const char *graph_name)
{
    auto it = g_cache.find(std::string(graph_name));
    if (it != g_cache.end()) {
        g_total_bytes -= it->second.approx_bytes;
        g_cache.erase(it);
    }
}

static void session_cache_exit_cb(int /*code*/, Datum /*arg*/)
{
    session_cache_clear();
}

void session_cache_register_exit_hook()
{
    on_proc_exit(session_cache_exit_cb, (Datum) 0);
}

/* ---------------------------------------------------------------- anonymous-load helper */

GraphHandle get_or_load_graph(
    const char *graph_name,
    const char *age_graph,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted)
{
    if (graph_name && age_graph)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("supply graph_name OR age_graph, not both")));
    if (!graph_name && !age_graph)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("must supply graph_name or age_graph")));

    GraphHandle h;
    if (graph_name) {
        h.pg = &session_cache_get(graph_name);
    } else {
        if (!node_query || !edge_query)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("age_graph requires node_query and edge_query")));
        LoadedGraph lg = age_load_graph(age_graph, node_query, edge_query,
                                        directed, weighted);
        h.owned.emplace();
        h.owned->name         = age_graph;
        h.owned->graph        = std::move(lg.graph);
        h.owned->id_map       = std::move(lg.id_map);
        h.owned->directed     = directed;
        h.owned->weighted     = weighted;
        h.owned->approx_bytes = 0;
        h.owned->last_used    = std::chrono::steady_clock::now();
        h.owned->live         = false;
        h.pg = &*h.owned;
    }
    return h;
}

/* ---------------------------------------------------------------- SQL functions */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_cache_load);
PG_FUNCTION_INFO_V1(age_gds_cache_stats);

Datum age_gds_cache_load(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    session_cache_get(name_str);
    PG_RETURN_VOID();
}

Datum age_gds_cache_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC | MAT_SRF_BLESS);

    Tuplestorestate *tupstore = rsinfo->setResult;
    TupleDesc        tupdesc  = rsinfo->setDesc;

    CacheStats stats = session_cache_stats();

    Datum values[3];
    bool  nulls[3]  = {false, false, false};

    values[0] = Int32GetDatum(stats.entry_count);
    values[1] = Int64GetDatum(stats.total_bytes);
    values[2] = Int64GetDatum(stats.limit_bytes);

    tuplestore_putvalues(tupstore, tupdesc, values, nulls);

    return (Datum) 0;
}

} /* extern "C" */
