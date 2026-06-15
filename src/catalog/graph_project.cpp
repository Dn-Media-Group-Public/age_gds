// src/catalog/graph_project.cpp

// NetworKit first — its Log.hpp defines `#define ERROR(...)` as a variadic
// function-like macro.  We #undef it below after including PG headers.
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/serialization.h"
#include "bridge/age_loader.h"

#include <string>
#include <stdexcept>

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

    SPI_connect();

    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(name_str)};

    int ret = SPI_execute_with_args(
        "DELETE FROM age_gds.graphs WHERE name = $1 RETURNING file_path",
        1, argtypes, args, nullptr, false, 0);

    if (ret != SPI_OK_DELETE_RETURNING)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_drop failed (SPI code %d)", ret)));

    if (SPI_processed == 0) {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("graph \"%s\" does not exist", name_str)));
    }

    /* Extract file_path before SPI_finish invalidates memory */
    bool  isnull;
    Datum fp_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                   SPI_tuptable->tupdesc, 1, &isnull);
    char  file_path[MAXPGPATH] = {0};
    if (!isnull)
        strlcpy(file_path, TextDatumGetCString(fp_datum), MAXPGPATH);

    SPI_finish();

    if (file_path[0] != '\0')
        (void) unlink(file_path);

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

    int ret = SPI_execute(
        "SELECT name, age_graph, node_count, edge_count, "
        "       directed, weighted, file_path, created_at "
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

        Datum values[8];
        bool  isnull[8];
        bool  is_null;

        for (int j = 0; j < 8; j++) {
            values[j] = SPI_getbinval(spi_tup, spi_desc, j + 1, &is_null);
            isnull[j] = is_null;
        }

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

} // extern "C"
