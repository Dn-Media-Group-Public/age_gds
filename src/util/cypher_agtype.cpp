// src/util/cypher_agtype.cpp
#include "cypher_agtype.h"

extern "C" {
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "lib/stringinfo.h"
}

#include <stdexcept>

/* ------------------------------------------------------------- agtype in/out */

static Datum agtype_from_cstring(const char *str)
{
    return DirectFunctionCall1(agtype_in, CStringGetDatum(str));
}

static char *agtype_arg_to_text(FunctionCallInfo fcinfo, int argno)
{
    return DatumGetCString(DirectFunctionCall1(agtype_out, PG_GETARG_DATUM(argno)));
}

bool agtype_arg_is_null(FunctionCallInfo fcinfo, int argno)
{
    if (PG_ARGISNULL(argno))
        return true;
    char *repr = agtype_arg_to_text(fcinfo, argno);
    return strcmp(repr, "null") == 0;
}

/* Decode a JSON string literal (as produced by agtype_out for a string
 * scalar, including the surrounding quotes) in place. Same approach as
 * age_sql/src/regexp.c's agtype_arg_to_cstring. */
static char *decode_json_string_literal(char *repr, const char *ctx)
{
    size_t len = strlen(repr);
    if (len < 2 || repr[0] != '"' || repr[len - 1] != '"')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s must be a string", ctx)));

    repr[len - 1] = '\0';
    char *result = repr + 1;
    char *dst = result;
    const char *src = result;
    while (*src) {
        if (*src == '\\' && *(src + 1)) {
            src++;
            switch (*src) {
                case '"':  *dst++ = '"';  break;
                case '\\': *dst++ = '\\'; break;
                case '/':  *dst++ = '/';  break;
                case 'n':  *dst++ = '\n'; break;
                case 'r':  *dst++ = '\r'; break;
                case 't':  *dst++ = '\t'; break;
                case 'b':  *dst++ = '\b'; break;
                case 'f':  *dst++ = '\f'; break;
                default:   *dst++ = '\\'; *dst++ = *src; break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return result;
}

Datum double_to_agtype_scalar(double v)
{
    Datum num = DirectFunctionCall1(float8out, Float8GetDatum(v));
    char *repr = DatumGetCString(num);
    return agtype_from_cstring(repr);
}

std::string get_agtype_arg_string(FunctionCallInfo fcinfo, int argno, const char *argname)
{
    if (PG_ARGISNULL(argno))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s is required", argname)));
    char *repr = agtype_arg_to_text(fcinfo, argno);
    return std::string(decode_json_string_literal(repr, argname));
}

/* ------------------------------------------------------------- reading config */

CypherConfig parse_cypher_config(FunctionCallInfo fcinfo, int argno)
{
    CypherConfig cfg;
    if (PG_ARGISNULL(argno))
        return cfg;

    char *repr = agtype_arg_to_text(fcinfo, argno);
    if (strcmp(repr, "null") == 0)
        return cfg;

    Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(repr));
    cfg.jb = DatumGetJsonbP(jsonb_datum);

    if (!JB_ROOT_IS_OBJECT(cfg.jb))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config must be a map")));
    return cfg;
}

static bool find_key(Jsonb *jb, const char *key, JsonbValue &out)
{
    if (!jb)
        return false;
    JsonbValue keyval;
    keyval.type = jbvString;
    keyval.val.string.val = (char *)key;
    keyval.val.string.len = (int)strlen(key);
    JsonbValue *found = findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &keyval);
    if (!found)
        return false;
    out = *found;
    return true;
}

bool CypherConfig::has(const char *key) const
{
    JsonbValue tmp;
    return find_key(jb, key, tmp);
}

bool CypherConfig::get_string(const char *key, std::string &out) const
{
    JsonbValue v;
    if (!find_key(jb, key, v))
        return false;
    if (v.type != jbvString)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config key '%s' must be a string", key)));
    out.assign(v.val.string.val, v.val.string.len);
    return true;
}

bool CypherConfig::get_double(const char *key, double &out) const
{
    JsonbValue v;
    if (!find_key(jb, key, v))
        return false;
    if (v.type != jbvNumeric)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config key '%s' must be a number", key)));
    out = DatumGetFloat8(DirectFunctionCall1(numeric_float8, NumericGetDatum(v.val.numeric)));
    return true;
}

bool CypherConfig::get_int(const char *key, int &out) const
{
    double d;
    if (!get_double(key, d))
        return false;
    out = (int)d;
    return true;
}

bool CypherConfig::get_int64(const char *key, int64_t &out) const
{
    double d;
    if (!get_double(key, d))
        return false;
    out = (int64_t)d;
    return true;
}

bool CypherConfig::get_bool(const char *key, bool &out) const
{
    JsonbValue v;
    if (!find_key(jb, key, v))
        return false;
    if (v.type != jbvBool)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config key '%s' must be a boolean", key)));
    out = v.val.boolean;
    return true;
}

bool CypherConfig::get_int64_array(const char *key, std::vector<int64_t> &out) const
{
    JsonbValue v;
    if (!find_key(jb, key, v))
        return false;
    if (v.type != jbvBinary || !JsonContainerIsArray(v.val.binary.data))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("config key '%s' must be an array", key)));

    JsonbIterator *it = JsonbIteratorInit(v.val.binary.data);
    JsonbValue elem;
    JsonbIteratorToken tok;
    out.clear();
    while ((tok = JsonbIteratorNext(&it, &elem, true)) != WJB_DONE) {
        if (tok != WJB_ELEM)
            continue;
        if (elem.type != jbvNumeric)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("config key '%s' must be an array of numbers", key)));
        double d = DatumGetFloat8(DirectFunctionCall1(numeric_float8, NumericGetDatum(elem.val.numeric)));
        out.push_back((int64_t)d);
    }
    return true;
}

void CypherConfig::validate_keys(const std::vector<std::string> &known_keys) const
{
    if (!jb)
        return;
    JsonbIterator *it = JsonbIteratorInit(&jb->root);
    JsonbValue key, val;
    JsonbIteratorToken tok;
    while ((tok = JsonbIteratorNext(&it, &key, false)) != WJB_DONE) {
        if (tok != WJB_KEY)
            continue;
        JsonbIteratorNext(&it, &val, true);
        std::string k(key.val.string.val, key.val.string.len);
        bool known = false;
        for (const auto &kk : known_keys)
            if (kk == k) { known = true; break; }
        if (!known)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("unknown config key '%s'", k.c_str())));
    }
}

/* ------------------------------------------------------------- result building */

CypherResultBuilder::CypherResultBuilder(bool as_array) : as_array_(as_array)
{
    if (as_array_)
        pushJsonbValue(&state_, WJB_BEGIN_ARRAY, nullptr);
}

void CypherResultBuilder::begin_row()
{
    pushJsonbValue(&state_, WJB_BEGIN_OBJECT, nullptr);
}

void CypherResultBuilder::end_row()
{
    final_ = pushJsonbValue(&state_, WJB_END_OBJECT, nullptr);
}

void CypherResultBuilder::key(const char *name)
{
    JsonbValue v;
    v.type = jbvString;
    v.val.string.val = pstrdup(name);
    v.val.string.len = (int)strlen(name);
    pushJsonbValue(&state_, WJB_KEY, &v);
}

void CypherResultBuilder::push_scalar(JsonbValue &v)
{
    pushJsonbValue(&state_, WJB_VALUE, &v);
}

void CypherResultBuilder::add_string(const std::string &s)
{
    JsonbValue v;
    v.type = jbvString;
    v.val.string.val = pstrdup(s.c_str());
    v.val.string.len = (int)s.size();
    push_scalar(v);
}

void CypherResultBuilder::add_int64(int64_t n)
{
    JsonbValue v;
    v.type = jbvNumeric;
    v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(n)));
    push_scalar(v);
}

void CypherResultBuilder::add_double(double d)
{
    JsonbValue v;
    v.type = jbvNumeric;
    v.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric, Float8GetDatum(d)));
    push_scalar(v);
}

void CypherResultBuilder::add_bool(bool b)
{
    JsonbValue v;
    v.type = jbvBool;
    v.val.boolean = b;
    push_scalar(v);
}

void CypherResultBuilder::add_null()
{
    JsonbValue v;
    v.type = jbvNull;
    push_scalar(v);
}

void CypherResultBuilder::add_int64_array(const std::vector<int64_t> &vals)
{
    pushJsonbValue(&state_, WJB_BEGIN_ARRAY, nullptr);
    for (int64_t n : vals) {
        JsonbValue v;
        v.type = jbvNumeric;
        v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(n)));
        pushJsonbValue(&state_, WJB_ELEM, &v);
    }
    pushJsonbValue(&state_, WJB_END_ARRAY, nullptr);
}

void CypherResultBuilder::add_double_array(const std::vector<double> &vals)
{
    pushJsonbValue(&state_, WJB_BEGIN_ARRAY, nullptr);
    for (double d : vals) {
        JsonbValue v;
        v.type = jbvNumeric;
        v.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric, Float8GetDatum(d)));
        pushJsonbValue(&state_, WJB_ELEM, &v);
    }
    pushJsonbValue(&state_, WJB_END_ARRAY, nullptr);
}

Datum CypherResultBuilder::finish()
{
    if (as_array_)
        final_ = pushJsonbValue(&state_, WJB_END_ARRAY, nullptr);

    Jsonb *jb = JsonbValueToJsonb(final_);
    StringInfoData buf;
    initStringInfo(&buf);
    JsonbToCString(&buf, &jb->root, VARSIZE(jb));
    return agtype_from_cstring(buf.data);
}
