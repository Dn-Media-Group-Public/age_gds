// src/catalog/graph_index.cpp
//
// Live graph index: creation, trigger functions, on_new_label handler,
// and graph_reindex (both live compaction and snapshot rebuild).
//
// Include order: NetworKit first, then PG headers, then #undef ERROR.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "catalog/index_storage.h"
#include "catalog/serialization.h"
#include "catalog/session_cache.h"
#include "bridge/age_loader.h"

#include <string>
#include <cstring>
#include <cerrno>
#include <stdexcept>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "access/xact.h"
#include "utils/elog.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ============================================================ helpers ===== */

/*
 * Acquire a transaction-level exclusive advisory lock keyed by index name.
 * Released automatically when the current transaction commits or aborts.
 * Uses pg_advisory_xact_lock_int8() so no shared memory registration is needed.
 */
static void acquire_index_lock(const char *name)
{
    /* FNV-1a 64-bit hash of the name as advisory lock key */
    uint64_t h = 0x14057B7EF767814FULL;
    for (const char *p = name; *p; p++)
        h = (h ^ (unsigned char)*p) * 0x00000100000001B3ULL;
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum((int64_t) h));
}

/* Expand $PGDATA prefix (same helper as graph_project.cpp). */
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

/* mkdir -p */
static void mkdirp(const char *path)
{
    char   tmp[MAXPGPATH];
    size_t len = strlen(path);
    if (len >= MAXPGPATH)
        ereport(ERROR, (errmsg("data_dir path too long")));
    memcpy(tmp, path, len + 1);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
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

/*
 * Escape an identifier for use inside a SQL string (single-quote escape).
 * The result is palloc'd.
 */
static char *escape_sql_ident(const char *s)
{
    size_t len = strlen(s);
    char  *out = (char *) palloc(len * 2 + 3); /* worst case */
    char  *p   = out;
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') *p++ = '"'; /* double the quote */
        *p++ = s[i];
    }
    *p++ = '"';
    *p   = '\0';
    return out;
}

/*
 * Build a trigger name that is unique to (index_name, label_name) and fits
 * within PostgreSQL's 63-byte identifier limit.
 * Format: age_gds_idx_<v|e>_<8-hex-hash>
 */
static void build_trigger_name(char *out, size_t outsz,
                                char kind,
                                const char *index_name,
                                const char *label_name)
{
    unsigned int h = 0x12345678u;
    for (const char *p = index_name; *p; p++) h = h * 37u + (unsigned char)*p;
    h = h * 1000003u + 0xF1u;
    for (const char *p = label_name; *p; p++) h = h * 37u + (unsigned char)*p;
    snprintf(out, outsz, "age_gds_idx_%c_%08x", kind, h);
}

/*
 * Install a single data trigger on a label table and record it in the
 * age_gds.index_triggers catalog.  Runs inside an open SPI session.
 * kind: 'v' = vertex, 'e' = edge.
 */
static void install_label_trigger(
    const char *index_name,
    const char *age_graph,
    const char *label_name,
    char        kind)
{
    char trigger_name[64];
    build_trigger_name(trigger_name, sizeof(trigger_name),
                       kind, index_name, label_name);

    const char *fn = (kind == 'v')
        ? "age_gds.index_vertex_trigger"
        : "age_gds.index_edge_trigger";

    char *schema_q = escape_sql_ident(age_graph);
    char *label_q  = escape_sql_ident(label_name);
    char *trig_q   = escape_sql_ident(trigger_name);

    /* CREATE TRIGGER ... EXECUTE FUNCTION fn(index_name) */
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "CREATE TRIGGER %s "
        "AFTER INSERT OR UPDATE OR DELETE "
        "ON %s.%s "
        "FOR EACH ROW "
        "EXECUTE FUNCTION %s('%s')",
        trig_q, schema_q, label_q, fn, index_name);

    int ret = SPI_execute(sql, false, 0);
    if (ret != SPI_OK_UTILITY)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_index: CREATE TRIGGER failed (SPI %d) sql: %s",
                        ret, sql)));

    /* Record in catalog */
    Oid   art[4] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID};
    Datum arv[4] = {
        CStringGetTextDatum(index_name),
        CStringGetTextDatum(trigger_name),
        CStringGetTextDatum(age_graph),
        CStringGetTextDatum(label_name)
    };
    ret = SPI_execute_with_args(
        "INSERT INTO age_gds.index_triggers "
        "(graph_name, trigger_name, table_schema, table_name) "
        "VALUES ($1, $2, $3, $4) "
        "ON CONFLICT DO NOTHING",
        4, art, arv, nullptr, false, 0);
    if (ret != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_index: index_triggers insert failed (SPI %d)", ret)));
}

/* ============================================================ C functions == */

extern "C" {

/* Already declared in age_gds.cpp via PG_FUNCTION_INFO_V1 */

/* --------------------------------------------------- graph_index ---------- */

Datum age_gds_graph_index(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph_index: name and age_graph must not be NULL")));

    char *name_str      = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *age_graph_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool  directed      = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
    bool  weighted      = PG_ARGISNULL(3) ? false : PG_GETARG_BOOL(3);

    /* Build file paths */
    const char *raw_dir     = GetConfigOption("age_gds.data_dir", false, false);
    char       *data_dir_str = expand_data_dir(raw_dir);
    mkdirp(data_dir_str);

    char nodes_path[MAXPGPATH];
    char edges_path[MAXPGPATH];
    snprintf(nodes_path, MAXPGPATH, "%s/%s.nodes", data_dir_str, name_str);
    snprintf(edges_path, MAXPGPATH, "%s/%s.edges", data_dir_str, name_str);

    /* ---- Initial whole-graph load ---- */
    const char *node_q = "MATCH (n) RETURN id(n) AS id";
    const char *edge_q = "MATCH (a)-[r]->(b) "
                         "RETURN id(a) AS src, id(b) AS dst, r.weight AS weight";

    LoadedGraph loaded = age_load_graph(age_graph_str, node_q, edge_q,
                                        directed, weighted);

    /* ---- Write index files ---- */
    try {
        index_storage_write_initial(nodes_path, edges_path,
                                    loaded.graph, loaded.id_map,
                                    directed, weighted);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_index: write initial files failed: %s", ex.what())));
    }

    int64_t node_count = (int64_t) loaded.graph.numberOfNodes();
    int64_t edge_count = (int64_t) loaded.graph.numberOfEdges();

    /* ---- Discover labels and install triggers ---- */
    SPI_connect();

    /* Insert catalog row */
    {
        Oid   argtypes[8] = {TEXTOID, TEXTOID, INT8OID, INT8OID,
                              BOOLOID, BOOLOID, TEXTOID, TEXTOID};
        Datum args[8]     = {
            CStringGetTextDatum(name_str),
            CStringGetTextDatum(age_graph_str),
            Int64GetDatum(node_count),
            Int64GetDatum(edge_count),
            BoolGetDatum(directed),
            BoolGetDatum(weighted),
            CStringGetTextDatum(nodes_path),
            CStringGetTextDatum(edges_path)
        };
        int ret = SPI_execute_with_args(
            "INSERT INTO age_gds.graphs "
            "(name, age_graph, node_count, edge_count, directed, weighted, "
            " live, nodes_file_path, edges_file_path) "
            "VALUES ($1, $2, $3, $4, $5, $6, true, $7, $8)",
            8, argtypes, args, nullptr, false, 0);
        if (ret != SPI_OK_INSERT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_index: catalog insert failed (SPI %d)", ret)));
    }

    /* Query ag_label for all labels on this graph */
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(age_graph_str)};

        int ret = SPI_execute_with_args(
            "SELECT al.name, al.kind "
            "FROM ag_catalog.ag_label al "
            "JOIN ag_catalog.ag_graph ag ON al.graph = ag.graphid "
            "WHERE ag.name = $1",
            1, argtypes, args, nullptr, true, 0);

        if (ret != SPI_OK_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_index: label query failed (SPI %d)", ret)));

        uint64_t nlabels = SPI_processed;
        /* Copy label data before further SPI calls in install_label_trigger */
        struct LabelInfo { std::string name; char kind; };
        std::vector<LabelInfo> labels;
        labels.reserve(nlabels);
        for (uint64_t i = 0; i < nlabels; i++) {
            bool  isnull;
            Datum dn = SPI_getbinval(SPI_tuptable->vals[i],
                                      SPI_tuptable->tupdesc, 1, &isnull);
            Datum dk = SPI_getbinval(SPI_tuptable->vals[i],
                                      SPI_tuptable->tupdesc, 2, &isnull);
            if (isnull) continue;
            char *lname = pstrdup(NameStr(*DatumGetName(dn)));
            char  lkind = DatumGetChar(dk);
            /* Skip the internal _ag_label_vertex / _ag_label_edge meta-labels */
            if (strcmp(lname, "_ag_label_vertex") == 0) continue;
            if (strcmp(lname, "_ag_label_edge")   == 0) continue;
            labels.push_back({std::string(lname), lkind});
        }

        for (auto &li : labels) {
            install_label_trigger(name_str, age_graph_str,
                                  li.name.c_str(), li.kind);
        }
    }

    SPI_finish();

    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_reindex -------- */

Datum age_gds_graph_reindex(PG_FUNCTION_ARGS)
{
    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));

    /* Look up catalog */
    char  nodes_path[MAXPGPATH] = {0};
    char  edges_path[MAXPGPATH] = {0};
    char  file_path[MAXPGPATH]  = {0};
    char  age_graph[256]        = {0};
    char  node_query[4096]      = {0};
    char  edge_query[4096]      = {0};
    bool  is_live               = false;
    bool  is_directed           = false;
    bool  is_weighted           = false;

    SPI_connect();
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};
        int   ret         = SPI_execute_with_args(
            "SELECT live, nodes_file_path, edges_file_path, "
            "       file_path, age_graph, node_query, edge_query, "
            "       directed, weighted "
            "FROM age_gds.graphs WHERE name = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: catalog query failed (SPI %d)", ret)));
        }
        if (SPI_processed == 0) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("graph \"%s\" does not exist", name_str)));
        }

        bool isnull;
        TupleDesc td = SPI_tuptable->tupdesc;
        HeapTuple t  = SPI_tuptable->vals[0];

#define GET_BOOL(col)   DatumGetBool(SPI_getbinval(t, td, col, &isnull))
#define GET_TEXT(col, buf) \
        do { Datum _d = SPI_getbinval(t, td, col, &isnull); \
             if (!isnull) strlcpy(buf, TextDatumGetCString(_d), sizeof(buf)); } while(0)

        is_live    = GET_BOOL(1);
        GET_TEXT(2, nodes_path);
        GET_TEXT(3, edges_path);
        GET_TEXT(4, file_path);
        GET_TEXT(5, age_graph);
        GET_TEXT(6, node_query);
        GET_TEXT(7, edge_query);
        is_directed = GET_BOOL(8);
        is_weighted = GET_BOOL(9);
#undef GET_BOOL
#undef GET_TEXT
    }
    SPI_finish();

    if (is_live) {
        /* Live index: compact files */
        if (nodes_path[0] == '\0' || edges_path[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: live index \"%s\" has no file paths",
                            name_str)));

        /* Exclusive advisory lock — released at transaction end automatically */
        acquire_index_lock(name_str);
        try {
            index_storage_compact(nodes_path, edges_path);
        } catch (const std::exception &ex) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: compact failed: %s", ex.what())));
        }
        session_cache_invalidate(name_str);
    } else {
        /* Snapshot: rebuild from Cypher queries */
        if (age_graph[0] == '\0' || node_query[0] == '\0' || edge_query[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("graph_reindex: snapshot \"%s\" is missing queries",
                            name_str)));
        if (file_path[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: snapshot \"%s\" has no file_path",
                            name_str)));

        LoadedGraph loaded = age_load_graph(age_graph, node_query, edge_query,
                                            is_directed, is_weighted);
        try {
            gds_serialize(file_path, loaded.graph, loaded.id_map,
                          is_directed, is_weighted);
        } catch (const std::exception &ex) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: serialize failed: %s", ex.what())));
        }

        /* Update catalog counts */
        SPI_connect();
        {
            int64_t nc = (int64_t) loaded.graph.numberOfNodes();
            int64_t ec = (int64_t) loaded.graph.numberOfEdges();
            Oid   argtypes[3] = {INT8OID, INT8OID, TEXTOID};
            Datum args[3]     = {
                Int64GetDatum(nc),
                Int64GetDatum(ec),
                CStringGetTextDatum(name_str)
            };
            SPI_execute_with_args(
                "UPDATE age_gds.graphs SET node_count=$1, edge_count=$2 WHERE name=$3",
                3, argtypes, args, nullptr, false, 0);
        }
        SPI_finish();

        session_cache_invalidate(name_str);
    }

    PG_RETURN_VOID();
}

/* ============================================================ triggers ==== */

/*
 * Helper: look up nodes_file_path and edges_file_path for a live index by
 * name.  Returns false if not found.  Must be called inside an SPI session.
 */
static bool lookup_live_paths(const char *index_name,
                               char *nodes_out, size_t nodes_sz,
                               char *edges_out, size_t edges_sz)
{
    Oid   argtypes[1] = {TEXTOID};
    Datum args[1]     = {CStringGetTextDatum(index_name)};
    int   ret         = SPI_execute_with_args(
        "SELECT nodes_file_path, edges_file_path "
        "FROM age_gds.graphs WHERE name = $1 AND live = true",
        1, argtypes, args, nullptr, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
        return false;

    bool  isnull;
    Datum d1 = SPI_getbinval(SPI_tuptable->vals[0],
                               SPI_tuptable->tupdesc, 1, &isnull);
    if (!isnull) strlcpy(nodes_out, TextDatumGetCString(d1), nodes_sz);

    Datum d2 = SPI_getbinval(SPI_tuptable->vals[0],
                               SPI_tuptable->tupdesc, 2, &isnull);
    if (!isnull) strlcpy(edges_out, TextDatumGetCString(d2), edges_sz);

    return (nodes_out[0] != '\0' && edges_out[0] != '\0');
}

/* --------------------------------------------------- index_vertex_trigger - */

Datum age_gds_index_vertex_trigger(PG_FUNCTION_ARGS)
{
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errmsg("index_vertex_trigger: not called as trigger")));

    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    Trigger     *trigger  = trigdata->tg_trigger;

    if (trigger->tgnargs < 1)
        ereport(ERROR,
                (errmsg("index_vertex_trigger: missing index name argument")));

    const char *index_name = trigger->tgargs[0];

    /* Lookup file paths */
    char nodes_path[MAXPGPATH] = {0};
    char edges_path[MAXPGPATH] = {0};

    SPI_connect();
    bool found = lookup_live_paths(index_name,
                                   nodes_path, sizeof(nodes_path),
                                   edges_path, sizeof(edges_path));
    SPI_finish();

    if (!found)
        return PointerGetDatum(trigdata->tg_trigtuple);

    bool is_insert = TRIGGER_FIRED_BY_INSERT(trigdata->tg_event);
    bool is_delete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);
    bool is_update = TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event);

    TupleDesc desc = trigdata->tg_relation->rd_att;

    /* Get graphids from the appropriate tuple.
     * AGE vertex table column order: 1=id, 2=properties.
     * graphid is stored as int8 (8-byte integer). */
    auto get_graphid = [&](HeapTuple tup) -> int64_t {
        bool  isnull;
        Datum d = heap_getattr(tup, 1, desc, &isnull);
        if (isnull) return -1;
        return DatumGetInt64(d);
    };

    /* Acquire exclusive advisory lock — released when transaction ends */
    acquire_index_lock(index_name);

    try {
        if (is_insert) {
            /* INSERT: new row is in tg_trigtuple */
            int64_t gid = get_graphid(trigdata->tg_trigtuple);
            if (gid != -1) {
                index_storage_add_node(nodes_path, gid);
                index_storage_bump_version(nodes_path);
            }
        } else if (is_delete) {
            int64_t gid  = get_graphid(trigdata->tg_trigtuple);
            uint64_t slot = index_storage_find_node(nodes_path, gid);
            if (slot != UINT64_MAX) {
                index_storage_kill_node(nodes_path, slot);
                index_storage_bump_version(nodes_path);
            }
        } else if (is_update) {
            /* UPDATE: old row in tg_trigtuple, new row in tg_newtuple */
            int64_t old_gid = get_graphid(trigdata->tg_trigtuple);
            int64_t new_gid = get_graphid(trigdata->tg_newtuple);
            if (old_gid != new_gid) {
                /* ID changed: tombstone old, append new */
                uint64_t slot = index_storage_find_node(nodes_path, old_gid);
                if (slot != UINT64_MAX)
                    index_storage_kill_node(nodes_path, slot);
                if (new_gid != -1)
                    index_storage_add_node(nodes_path, new_gid);
            }
            /* Always bump version (properties or id changed) */
            index_storage_bump_version(nodes_path);
        }
    } catch (const std::exception &ex) {
        ereport(WARNING,
                (errmsg("index_vertex_trigger: file update failed: %s", ex.what())));
        return PointerGetDatum(trigdata->tg_trigtuple);
    }

    /* Invalidate this backend's cached graph so next algorithm call reloads */
    session_cache_invalidate(index_name);

    return PointerGetDatum(trigdata->tg_trigtuple);
}

/* --------------------------------------------------- index_edge_trigger --- */

Datum age_gds_index_edge_trigger(PG_FUNCTION_ARGS)
{
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errmsg("index_edge_trigger: not called as trigger")));

    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    Trigger     *trigger  = trigdata->tg_trigger;

    if (trigger->tgnargs < 1)
        ereport(ERROR,
                (errmsg("index_edge_trigger: missing index name argument")));

    const char *index_name = trigger->tgargs[0];

    char nodes_path[MAXPGPATH] = {0};
    char edges_path[MAXPGPATH] = {0};

    SPI_connect();
    bool found = lookup_live_paths(index_name,
                                   nodes_path, sizeof(nodes_path),
                                   edges_path, sizeof(edges_path));
    SPI_finish();

    if (!found)
        return PointerGetDatum(trigdata->tg_trigtuple);

    bool is_insert = TRIGGER_FIRED_BY_INSERT(trigdata->tg_event);
    bool is_delete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);
    bool is_update = TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event);

    TupleDesc desc = trigdata->tg_relation->rd_att;

    /* AGE edge table column order: 1=id, 2=start_id, 3=end_id, 4=properties */
    auto get_start_id = [&](HeapTuple tup) -> int64_t {
        bool  isnull;
        Datum d = heap_getattr(tup, 2, desc, &isnull);
        return isnull ? (int64_t)-1 : DatumGetInt64(d);
    };
    auto get_end_id = [&](HeapTuple tup) -> int64_t {
        bool  isnull;
        Datum d = heap_getattr(tup, 3, desc, &isnull);
        return isnull ? (int64_t)-1 : DatumGetInt64(d);
    };

    /* Acquire exclusive advisory lock — released when transaction ends */
    acquire_index_lock(index_name);

    try {
        auto do_insert_edge = [&](HeapTuple tup) {
            int64_t  sid = get_start_id(tup);
            int64_t  did = get_end_id(tup);
            if (sid == -1 || did == -1) return;
            uint64_t src_slot = index_storage_find_node(nodes_path, sid);
            uint64_t dst_slot = index_storage_find_node(nodes_path, did);
            if (src_slot == UINT64_MAX || dst_slot == UINT64_MAX) return;
            index_storage_add_edge(edges_path, src_slot, dst_slot, 1.0);
        };

        auto do_delete_edge = [&](HeapTuple tup) {
            int64_t  sid = get_start_id(tup);
            int64_t  did = get_end_id(tup);
            if (sid == -1 || did == -1) return;
            uint64_t src_slot = index_storage_find_node(nodes_path, sid);
            uint64_t dst_slot = index_storage_find_node(nodes_path, did);
            if (src_slot == UINT64_MAX || dst_slot == UINT64_MAX) return;
            uint64_t e_slot   = index_storage_find_edge(edges_path,
                                                         src_slot, dst_slot);
            if (e_slot != UINT64_MAX)
                index_storage_kill_edge(edges_path, e_slot);
        };

        if (is_insert) {
            /* INSERT: new row is in tg_trigtuple */
            do_insert_edge(trigdata->tg_trigtuple);
        } else if (is_delete) {
            do_delete_edge(trigdata->tg_trigtuple);
        } else if (is_update) {
            /* UPDATE: old in tg_trigtuple, new in tg_newtuple */
            do_delete_edge(trigdata->tg_trigtuple);
            do_insert_edge(trigdata->tg_newtuple);
        }
        index_storage_bump_version(nodes_path);
    } catch (const std::exception &ex) {
        ereport(WARNING,
                (errmsg("index_edge_trigger: file update failed: %s", ex.what())));
        return PointerGetDatum(trigdata->tg_trigtuple);
    }

    session_cache_invalidate(index_name);

    return PointerGetDatum(trigdata->tg_trigtuple);
}

/* --------------------------------------------------- on_new_label --------- */

Datum age_gds_on_new_label(PG_FUNCTION_ARGS)
{
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errmsg("on_new_label: not called as trigger")));

    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    /* INSERT trigger: new row in tg_trigtuple, tg_newtuple is NULL */
    HeapTuple tup  = trigdata->tg_trigtuple;
    TupleDesc desc = trigdata->tg_relation->rd_att;

    if (!tup)
        return PointerGetDatum(trigdata->tg_trigtuple);

    /* Extract label info from the new ag_label row.
     * ag_catalog.ag_label columns (by position in AGE schema):
     *   1: name (name)
     *   2: graph (oid — ag_graph.graphid)
     *   3: id (int2)
     *   4: kind (char: 'v' or 'e')
     *   5: relation (regclass)
     *   6: seq_name (name)
     *
     * We look up column numbers by name for robustness. */

    int name_col  = SPI_fnumber(desc, "name");
    int graph_col = SPI_fnumber(desc, "graph");
    int kind_col  = SPI_fnumber(desc, "kind");

    if (name_col <= 0 || graph_col <= 0 || kind_col <= 0)
        return PointerGetDatum(tup); /* unexpected schema */

    bool  isnull;
    Datum name_d  = heap_getattr(tup, name_col,  desc, &isnull);
    char *label_name = isnull ? nullptr : NameStr(*DatumGetName(name_d));

    Datum graph_d  = heap_getattr(tup, graph_col, desc, &isnull);
    Oid   graph_oid = isnull ? InvalidOid : DatumGetObjectId(graph_d);

    Datum kind_d  = heap_getattr(tup, kind_col,  desc, &isnull);
    char  kind    = isnull ? '\0' : DatumGetChar(kind_d);

    if (!label_name || graph_oid == InvalidOid || (kind != 'v' && kind != 'e'))
        return PointerGetDatum(tup);

    /* Skip internal AGE meta-labels */
    if (strcmp(label_name, "_ag_label_vertex") == 0 ||
        strcmp(label_name, "_ag_label_edge")   == 0)
        return PointerGetDatum(tup);

    /* Lookup the AGE graph name for this OID */
    char age_graph_name[256] = {0};

    SPI_connect();
    {
        Oid   argtypes[1] = {OIDOID};
        Datum args[1]     = {ObjectIdGetDatum(graph_oid)};
        int   ret         = SPI_execute_with_args(
            "SELECT name FROM ag_catalog.ag_graph WHERE graphid = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            bool  isnull2;
            Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                     SPI_tuptable->tupdesc, 1, &isnull2);
            if (!isnull2)
                strlcpy(age_graph_name, NameStr(*DatumGetName(d)), sizeof(age_graph_name));
        }
    }
    if (age_graph_name[0] == '\0') {
        SPI_finish();
        return PointerGetDatum(tup);
    }

    /* Find all live indices on this AGE graph */
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(age_graph_name)};
        int   ret         = SPI_execute_with_args(
            "SELECT name FROM age_gds.graphs "
            "WHERE age_graph = $1 AND live = true",
            1, argtypes, args, nullptr, true, 0);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            std::vector<std::string> index_names;
            index_names.reserve(SPI_processed);
            for (uint64_t i = 0; i < SPI_processed; i++) {
                bool  isnull2;
                Datum d = SPI_getbinval(SPI_tuptable->vals[i],
                                         SPI_tuptable->tupdesc, 1, &isnull2);
                if (!isnull2)
                    index_names.emplace_back(TextDatumGetCString(d));
            }
            for (auto &iname : index_names) {
                install_label_trigger(iname.c_str(), age_graph_name,
                                      label_name, kind);
            }
        }
    }

    SPI_finish();

    return PointerGetDatum(tup);
}

} /* extern "C" */
