// src/util/srf_helper.cpp

#include "srf_helper.h"

extern "C" {
#include "access/tupdesc.h"
#include "utils/memutils.h"
#include "miscadmin.h"
}

SRFState srf_begin(FunctionCallInfo fcinfo, int nattrs, Oid *types)
{
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    if (!rsi || !IsA(rsi, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));

    rsi->returnMode = SFRM_Materialize;

    MemoryContext old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

    TupleDesc tupdesc = CreateTemplateTupleDesc(nattrs);
    for (int i = 0; i < nattrs; i++)
        TupleDescInitEntry(tupdesc, (AttrNumber)(i + 1), NULL, types[i], -1, 0);
    tupdesc = BlessTupleDesc(tupdesc);

    Tuplestorestate *store = tuplestore_begin_heap(true, false, work_mem);

    MemoryContextSwitchTo(old);

    SRFState s;
    s.tuplestore = store;
    s.tupdesc    = tupdesc;
    return s;
}

void srf_emit(SRFState &s, Datum *values, bool *nulls)
{
    tuplestore_putvalues(s.tuplestore, s.tupdesc, values, nulls);
}

Datum srf_end(SRFState &s, FunctionCallInfo fcinfo)
{
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    rsi->setDesc   = s.tupdesc;
    rsi->setResult = s.tuplestore;
    fcinfo->isnull = true;
    return (Datum) 0;
}
