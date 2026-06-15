extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;
}

#include <networkit/graph/Graph.hpp>
#include <networkit/Globals.hpp>

/* GUC variables */
static int  session_cache_mb = 512;
static char *data_dir = NULL;

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
}

extern "C" {

PG_FUNCTION_INFO_V1(age_gds_version);

Datum age_gds_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("0.1.0"));
}

} /* extern "C" */
