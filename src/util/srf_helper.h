// src/util/srf_helper.h
#pragma once

extern "C" {
#include "postgres.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
}

struct SRFState {
    Tuplestorestate *tuplestore;
    TupleDesc        tupdesc;
};

SRFState srf_begin(FunctionCallInfo fcinfo, int nattrs, Oid *types);
void     srf_emit(SRFState &s, Datum *values, bool *nulls);
Datum    srf_end(SRFState &s, FunctionCallInfo fcinfo);
