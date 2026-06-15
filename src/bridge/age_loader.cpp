// src/bridge/age_loader.cpp
//
// Load an AGE Cypher graph into a NetworKit::Graph via SPI.
//
// Include order: NetworKit first, then PostgreSQL headers, then #undef ERROR.
// NetworKit's Log.hpp redefines ERROR as a variadic macro that shadows the
// PostgreSQL integer constant.  We restore it after all includes.

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

#include "bridge/age_loader.h"

#include <string>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
}

#ifdef ERROR
#  undef ERROR
#endif
#define ERROR 21

/* ------------------------------------------------------------------ */

LoadedGraph age_load_graph(
    const char *age_graph_name,
    const char *node_query,
    const char *edge_query,
    bool        directed,
    bool        weighted)
{
    LoadedGraph result;
    result.graph = NetworKit::Graph(0, weighted, directed);

    SPI_connect();

    /* ---- 1. Verify the AGE graph exists ---- */
    {
        Oid   argtypes[1] = {TEXTOID};
        Datum args[1]     = {CStringGetTextDatum(age_graph_name)};

        int ret = SPI_execute_with_args(
            "SELECT 1 FROM ag_catalog.ag_graph WHERE name = $1",
            1, argtypes, args, nullptr, true, 1);

        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: graph existence check failed"
                            " (SPI code %d)", ret)));
        }

        if (SPI_processed == 0) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("AGE graph \"%s\" does not exist",
                            age_graph_name)));
        }
    }

    /* ---- 2. Load nodes ---- */
    {
        std::string sql =
            std::string("SELECT * FROM ag_catalog.cypher('")
            + age_graph_name
            + "', $$ " + node_query + " $$) AS (id agtype)";

        int ret = SPI_execute(sql.c_str(), true, 0);
        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: node query failed"
                            " (SPI code %d)", ret)));
        }

        uint64_t nrows = SPI_processed;
        for (uint64_t i = 0; i < nrows; i++) {
            char *txt = SPI_getvalue(SPI_tuptable->vals[i],
                                     SPI_tuptable->tupdesc, 1);
            if (!txt)
                continue;
            int64_t gid = (int64_t) strtoll(txt, nullptr, 10);
            result.id_map.intern(gid);
            result.graph.addNode();
        }
    }

    /* ---- 3. Load edges ---- */
    {
        std::string sql =
            std::string("SELECT * FROM ag_catalog.cypher('")
            + age_graph_name
            + "', $$ " + edge_query
            + " $$) AS (src agtype, dst agtype, weight agtype)";

        int ret = SPI_execute(sql.c_str(), true, 0);
        if (ret != SPI_OK_SELECT) {
            SPI_finish();
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("age_load_graph: edge query failed"
                            " (SPI code %d)", ret)));
        }

        int      ncols = SPI_tuptable->tupdesc->natts;
        uint64_t nrows = SPI_processed;

        for (uint64_t i = 0; i < nrows; i++) {
            HeapTuple tup  = SPI_tuptable->vals[i];
            TupleDesc desc = SPI_tuptable->tupdesc;

            char *src_txt = SPI_getvalue(tup, desc, 1);
            char *dst_txt = SPI_getvalue(tup, desc, 2);
            if (!src_txt || !dst_txt)
                continue;

            int64_t src_gid = (int64_t) strtoll(src_txt, nullptr, 10);
            int64_t dst_gid = (int64_t) strtoll(dst_txt, nullptr, 10);

            auto sit = result.id_map.graphid_to_nk.find(src_gid);
            auto dit = result.id_map.graphid_to_nk.find(dst_gid);
            if (sit == result.id_map.graphid_to_nk.end() ||
                dit == result.id_map.graphid_to_nk.end())
                continue;   /* edge references an unknown node — skip */

            NetworKit::node src_nk = sit->second;
            NetworKit::node dst_nk = dit->second;

            double w = 1.0;
            if (ncols >= 3) {
                char *wt_txt = SPI_getvalue(tup, desc, 3);
                if (wt_txt)
                    w = strtod(wt_txt, nullptr);
            }

            if (weighted)
                result.graph.addEdge(src_nk, dst_nk,
                                     static_cast<NetworKit::edgeweight>(w));
            else
                result.graph.addEdge(src_nk, dst_nk);
        }
    }

    SPI_finish();
    return result;
}
