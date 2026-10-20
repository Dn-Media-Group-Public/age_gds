// src/catalog/graph_project.cpp

// NetworKit first — its Log.hpp defines `#define ERROR(...)` as a variadic
// function-like macro.  We #undef it below after including PG headers.
#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/serialization.h"
#include "catalog/session_cache.h"
#include "catalog/index_storage.h"
#include "bridge/age_loader.h"
#include "util/cypher_agtype.h"

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
static char g_pending_bin[MAXPGPATH]   = {0};
static char g_pending_nodes[MAXPGPATH] = {0};
static char g_pending_edges[MAXPGPATH] = {0};
static bool g_xact_cb_registered      = false;

static void cleanup_bin_xact_cb(XactEvent event, void * /*arg*/)
{
    if (g_pending_bin[0] != '\0') {
        if (event == XACT_EVENT_ABORT)
            (void) unlink(g_pending_bin);
        g_pending_bin[0] = '\0';
    }
    if (g_pending_nodes[0] != '\0' || g_pending_edges[0] != '\0') {
        if (event == XACT_EVENT_ABORT) {
            if (g_pending_nodes[0] != '\0') (void) unlink(g_pending_nodes);
            if (g_pending_edges[0] != '\0') (void) unlink(g_pending_edges);
        }
        g_pending_nodes[0] = '\0';
        g_pending_edges[0] = '\0';
    }
}

/* Shared core: both age_gds_graph_project (SQL) and
 * age_gds_graph_project_cypher call this directly after resolving their own
 * argument shapes (no SPI round-trip between the two wrappers). */
static void do_graph_project(
    const char *name_str, const char *age_graph_str,
    const char *node_query_str, const char *edge_query_str,
    bool directed, bool weighted, bool incremental)
{
    /* Expand data_dir */
    const char *raw_dir = GetConfigOption("age_gds.data_dir", false, false);
    char       *data_dir_str = expand_data_dir(raw_dir);

    /* Create directory */
    mkdirp(data_dir_str);

    /* Register per-backend XactCallback once */
    if (!g_xact_cb_registered) {
        RegisterXactCallback(cleanup_bin_xact_cb, nullptr);
        g_xact_cb_registered = true;
    }

    /* Both queries NULL = whole graph (SQL-loaded); exactly one is an error. */
    if ((node_query_str == nullptr) != (edge_query_str == nullptr))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph_project: node_query and edge_query must both be "
                        "given or both omitted (omitted = the whole graph)")));

    /* ---- Load graph from AGE (Cypher queries, or SQL for the whole graph) ---- */
    LoadedGraph loaded = age_load_graph(
        age_graph_str, node_query_str, edge_query_str, directed, weighted);

    int64_t node_count = (int64_t) loaded.graph.numberOfNodes();
    int64_t edge_count = (int64_t) loaded.graph.numberOfEdges();

    char file_path[MAXPGPATH]  = {0};
    char nodes_path[MAXPGPATH] = {0};
    char edges_path[MAXPGPATH] = {0};

    if (incremental) {
        snprintf(nodes_path, MAXPGPATH, "%s/%s.nodes", data_dir_str, name_str);
        snprintf(edges_path, MAXPGPATH, "%s/%s.edges", data_dir_str, name_str);
        strlcpy(g_pending_nodes, nodes_path, MAXPGPATH);
        strlcpy(g_pending_edges, edges_path, MAXPGPATH);

        try {
            index_storage_write_initial(nodes_path, edges_path,
                                        loaded.graph, loaded.id_map,
                                        directed, weighted);
        } catch (const std::exception &ex) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_project write_initial: %s", ex.what())));
        }
    } else {
        snprintf(file_path, MAXPGPATH, "%s/%s.bin", data_dir_str, name_str);
        strlcpy(g_pending_bin, file_path, MAXPGPATH);

        try {
            gds_serialize(file_path, loaded.graph, loaded.id_map,
                         directed, weighted, /* content_version */ 0);
        } catch (const std::exception &ex) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_project serialize: %s", ex.what())));
        }
    }

    /* ---- Insert catalog row via SPI ---- */
    SPI_connect();

    Oid   argtypes[12] = {
        TEXTOID, TEXTOID, TEXTOID, TEXTOID,
        INT8OID, INT8OID,
        BOOLOID, BOOLOID,
        TEXTOID, TEXTOID, TEXTOID,
        BOOLOID
    };
    Datum args[12] = {
        CStringGetTextDatum(name_str),        /* $1  name        */
        CStringGetTextDatum(age_graph_str),   /* $2  age_graph   */
        node_query_str ? CStringGetTextDatum(node_query_str) : (Datum) 0,  /* $3  node_query  */
        edge_query_str ? CStringGetTextDatum(edge_query_str) : (Datum) 0,  /* $4  edge_query  */
        Int64GetDatum(node_count),            /* $5  node_count  */
        Int64GetDatum(edge_count),            /* $6  edge_count  */
        BoolGetDatum(directed),               /* $7  directed    */
        BoolGetDatum(weighted),               /* $8  weighted    */
        CStringGetTextDatum(file_path),       /* $9  file_path   */
        CStringGetTextDatum(nodes_path),      /* $10 nodes_file_path */
        CStringGetTextDatum(edges_path),      /* $11 edges_file_path */
        BoolGetDatum(incremental)             /* $12 incremental */
    };
    char  nulls[13] = {' ',' ',(char) (node_query_str ? ' ' : 'n'),
                       (char) (edge_query_str ? ' ' : 'n'),' ',' ',' ',' ',
                       (char) (file_path[0]  == '\0' ? 'n' : ' '),
                       (char) (nodes_path[0] == '\0' ? 'n' : ' '),
                       (char) (edges_path[0] == '\0' ? 'n' : ' '),
                       ' ',
                       '\0'};

    int ret = SPI_execute_with_args(
        "INSERT INTO age_gds.graphs "
        "(name, age_graph, node_query, edge_query, node_count, edge_count, "
        " directed, weighted, file_path, nodes_file_path, edges_file_path, incremental) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
        12, argtypes, args, nulls, false, 0);

    if (ret != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_project catalog insert failed (SPI code %d)", ret)));

    SPI_finish();
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
    char *node_query_str = PG_ARGISNULL(2) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *edge_query_str = PG_ARGISNULL(3) ? nullptr : text_to_cstring(PG_GETARG_TEXT_PP(3));
    bool  directed       = PG_GETARG_BOOL(4);
    bool  weighted       = PG_GETARG_BOOL(5);
    bool  incremental    = PG_GETARG_BOOL(6);

    do_graph_project(name_str, age_graph_str, node_query_str, edge_query_str,
                     directed, weighted, incremental);
    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_project (Cypher) */
/* age_gds.graph_project(graphName agtype, nodeProjection agtype,
 * relationshipProjection agtype, config agtype DEFAULT NULL) RETURNS agtype.
 * Matches Neo4j's real gds.graph.project(graphName, nodeProjection,
 * relationshipProjection, configuration) shape (not the (graphName, config)
 * 2-arg convention every other wrapper uses -- there is no "graph to select"
 * here, graphName IS the new projection's name).
 *
 * nodeProjection/relationshipProjection: '*' (wildcard) or a map of
 * label/type -> projection-config map. Only an empty {} projection-config
 * is supported per label/type; Neo4j's "properties" key is refused as
 * unsupported (see plan's projection-translation section) rather than
 * silently ignored. config = {ageGraph (required), directed, weighted,
 * incremental}. */

static std::string translate_projection(Jsonb *proj, bool is_node, const char *argname)
{
    if (jsonb_is_string_eq(proj, "*"))
        return is_node ? "MATCH (n) RETURN id(n) AS id"
                       : "MATCH (a)-[r]->(b) RETURN id(a) AS src, id(b) AS dst, r.weight AS weight";

    if (!proj || !JB_ROOT_IS_OBJECT(proj))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s must be '*' or a map of label/type -> {}", argname)));

    std::vector<std::string> labels;
    JsonbIterator *it = JsonbIteratorInit(&proj->root);
    JsonbValue key, val;
    JsonbIteratorToken tok;
    while ((tok = JsonbIteratorNext(&it, &key, false)) != WJB_DONE) {
        if (tok != WJB_KEY) continue;
        std::string label(key.val.string.val, key.val.string.len);
        JsonbIteratorToken vtok = JsonbIteratorNext(&it, &val, true);
        if (vtok == WJB_VALUE && val.type == jbvBinary && JsonContainerIsObject(val.val.binary.data)) {
            JsonbValue found;
            JsonbValue propkey;
            propkey.type = jbvString;
            propkey.val.string.val = (char *)"properties";
            propkey.val.string.len = 10;
            if (findJsonbValueFromContainer(val.val.binary.data, JB_FOBJECT, &propkey))
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("%s: 'properties' is not supported for label/type \"%s\"",
                                       argname, label.c_str())));
        }
        labels.push_back(label);
    }
    if (labels.empty())
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s must name at least one label/type", argname)));

    std::string query;
    for (size_t i = 0; i < labels.size(); i++) {
        if (i > 0) query += " UNION ";
        if (is_node)
            query += "MATCH (n:`" + labels[i] + "`) RETURN id(n) AS id";
        else
            query += "MATCH (a)-[r:`" + labels[i] + "`]->(b) "
                     "RETURN id(a) AS src, id(b) AS dst, r.weight AS weight";
    }
    return query;
}

PG_FUNCTION_INFO_V1(age_gds_graph_project_cypher);

Datum age_gds_graph_project_cypher(PG_FUNCTION_ARGS)
{
    if (agtype_arg_is_null(fcinfo, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_project: graphName is required")));
    std::string name = get_agtype_arg_string(fcinfo, 0, "graphName");

    Jsonb *node_proj = agtype_arg_to_jsonb_any(fcinfo, 1);
    Jsonb *rel_proj  = agtype_arg_to_jsonb_any(fcinfo, 2);
    if (!node_proj != !rel_proj)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_project: nodeProjection and relationshipProjection "
                               "must both be given or both NULL (NULL = the whole graph)")));

    CypherConfig cfg = parse_cypher_config(fcinfo, 3);
    cfg.validate_keys({"ageGraph", "directed", "weighted", "incremental"});
    std::string age_graph;
    if (!cfg.get_string("ageGraph", age_graph))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_project: config.ageGraph is required")));
    bool directed = false, weighted = false, incremental = false;
    cfg.get_bool("directed", directed);
    cfg.get_bool("weighted", weighted);
    cfg.get_bool("incremental", incremental);

    std::string node_query, edge_query;
    if (node_proj) {
        node_query = translate_projection(node_proj, true, "nodeProjection");
        edge_query = translate_projection(rel_proj, false, "relationshipProjection");
    }

    do_graph_project(name.c_str(), age_graph.c_str(),
                     node_proj ? node_query.c_str() : nullptr,
                     node_proj ? edge_query.c_str() : nullptr,
                     directed, weighted, incremental);

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("graphName"); b.add_string(name);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
}

/* --------------------------------------------------- graph_drop ----------- */

static void do_graph_drop(const char *name_str)
{
    /* ---- Step 1: look up catalog row ---- */
    char  file_path_buf[MAXPGPATH]  = {0};
    char  nodes_path_buf[MAXPGPATH] = {0};
    char  edges_path_buf[MAXPGPATH] = {0};
    bool  is_live                   = false;
    bool  is_incremental            = false;

    SPI_connect();
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};
        int   ret         = SPI_execute_with_args(
            "SELECT live, file_path, nodes_file_path, edges_file_path, incremental "
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

        Datum d5 = SPI_getbinval(t, td, 5, &isnull);
        if (!isnull) is_incremental = DatumGetBool(d5);
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
    if (is_incremental) {
        if (nodes_path_buf[0] != '\0') (void) unlink(nodes_path_buf);
        if (edges_path_buf[0] != '\0') (void) unlink(edges_path_buf);
    } else {
        if (file_path_buf[0] != '\0') (void) unlink(file_path_buf);
    }

    /* ---- Step 5: evict from session cache ---- */
    session_cache_invalidate(name_str);
}

Datum age_gds_graph_drop(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    do_graph_drop(name_str);
    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_drop (Cypher) - */

PG_FUNCTION_INFO_V1(age_gds_graph_drop_cypher);

Datum age_gds_graph_drop_cypher(PG_FUNCTION_ARGS)
{
    if (agtype_arg_is_null(fcinfo, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_drop: graphName is required")));
    std::string name = get_agtype_arg_string(fcinfo, 0, "graphName");
    do_graph_drop(name.c_str());

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("graphName"); b.add_string(name);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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
     *          directed(5), weighted(6), file_path(7), created_at(8),
     *          live(9), incremental(10) */
    int ret = SPI_execute(
        "SELECT name, age_graph, node_count, edge_count, "
        "       directed, weighted, file_path, created_at, live, incremental "
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
         * 10    live (SPI col 9)
         * 11    incremental (SPI col 10) */
        Datum values[11];
        bool  isnull[11];
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

        /* incremental = SPI col 10 */
        values[10] = SPI_getbinval(spi_tup, spi_desc, 10, &is_null);
        isnull[10] = is_null;

        tuplestore_putvalues(tupstore, tupdesc, values, isnull);
    }

    SPI_finish();

    return (Datum) 0;
}

/* --------------------------------------------------- graph_list (Cypher) - */
/* age_gds.graph_list() takes no arguments in SQL; the Cypher-callable
 * overload keeps the same (no-arg) shape -- there is no graph to select and
 * no per-algorithm config. Array of maps, one per catalog row. */

PG_FUNCTION_INFO_V1(age_gds_graph_list_cypher);

Datum age_gds_graph_list_cypher(PG_FUNCTION_ARGS)
{
    SPI_connect();

    int ret = SPI_execute(
        "SELECT name, age_graph, node_count, edge_count, "
        "       directed, weighted, file_path, created_at, live, incremental "
        "FROM age_gds.graphs "
        "ORDER BY created_at",
        true, 0);

    if (ret != SPI_OK_SELECT) {
        SPI_finish();
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("graph_list query failed (SPI code %d)", ret)));
    }

    CypherResultBuilder b(true);
    for (uint64_t i = 0; i < SPI_processed; i++) {
        HeapTuple spi_tup  = SPI_tuptable->vals[i];
        TupleDesc spi_desc = SPI_tuptable->tupdesc;
        bool isnull;

        auto get_text = [&](int col) -> std::string {
            Datum d = SPI_getbinval(spi_tup, spi_desc, col, &isnull);
            return isnull ? std::string{} : std::string(TextDatumGetCString(d));
        };
        Datum name_d       = SPI_getbinval(spi_tup, spi_desc, 1, &isnull);
        std::string name   = isnull ? "" : TextDatumGetCString(name_d);
        std::string age_g  = get_text(2);
        Datum nc_d = SPI_getbinval(spi_tup, spi_desc, 3, &isnull);
        int64_t nc = isnull ? 0 : DatumGetInt64(nc_d);
        Datum ec_d = SPI_getbinval(spi_tup, spi_desc, 4, &isnull);
        int64_t ec = isnull ? 0 : DatumGetInt64(ec_d);
        Datum dir_d = SPI_getbinval(spi_tup, spi_desc, 5, &isnull);
        bool directed = !isnull && DatumGetBool(dir_d);
        Datum wt_d = SPI_getbinval(spi_tup, spi_desc, 6, &isnull);
        bool weighted = !isnull && DatumGetBool(wt_d);
        std::string file_path = get_text(7);
        Datum ts_d = SPI_getbinval(spi_tup, spi_desc, 8, &isnull);
        std::string created_at = isnull ? "" :
            DatumGetCString(DirectFunctionCall1(timestamptz_out, ts_d));
        Datum live_d = SPI_getbinval(spi_tup, spi_desc, 9, &isnull);
        bool live = !isnull && DatumGetBool(live_d);
        Datum inc_d = SPI_getbinval(spi_tup, spi_desc, 10, &isnull);
        bool incremental = !isnull && DatumGetBool(inc_d);

        b.begin_row();
        b.key("name"); b.add_string(name);
        b.key("age_graph"); b.add_string(age_g);
        b.key("node_count"); b.add_int64(nc);
        b.key("edge_count"); b.add_int64(ec);
        b.key("directed"); b.add_bool(directed);
        b.key("weighted"); b.add_bool(weighted);
        b.key("file_path"); if (file_path.empty()) b.add_null(); else b.add_string(file_path);
        b.key("created_at"); if (created_at.empty()) b.add_null(); else b.add_string(created_at);
        b.key("memoryUsage"); b.add_int64(nc * 8 + ec * 20);
        b.key("live"); b.add_bool(live);
        b.key("incremental"); b.add_bool(incremental);
        b.end_row();
    }

    SPI_finish();
    PG_RETURN_DATUM(b.finish());
}

/* --------------------------------------------------- graph_exists --------- */

static bool do_graph_exists(const char *name_str)
{
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
    return found;
}

Datum age_gds_graph_exists(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(do_graph_exists(name_str));
}

/* --------------------------------------------------- graph_exists (Cypher) */

PG_FUNCTION_INFO_V1(age_gds_graph_exists_cypher);

Datum age_gds_graph_exists_cypher(PG_FUNCTION_ARGS)
{
    if (agtype_arg_is_null(fcinfo, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_exists: graphName is required")));
    std::string name = get_agtype_arg_string(fcinfo, 0, "graphName");
    return bool_to_agtype_scalar(do_graph_exists(name.c_str()));
}

/* --------------------------------------------------- graph_is_stale ------- */

static bool do_graph_is_stale(const char *name_str)
{
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
        return false;

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

    return stale;
}

Datum age_gds_graph_is_stale(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    PG_RETURN_BOOL(do_graph_is_stale(name_str));
}

/* --------------------------------------------------- graph_is_stale (Cypher) */

PG_FUNCTION_INFO_V1(age_gds_graph_is_stale_cypher);

Datum age_gds_graph_is_stale_cypher(PG_FUNCTION_ARGS)
{
    if (agtype_arg_is_null(fcinfo, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_is_stale: graphName is required")));
    std::string name = get_agtype_arg_string(fcinfo, 0, "graphName");
    return bool_to_agtype_scalar(do_graph_is_stale(name.c_str()));
}

} // extern "C"
