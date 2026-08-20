// src/util/cypher_agtype.h
//
// agtype <-> jsonb <-> text glue for the Cypher-callable (agtype, agtype)
// overloads. Only agtype_in/agtype_out are called across the age.so
// boundary (see docs/architecture.md's "Cypher-callable interface"
// section for why); everything else uses PostgreSQL's own core
// utils/jsonb.h API, exactly like config_parser.cpp already does for
// PageRank.
#pragma once

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/jsonb.h"
}

#include <cstdint>
#include <string>
#include <vector>

// age_gds links against age.so (see CMakeLists.txt); these two symbols
// are AGE's only stable, exported agtype entry points.
extern "C" {
Datum agtype_out(PG_FUNCTION_ARGS);
Datum agtype_in(PG_FUNCTION_ARGS);
}

// Read-only view over a parsed agtype config argument (a JSON object).
// `has_config` false means the caller passed NULL/omitted config; every
// getter then behaves as "key not present".
struct CypherConfig {
    Jsonb *jb = nullptr;

    bool has(const char *key) const;
    bool get_string(const char *key, std::string &out) const;
    bool get_double(const char *key, double &out) const;
    bool get_int(const char *key, int &out) const;
    bool get_int64(const char *key, int64_t &out) const;
    bool get_bool(const char *key, bool &out) const;
    // Array of scalar bigints, e.g. targetNodes: [1,2,3].
    bool get_int64_array(const char *key, std::vector<int64_t> &out) const;

    // Errors (naming the bad key) if any top-level key is not in
    // `known_keys`. Matches Neo4j's strict config validation.
    void validate_keys(const std::vector<std::string> &known_keys) const;
};

// Parse the `config agtype` argument (arg index `argno`) of the current
// call into a CypherConfig. Returns an empty CypherConfig if the arg is
// NULL.
CypherConfig parse_cypher_config(FunctionCallInfo fcinfo, int argno);

// Parse any agtype argument (arg index `argno`) into a Jsonb*, without
// requiring it to be a map (used for nodeProjection/relationshipProjection,
// which may be the string '*' or a map of label -> projection-config).
// Returns nullptr if the arg is SQL NULL.
Jsonb *agtype_arg_to_jsonb_any(FunctionCallInfo fcinfo, int argno);

// True if `jb` is a JSON string equal to `s` (case-sensitive).
bool jsonb_is_string_eq(Jsonb *jb, const char *s);

// Extract a plain C string from a string-typed agtype argument (arg
// index `argno`). Errors if NULL or not a string.
std::string get_agtype_arg_string(FunctionCallInfo fcinfo, int argno, const char *argname);

// True if the agtype argument is SQL NULL or JSON null.
bool agtype_arg_is_null(FunctionCallInfo fcinfo, int argno);

// Build a bare scalar agtype (not a map/array) from a double. For the
// single-pair link-prediction functions, which are already scalar
// (RETURNS float8 in SQL) and need no array/map packing for their Cypher
// overload -- see plan phase 5.
Datum double_to_agtype_scalar(double v);

// Same as double_to_agtype_scalar(), for bool (graph_exists, graph_is_stale).
Datum bool_to_agtype_scalar(bool v);

// --- Result building: JSON array-of-maps (*_stream) or single map
// (*_stats/*_write), converted to an agtype Datum. ---
class CypherResultBuilder {
public:
    explicit CypherResultBuilder(bool as_array);

    void begin_row();      // begin one map (element of the array, or the only map)
    void end_row();

    void key(const char *name);
    void add_string(const std::string &v);
    void add_int64(int64_t v);
    void add_double(double v);
    void add_bool(bool v);
    void add_null();
    void add_int64_array(const std::vector<int64_t> &v);
    void add_double_array(const std::vector<double> &v);

    Datum finish();

private:
    JsonbParseState *state_ = nullptr;
    JsonbValue       *final_ = nullptr;
    bool as_array_;
    void push_scalar(JsonbValue &v);
};
