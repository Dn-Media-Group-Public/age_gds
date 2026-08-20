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
#include "util/pair_hash.h"
#include "util/cypher_agtype.h"

#include <string>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

/*
 * Discover all (non-meta) labels on age_graph_str and install a trigger for
 * any label that doesn't already have one recorded in index_triggers for
 * this index (install_label_trigger's ON CONFLICT DO NOTHING on the catalog
 * insert doesn't stop CREATE TRIGGER from re-running, so we filter here).
 * Runs inside an already-open SPI session.
 */
static void install_missing_label_triggers(
    const char *index_name,
    const char *age_graph_str)
{
    struct LabelInfo { std::string name; char kind; };
    std::vector<LabelInfo> labels;

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
                     errmsg("graph_reindex: label query failed (SPI %d)", ret)));

        for (uint64_t i = 0; i < SPI_processed; i++) {
            bool  isnull;
            Datum dn = SPI_getbinval(SPI_tuptable->vals[i],
                                      SPI_tuptable->tupdesc, 1, &isnull);
            Datum dk = SPI_getbinval(SPI_tuptable->vals[i],
                                      SPI_tuptable->tupdesc, 2, &isnull);
            if (isnull) continue;
            char *lname = pstrdup(NameStr(*DatumGetName(dn)));
            char  lkind = DatumGetChar(dk);
            if (strcmp(lname, "_ag_label_vertex") == 0) continue;
            if (strcmp(lname, "_ag_label_edge")   == 0) continue;
            labels.push_back({std::string(lname), lkind});
        }
    }

    for (auto &li : labels) {
        Oid   argtypes[2] = {TEXTOID, TEXTOID};
        Datum args[2]     = {
            CStringGetTextDatum(index_name),
            CStringGetTextDatum(li.name.c_str())
        };
        int ret = SPI_execute_with_args(
            "SELECT 1 FROM age_gds.index_triggers "
            "WHERE graph_name = $1 AND table_name = $2",
            2, argtypes, args, nullptr, true, 1);

        bool already_installed = (ret == SPI_OK_SELECT && SPI_processed > 0);
        if (!already_installed)
            install_label_trigger(index_name, age_graph_str,
                                  li.name.c_str(), li.kind);
    }
}

/*
 * Overwrite <nodes_path>/<edges_path> from scratch with `loaded`, then bump
 * version_counter until it differs from its pre-rebuild value so that every
 * backend's cached copy is recognized as stale.
 */
static void rebuild_slot_files_from_scratch(
    const char       *nodes_path,
    const char       *edges_path,
    const LoadedGraph &loaded,
    bool              directed,
    bool              weighted)
{
    uint64_t old_version = 0;
    try {
        old_version = index_storage_read_version(nodes_path);
    } catch (...) {
        /* file may not exist yet; treat as 0 */
    }

    try {
        index_storage_write_initial(nodes_path, edges_path,
                                    loaded.graph, loaded.id_map,
                                    directed, weighted);
        /* write_initial always resets version_counter to 1; force it past
         * old_version so every session cache's version check notices the
         * rewrite, regardless of how high old_version already was. */
        index_storage_set_version(nodes_path, old_version + 1);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_reindex: full rebuild failed: %s", ex.what())));
    }
}

/*
 * Query-diff-driven incremental reindex: re-run node_query/edge_query, diff
 * against the current slot/tombstone files by identity (graphid for nodes,
 * (src_gid, dst_gid) for edges), and apply only the delta via index_storage's
 * append/tombstone/weight-update primitives. Bumps version_counter once.
 */
static void diff_apply_reindex(
    const char *nodes_path,
    const char *edges_path,
    const char *age_graph,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted,
    int64_t    &out_node_count,
    int64_t    &out_edge_count)
{
    LoadedGraph new_loaded = age_load_graph(age_graph, node_query, edge_query,
                                            directed, weighted);

    IndexDeserializedGraph old_dg;
    try {
        old_dg = index_storage_deserialize(nodes_path, edges_path);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_reindex: deserialize failed: %s", ex.what())));
    }

    std::unordered_set<int64_t> new_node_gids;
    for (auto &kv : new_loaded.id_map.graphid_to_nk)
        new_node_gids.insert(kv.first);

    std::unordered_set<int64_t> old_node_gids;
    for (auto &kv : old_dg.id_map.graphid_to_nk)
        old_node_gids.insert(kv.first);

    struct OldEdgeInfo { double weight; uint64_t file_slot; };
    std::unordered_map<std::pair<int64_t,int64_t>, OldEdgeInfo, PairHash> old_edges;
    for (auto &kv : old_dg.edge_file_index) {
        NetworKit::node src_nk = kv.first.first;
        NetworKit::node dst_nk = kv.first.second;
        int64_t src_gid = old_dg.id_map.resolve(src_nk);
        int64_t dst_gid = old_dg.id_map.resolve(dst_nk);
        double  w       = old_dg.graph.weight(src_nk, dst_nk);
        old_edges[{src_gid, dst_gid}] = OldEdgeInfo{w, kv.second};
    }

    std::unordered_map<std::pair<int64_t,int64_t>, double, PairHash> new_edges;
    new_loaded.graph.forEdges([&](NetworKit::node u, NetworKit::node v,
                                  NetworKit::edgeweight w) {
        int64_t src_gid = new_loaded.id_map.resolve(u);
        int64_t dst_gid = new_loaded.id_map.resolve(v);
        new_edges[{src_gid, dst_gid}] = static_cast<double>(w);
    });

    try {
        for (int64_t gid : old_node_gids) {
            if (new_node_gids.count(gid)) continue;
            uint64_t slot = index_storage_find_node(nodes_path, gid);
            if (slot != UINT64_MAX)
                index_storage_kill_node(nodes_path, slot);
        }
        for (int64_t gid : new_node_gids) {
            if (old_node_gids.count(gid)) continue;
            index_storage_add_node(nodes_path, gid);
        }

        for (auto &kv : old_edges) {
            if (new_edges.count(kv.first)) continue;
            index_storage_kill_edge(edges_path, kv.second.file_slot);
        }
        for (auto &kv : new_edges) {
            auto it = old_edges.find(kv.first);
            if (it == old_edges.end()) {
                uint64_t src_slot = index_storage_find_node(nodes_path, kv.first.first);
                uint64_t dst_slot = index_storage_find_node(nodes_path, kv.first.second);
                if (src_slot == UINT64_MAX || dst_slot == UINT64_MAX)
                    continue; /* shouldn't happen: node diff ran first */
                index_storage_add_edge(edges_path, src_slot, dst_slot, kv.second);
            } else if (it->second.weight != kv.second) {
                index_storage_set_edge_weight(edges_path, it->second.file_slot, kv.second);
            }
        }

        index_storage_bump_version(nodes_path);
    } catch (const std::exception &ex) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("graph_reindex: diff-apply failed: %s", ex.what())));
    }

    out_node_count = (int64_t) new_node_gids.size();
    out_edge_count = (int64_t) new_edges.size();
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
        Oid   argtypes[9] = {TEXTOID, TEXTOID, INT8OID, INT8OID,
                              BOOLOID, BOOLOID, TEXTOID, TEXTOID, TEXTOID};
        Datum args[9]     = {
            CStringGetTextDatum(name_str),
            CStringGetTextDatum(age_graph_str),
            Int64GetDatum(node_count),
            Int64GetDatum(edge_count),
            BoolGetDatum(directed),
            BoolGetDatum(weighted),
            CStringGetTextDatum(nodes_path),
            CStringGetTextDatum(edges_path),
            CStringGetTextDatum(edge_q)
        };
        int ret = SPI_execute_with_args(
            "INSERT INTO age_gds.graphs "
            "(name, age_graph, node_count, edge_count, directed, weighted, "
            " live, incremental, nodes_file_path, edges_file_path, edge_query) "
            "VALUES ($1, $2, $3, $4, $5, $6, true, true, $7, $8, $9)",
            9, argtypes, args, nullptr, false, 0);
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

static void do_graph_reindex(const char *name_str, bool force_is_null, bool force_val)
{
    /* Look up catalog */
    char  nodes_path[MAXPGPATH] = {0};
    char  edges_path[MAXPGPATH] = {0};
    char  file_path[MAXPGPATH]  = {0};
    char  age_graph[256]        = {0};
    char  node_query[4096]      = {0};
    char  edge_query[4096]      = {0};
    bool  is_live               = false;
    bool  is_incremental        = false;
    bool  is_directed           = false;
    bool  is_weighted           = false;

    SPI_connect();
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(name_str)};
        int   ret         = SPI_execute_with_args(
            "SELECT live, nodes_file_path, edges_file_path, "
            "       file_path, age_graph, node_query, edge_query, "
            "       directed, weighted, incremental "
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
        is_directed    = GET_BOOL(8);
        is_weighted    = GET_BOOL(9);
        is_incremental = GET_BOOL(10);
#undef GET_BOOL
#undef GET_TEXT
    }
    SPI_finish();

    if (!is_incremental) {
        /* ---- Flat storage (graph_project default) ---- */
        if (force_is_null == false && force_val == false)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("graph_reindex: flat projection \"%s\" has no "
                            "incremental mode (force := false is invalid)",
                            name_str)));

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

        uint64_t old_version = 0;
        try {
            old_version = gds_read_version(file_path);
        } catch (...) {
            /* fall through with old_version = 0 */
        }

        LoadedGraph loaded = age_load_graph(age_graph, node_query, edge_query,
                                            is_directed, is_weighted);
        try {
            gds_serialize(file_path, loaded.graph, loaded.id_map,
                          is_directed, is_weighted, old_version + 1);
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

    } else if (!is_live) {
        /* ---- Incremental, non-live: graph_project(incremental := true) ---- */
        if (nodes_path[0] == '\0' || edges_path[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: \"%s\" has no file paths", name_str)));
        if (age_graph[0] == '\0' || node_query[0] == '\0' || edge_query[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("graph_reindex: \"%s\" is missing queries", name_str)));

        acquire_index_lock(name_str);

        int64_t nc = 0, ec = 0;
        if (force_is_null == false && force_val == true) {
            /* Full rebuild: fresh load into new slot files */
            LoadedGraph loaded = age_load_graph(age_graph, node_query, edge_query,
                                                is_directed, is_weighted);
            rebuild_slot_files_from_scratch(nodes_path, edges_path, loaded,
                                            is_directed, is_weighted);
            nc = (int64_t) loaded.graph.numberOfNodes();
            ec = (int64_t) loaded.graph.numberOfEdges();
        } else {
            /* Diff-and-apply (default) */
            diff_apply_reindex(nodes_path, edges_path,
                               age_graph, node_query, edge_query,
                               is_directed, is_weighted, nc, ec);
        }

        SPI_connect();
        {
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

    } else {
        /* ---- Incremental, live: graph_index ---- */
        if (nodes_path[0] == '\0' || edges_path[0] == '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("graph_reindex: live index \"%s\" has no file paths",
                            name_str)));

        acquire_index_lock(name_str);

        if (force_is_null == false && force_val == true) {
            /* Full re-load from AGE: rediscover labels, rebuild slot files
             * from scratch. Triggers already installed stay installed;
             * install any missing ones for newly-discovered labels. */
            const char *node_q = "MATCH (n) RETURN id(n) AS id";
            const char *edge_q = "MATCH (a)-[r]->(b) "
                                 "RETURN id(a) AS src, id(b) AS dst, r.weight AS weight";
            LoadedGraph loaded = age_load_graph(age_graph, node_q, edge_q,
                                                is_directed, is_weighted);
            rebuild_slot_files_from_scratch(nodes_path, edges_path, loaded,
                                            is_directed, is_weighted);

            SPI_connect();
            install_missing_label_triggers(name_str, age_graph);
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
        } else {
            /* Compact (default): rewrite files without tombstones */
            try {
                index_storage_compact(nodes_path, edges_path);
            } catch (const std::exception &ex) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("graph_reindex: compact failed: %s", ex.what())));
            }
        }

        session_cache_invalidate(name_str);
    }
}

Datum age_gds_graph_reindex(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("graph_reindex: name must not be NULL")));

    char *name_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool force_is_null = PG_ARGISNULL(1);
    bool force_val     = force_is_null ? false : PG_GETARG_BOOL(1);

    do_graph_reindex(name_str, force_is_null, force_val);
    PG_RETURN_VOID();
}

/* --------------------------------------------------- graph_reindex (Cypher) */
/* age_gds.graph_reindex(graphName agtype, config agtype DEFAULT NULL);
 * config = {force}. */

PG_FUNCTION_INFO_V1(age_gds_graph_reindex_cypher);

Datum age_gds_graph_reindex_cypher(PG_FUNCTION_ARGS)
{
    if (agtype_arg_is_null(fcinfo, 0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("graph_reindex: graphName is required")));
    std::string name = get_agtype_arg_string(fcinfo, 0, "graphName");

    CypherConfig cfg = parse_cypher_config(fcinfo, 1);
    cfg.validate_keys({"force"});
    bool force_val = false;
    bool force_is_null = !cfg.get_bool("force", force_val);

    do_graph_reindex(name.c_str(), force_is_null, force_val);

    CypherResultBuilder b(false);
    b.begin_row();
    b.key("graphName"); b.add_string(name);
    b.end_row();
    PG_RETURN_DATUM(b.finish());
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
