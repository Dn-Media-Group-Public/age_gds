extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;
}

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

/* GUC variables */
static int  session_cache_mb = 512;
static char *data_dir = NULL;

/* Forward declarations */
void session_cache_register_exit_hook();

extern "C" void _PG_init(void);

void _PG_init(void)
{

    DefineCustomIntVariable(
        "age_gds.session_cache_mb",
        "Per-session nk::Graph cache size limit (MB)",
        NULL,
        &session_cache_mb,
        512,        /* boot default */
        1,          /* min */
        65536,      /* max */
        PGC_USERSET,
        0,
        NULL, NULL, NULL
    );

    DefineCustomStringVariable(
        "age_gds.data_dir",
        "Directory for .bin projection files",
        NULL,
        &data_dir,
        "$PGDATA/age_gds",
        PGC_SUSET,
        0,
        NULL, NULL, NULL
    );

    session_cache_register_exit_hook();
}

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_version);

Datum age_gds_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("0.1.0"));
}

/* Live index entry points — implemented in catalog/graph_index.cpp */
PG_FUNCTION_INFO_V1(age_gds_graph_index);
PG_FUNCTION_INFO_V1(age_gds_graph_reindex);
PG_FUNCTION_INFO_V1(age_gds_index_vertex_trigger);
PG_FUNCTION_INFO_V1(age_gds_index_edge_trigger);
PG_FUNCTION_INFO_V1(age_gds_on_new_label);

} /* extern "C" */
