// src/catalog/graph_project.cpp

// NetworKit first — its Log.hpp defines `#define ERROR(...)` as a variadic
// function-like macro.  We #undef it below after including PG headers.
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/serialization.h"
#include "bridge/node_id_map.h"

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
    char *node_query_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *edge_query_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

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

    /* ---- Stub loader: 3 nodes, 3 edges, unweighted, undirected ---- */
    try {
        NetworKit::Graph g(3, false, false);
        g.addEdge(0, 1);
        g.addEdge(1, 2);
        g.addEdge(0, 2);

        NodeIdMap id_map;
        id_map.intern(1); /* nk::node 0 -> graphid 1 */
        id_map.intern(2); /* nk::node 1 -> graphid 2 */
        id_map.intern(3); /* nk::node 2 -> graphid 3 */

        gds_serialize(file_path, g, id_map, false, false);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_project serialize: %s", ex.what())));
    }

    /* ---- Insert catalog row via SPI ---- */
    SPI_connect();

    int64_t node_count = 3;
    int64_t edge_count = 3;

    Oid    argtypes[6] = {TEXTOID, TEXTOID, TEXTOID, INT8OID, INT8OID, TEXTOID};
    Datum  args[6]     = {
        CStringGetTextDatum(name_str),
        CStringGetTextDatum(node_query_str),
        CStringGetTextDatum(edge_query_str),
        Int64GetDatum(node_count),
        Int64GetDatum(edge_count),
        CStringGetTextDatum(file_path)
    };

    int ret = SPI_execute_with_args(
        "INSERT INTO age_gds.graphs "
        "(name, age_graph, node_query, edge_query, node_count, edge_count, "
        " directed, weighted, file_path) "
        "VALUES ($1, '', $2, $3, $4, $5, false, false, $6)",
        6, argtypes, args, nullptr, false, 0);

    if (ret != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_project catalog insert failed (SPI code %d)", ret)));

    SPI_finish();

    /* Catalog row committed → no cleanup needed on success */
    /* (g_pending_bin cleared by xact callback on commit) */

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
