// src/catalog/graph_project.cpp

// NetworKit first — its Log.hpp defines `#define ERROR(...)` as a variadic
// function-like macro.  We #undef it below after including PG headers.
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/serialization.h"
#include "catalog/session_cache.h"
#include "bridge/age_loader.h"

#include <string>
#include <stdexcept>
#include <vector>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"
#include "utils/memutils.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "catalog/pg_type.h"
#include "utils/guc.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
} // extern "C"

/*
 * NetworKit's Log.hpp (#included transitively above) redefines ERROR as a
 * variadic function-like macro, which shadows PostgreSQL's integer constant.
 * Restore the PostgreSQL definition explicitly.
 */
#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ============================================================ helpers ===== */

/*
 * Expand $PGDATA prefix using DataDir from miscadmin.h.
 * Returns palloc'd string.
 */
static char *expand_data_dir(const char *raw)
{
    if (strncmp(raw, "$PGDATA", 7) == 0) {
        const char *pgdata = DataDir ? DataDir : "";
        size_t      len    = strlen(pgdata) + strlen(raw + 7) + 1;
        char       *result = (char *) palloc(len);
        snprintf(result, len, "%s%s", pgdata, raw + 7);
        return result;
    }
    return pstrdup(raw);
}

/*
 * Recursive mkdir — equivalent of mkdir -p.
 * Uses 0700 permissions (PostgreSQL convention for data directories).
 */
static void mkdirp(const char *path)
{
    char   tmp[MAXPGPATH];
    size_t len = strlen(path);

    if (len >= MAXPGPATH)
        ereport(ERROR, (errmsg("data_dir path too long")));

    memcpy(tmp, path, len + 1);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                ereport(ERROR,
                        (errmsg("could not create directory \"%s\": %m", tmp)));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        ereport(ERROR,
                (errmsg("could not create directory \"%s\": %m", tmp)));
}

/* ========================================================= XactCallback === */

/*
 * On transaction abort, delete the .bin file that was just created by
 * graph_project so we don't leave orphaned files.  Registered once per
 * backend; only acts when g_pending_bin is non-empty.
 */
static char g_pending_bin[MAXPGPATH]  = {0};
static bool g_xact_cb_registered     = false;

static void cleanup_bin_xact_cb(XactEvent event, void * /*arg*/)
{
    if (g_pending_bin[0] != '\0') {
        if (event == XACT_EVENT_ABORT)
            (void) unlink(g_pending_bin);
        g_pending_bin[0] = '\0';
    }
}

/* ============================================================ C functions == */

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_graph_project);
PG_FUNCTION_INFO_V1(age_gds_graph_drop);
PG_FUNCTION_INFO_V1(age_gds_graph_list);
PG_FUNCTION_INFO_V1(age_gds_graph_exists);
PG_FUNCTION_INFO_V1(age_gds_graph_is_stale);

/* --------------------------------------------------- graph_project -------- */

Datum age_gds_graph_project(PG_FUNCTION_ARGS)
{
    char *name_str       = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *age_graph_str  = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *node_query_str = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *edge_query_str = text_to_cstring(PG_GETARG_TEXT_PP(3));
    bool  directed       = PG_GETARG_BOOL(4);
    bool  weighted       = PG_GETARG_BOOL(5);

    /* Expand data_dir */
    const char *raw_dir = GetConfigOption("age_gds.data_dir", false, false);
    char       *data_dir_str = expand_data_dir(raw_dir);

    /* Create directory */
    mkdirp(data_dir_str);

    /* Build file path */
    char file_path[MAXPGPATH];
    snprintf(file_path, MAXPGPATH, "%s/%s.bin", data_dir_str, name_str);

    /* Register per-backend XactCallback once */
    if (!g_xact_cb_registered) {
        RegisterXactCallback(cleanup_bin_xact_cb, nullptr);
        g_xact_cb_registered = true;
    }
    /* Track pending file for abort cleanup */
    strlcpy(g_pending_bin, file_path, MAXPGPATH);

    /* ---- Load graph from AGE via Cypher queries ---- */
    LoadedGraph loaded = age_load_graph(
        age_graph_str, node_query_str, edge_query_str, directed, weighted);

    /* ---- Serialize to .bin file ---- */
    try {
        gds_serialize(file_path, loaded.graph, loaded.id_map, directed, weighted);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_project serialize: %s", ex.what())));
    }

    /* ---- Insert catalog row via SPI ---- */
    SPI_connect();

    int64_t node_count = (int64_t) loaded.graph.numberOfNodes();
    int64_t edge_count = (int64_t) loaded.graph.numberOfEdges();

    Oid   argtypes[9] = {
        TEXTOID, TEXTOID, TEXTOID, TEXTOID,
        INT8OID, INT8OID,
        BOOLOID, BOOLOID,
        TEXTOID
    };
    Datum args[9] = {
        CStringGetTextDatum(name_str),        /* $1  name        */
        CStringGetTextDatum(age_graph_str),   /* $2  age_graph   */
        CStringGetTextDatum(node_query_str),  /* $3  node_query  */
        CStringGetTextDatum(edge_query_str),  /* $4  edge_query  */
        Int64GetDatum(node_count),            /* $5  node_count  */
        Int64GetDatum(edge_count),            /* $6  edge_count  */
        BoolGetDatum(directed),               /* $7  directed    */
        BoolGetDatum(weighted),               /* $8  weighted    */
        CStringGetTextDatum(file_path)        /* $9  file_path   */
    };

    int ret = SPI_execute_with_args(
        "INSERT INTO age_gds.graphs "
        "(name, age_graph, node_query, edge_query, node_count, edge_count, "
        " directed, weighted, file_path) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
        9, argtypes, args, nullptr, false, 0);

    if (ret != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_project catalog insert failed (SPI code %d)", ret)));

    SPI_finish();

    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_drop ----------- */

Datum age_gds_graph_drop(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

    /* ---- Step 1: look up catalog row ---- */
    char  file_path_buf[MAXPGPATH]  = {0};
    char  nodes_path_buf[MAXPGPATH] = {0};
    char  edges_path_buf[MAXPGPATH] = {0};
    bool  is_live                   = false;

    SPI_connect();
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};
        int   ret         = SPI_execute_with_args(
            "SELECT live, file_path, nodes_file_path, edges_file_path "
            "FROM age_gds.graphs WHERE name = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_drop: catalog lookup failed (SPI %d)", ret)));
        }
        if (SPI_processed == 0) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("graph \"%s\" does not exist", name_str)));
        }

        bool      isnull;
        TupleDesc td = SPI_tuptable->tupdesc;
        HeapTuple t  = SPI_tuptable->vals[0];

        Datum d1 = SPI_getbinval(t, td, 1, &isnull);
        if (!isnull) is_live = DatumGetBool(d1);

        Datum d2 = SPI_getbinval(t, td, 2, &isnull);
        if (!isnull) strlcpy(file_path_buf,  TextDatumGetCString(d2),
                             sizeof(file_path_buf));

        Datum d3 = SPI_getbinval(t, td, 3, &isnull);
        if (!isnull) strlcpy(nodes_path_buf, TextDatumGetCString(d3),
                             sizeof(nodes_path_buf));

        Datum d4 = SPI_getbinval(t, td, 4, &isnull);
        if (!isnull) strlcpy(edges_path_buf, TextDatumGetCString(d4),
                             sizeof(edges_path_buf));
    }

    if (is_live) {
        /* ---- Step 2a (live): drop PostgreSQL triggers recorded in index_triggers ---- */
        struct TrigInfo { std::string tname, tschema, ttable; };
        std::vector<TrigInfo> trigs;

        {
            Oid   argtypes[1] = {TEXTOID};
            Datum args[1]     = {CStringGetTextDatum(name_str)};
            int   ret         = SPI_execute_with_args(
                "SELECT trigger_name, table_schema, table_name "
                "FROM age_gds.index_triggers WHERE graph_name = $1",
                1, argtypes, args, nullptr, true, 0);

            if (ret == SPI_OK_SELECT) {
                for (uint64_t i = 0; i < SPI_processed; i++) {
                    bool      isnull;
                    TupleDesc td = SPI_tuptable->tupdesc;
                    HeapTuple t  = SPI_tuptable->vals[i];

                    auto gtext = [&](int col) -> std::string {
                        Datum d = SPI_getbinval(t, td, col, &isnull);
                        return isnull ? std::string{} : std::string(TextDatumGetCString(d));
                    };
                    trigs.push_back({gtext(1), gtext(2), gtext(3)});
                }
            }
        }

        for (auto &tr : trigs) {
            if (tr.tname.empty()) continue;
            /* Escape identifiers */
            char sql[512];
            snprintf(sql, sizeof(sql),
                "DROP TRIGGER IF EXISTS \"%s\" ON \"%s\".\"%s\"",
                tr.tname.c_str(), tr.tschema.c_str(), tr.ttable.c_str());
            SPI_execute(sql, false, 0);
            /* Ignore errors (table might have been dropped already) */
        }
    }

    /* ---- Step 3: delete catalog row (ON DELETE CASCADE handles index_triggers) ---- */
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};
        int   ret         = SPI_execute_with_args(
            "DELETE FROM age_gds.graphs WHERE name = $1",
            1, argtypes, args, nullptr, false, 0);

        if (ret != SPI_OK_DELETE) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_drop: catalog delete failed (SPI %d)", ret)));
        }
    }
    SPI_finish();

    /* ---- Step 4: remove files ---- */
    if (is_live) {
        if (nodes_path_buf[0] != '\0') (void) unlink(nodes_path_buf);
        if (edges_path_buf[0] != '\0') (void) unlink(edges_path_buf);
    } else {
        if (file_path_buf[0] != '\0') (void) unlink(file_path_buf);
    }

    /* ---- Step 5: evict from session cache ---- */
    session_cache_invalidate(name_str);

    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_list ----------- */

Datum age_gds_graph_list(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    InitMaterializedSRF(fcinfo,
                        MAT_SRF_USE_EXPECTED_DESC | MAT_SRF_BLESS);

    Tuplestorestate *tupstore = rsinfo->setResult;
    TupleDesc        tupdesc  = rsinfo->setDesc;

    SPI_connect();

    /* Columns: name(1), age_graph(2), node_count(3), edge_count(4),
     *          directed(5), weighted(6), file_path(7), created_at(8), live(9) */
    int ret = SPI_execute(
        "SELECT name, age_graph, node_count, edge_count, "
        "       directed, weighted, file_path, created_at, live "
        "FROM age_gds.graphs "
        "ORDER BY created_at",
        true, 0);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_list query failed (SPI code %d)", ret)));

    for (uint64_t i = 0; i < SPI_processed; i++) {
        HeapTuple spi_tup  = SPI_tuptable->vals[i];
        TupleDesc spi_desc = SPI_tuptable->tupdesc;

        /* Return TABLE has 10 columns:
         *  1-8  from SPI (name..created_at)
         *  9    computed memoryUsage
         * 10    live (SPI col 9) */
        Datum values[10];
        bool  isnull[10];
        bool  is_null;

        /* Copy first 8 SPI columns */
        for (int j = 0; j < 8; j++) {
            values[j] = SPI_getbinval(spi_tup, spi_desc, j + 1, &is_null);
            isnull[j] = is_null;
        }

        /* memoryUsage = node_count * 8 + edge_count * 20
         * SPI cols (1-indexed): 3=node_count, 4=edge_count
         * values[] (0-indexed): values[2]=node_count, values[3]=edge_count */
        int64_t nc = isnull[2] ? 0 : DatumGetInt64(values[2]);
        int64_t ec = isnull[3] ? 0 : DatumGetInt64(values[3]);
        values[8] = Int64GetDatum(nc * 8 + ec * 20);
        isnull[8] = false;

        /* live = SPI col 9 */
        values[9] = SPI_getbinval(spi_tup, spi_desc, 9, &is_null);
        isnull[9] = is_null;

        tuplestore_putvalues(tupstore, tupdesc, values, isnull);
    }

    SPI_finish();

    return (Datum) 0;
}

/* --------------------------------------------------- graph_exists --------- */

Datum age_gds_graph_exists(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

    SPI_connect();

    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(name_str)};

    int ret = SPI_execute_with_args(
        "SELECT 1 FROM age_gds.graphs WHERE name = $1",
        1, argtypes, args, nullptr, true, 1);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_exists query failed (SPI code %d)", ret)));

    bool found = (SPI_processed > 0);

    SPI_finish();

    PG_RETURN_BOOL(found);
}

/* --------------------------------------------------- graph_is_stale ------- */

Datum age_gds_graph_is_stale(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char  age_graph_buf[256] = {0};
    bool  is_live            = false;

    /* === SPI session 1: catalog lookup === */
    SPI_connect();

    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};

        int ret = SPI_execute_with_args(
            "SELECT age_graph, live FROM age_gds.graphs WHERE name = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_is_stale: catalog query failed (SPI %d)", ret)));
        }

        if (SPI_processed == 0) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("graph \"%s\" does not exist", name_str)));
        }

        bool      isnull;
        TupleDesc td = SPI_tuptable->tupdesc;
        HeapTuple t  = SPI_tuptable->vals[0];

        Datum d1 = SPI_getbinval(t, td, 1, &isnull);
        if (!isnull) strlcpy(age_graph_buf, TextDatumGetCString(d1), sizeof(age_graph_buf));

        Datum d2 = SPI_getbinval(t, td, 2, &isnull);
        if (!isnull) is_live = DatumGetBool(d2);
    }

    SPI_finish();

    /* Live indices are never stale */
    if (is_live)
        PG_RETURN_BOOL(false);

    /* === SPI session 2: staleness check via pg_stat_user_tables === */
    bool stale = false;

    SPI_connect();

    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(age_graph_buf)};

        int ret = SPI_execute_with_args(
            "SELECT coalesce(n_mod_since_analyze, 0) > 0 "
            "FROM pg_stat_user_tables "
            "WHERE schemaname = $1 "
            "  AND relname = '_ag_label_vertex' "
            "LIMIT 1",
            1, argtypes, args, nullptr, true, 1);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            bool  isnull;
            Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                     SPI_tuptable->tupdesc, 1, &isnull);
            if (!isnull)
                stale = DatumGetBool(d);
        }
    }

    SPI_finish();

    PG_RETURN_BOOL(stale);
}

} // extern "C"
