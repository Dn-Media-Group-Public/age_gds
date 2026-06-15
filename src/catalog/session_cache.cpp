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

/* ---------------------------------------------------------------- public API */

const ProjectedGraph& session_cache_get(const char *graph_name)
{
    std::string name(graph_name);

    /* Cache hit: refresh timestamp and return */
    auto it = g_cache.find(name);
    if (it != g_cache.end()) {
        it->second.last_used = std::chrono::steady_clock::now();
        return it->second;
    }

    /* Catalog lookup via SPI */
    char file_path_buf[4096] = {0};

    SPI_connect();

    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(graph_name)};

    int ret = SPI_execute_with_args(
        "SELECT file_path FROM age_gds.graphs WHERE name = $1",
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

    bool  isnull = false;
    Datum fp_d   = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull)
        strlcpy(file_path_buf, TextDatumGetCString(fp_d), sizeof(file_path_buf));

    SPI_finish();

    if (file_path_buf[0] == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph \"%s\" has no file_path in catalog", graph_name)));

    /* Deserialize from .bin file */
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

    /* Read GUC limit */
    const char *mb_str     = GetConfigOption("age_gds.session_cache_mb", false, false);
    int64_t     limit_bytes = mb_str ? (int64_t)atoi(mb_str) * 1024LL * 1024LL
                                     : 512LL * 1024LL * 1024LL;

    /* LRU eviction until we have room */
    while (!g_cache.empty() && (int64_t)(g_total_bytes + approx) > limit_bytes)
        evict_lru();

    /* Move deserialized graph into cache entry */
    ProjectedGraph pg;
    pg.name         = name;
    pg.graph        = std::move(dg.graph);
    pg.id_map       = std::move(dg.id_map);
    pg.directed     = dg.directed;
    pg.weighted     = dg.weighted;
    pg.approx_bytes = approx;
    pg.last_used    = std::chrono::steady_clock::now();

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
