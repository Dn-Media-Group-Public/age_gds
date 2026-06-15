// src/util/config_parser.cpp

#include "config_parser.h"

#include <string>

extern "C" {
#include "utils/builtins.h"
}

PageRankConfig parse_pagerank_config(
    bool   has_config,    Datum  config_jsonb,
    bool   has_damping,   double damping_factor,
    bool   has_max_iter,  int    max_iterations,
    bool   has_tolerance, double tolerance
) {
    PageRankConfig cfg;

    if (has_config) {
        Jsonb          *jb  = DatumGetJsonbP(config_jsonb);
        JsonbIterator  *it  = JsonbIteratorInit(&jb->root);
        JsonbValue      key, val;
        JsonbIteratorToken tok;

        while ((tok = JsonbIteratorNext(&it, &key, false)) != WJB_DONE) {
            if (tok == WJB_KEY) {
                JsonbIteratorToken vtok = JsonbIteratorNext(&it, &val, false);
                (void)vtok;
                std::string k(key.val.string.val, key.val.string.len);

                if (k == "dampingFactor" && val.type == jbvNumeric) {
                    cfg.damping_factor = DatumGetFloat8(
                        DirectFunctionCall1(numeric_float8,
                                            NumericGetDatum(val.val.numeric)));
                } else if (k == "maxIterations" && val.type == jbvNumeric) {
                    cfg.max_iterations = (int)DatumGetFloat8(
                        DirectFunctionCall1(numeric_float8,
                                            NumericGetDatum(val.val.numeric)));
                } else if (k == "tolerance" && val.type == jbvNumeric) {
                    cfg.tolerance = DatumGetFloat8(
                        DirectFunctionCall1(numeric_float8,
                                            NumericGetDatum(val.val.numeric)));
                }
            }
        }
    }

    /* Named params override jsonb defaults */
    if (has_damping)   cfg.damping_factor = damping_factor;
    if (has_max_iter)  cfg.max_iterations = max_iterations;
    if (has_tolerance) cfg.tolerance      = tolerance;

    return cfg;
}
