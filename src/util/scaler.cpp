// src/util/scaler.cpp

#include "scaler.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

void apply_scaler(std::vector<double> &scores, const std::string &scaler)
{
    if (scaler.empty())
        return;

    const size_t n = scores.size();
    if (n == 0)
        return;

    if (scaler == "MinMax") {
        auto [min_it, max_it] = std::minmax_element(scores.begin(), scores.end());
        double lo = *min_it, hi = *max_it;
        double range = hi - lo;
        for (double &x : scores)
            x = (range == 0.0) ? 0.0 : (x - lo) / range;
    } else if (scaler == "Max") {
        double max_abs = 0.0;
        for (double x : scores)
            max_abs = std::max(max_abs, std::fabs(x));
        for (double &x : scores)
            x = (max_abs == 0.0) ? 0.0 : x / max_abs;
    } else if (scaler == "Mean") {
        double mean = std::accumulate(scores.begin(), scores.end(), 0.0) / (double)n;
        for (double &x : scores)
            x = (mean == 0.0) ? 0.0 : x / mean;
    } else if (scaler == "Log") {
        for (double &x : scores)
            x = (x > 0.0) ? std::log(x) : 0.0;
    } else if (scaler == "StdScore") {
        double mean = std::accumulate(scores.begin(), scores.end(), 0.0) / (double)n;
        double var = 0.0;
        for (double x : scores) {
            double d = x - mean;
            var += d * d;
        }
        var /= (double)n;
        double stddev = std::sqrt(var);
        for (double &x : scores)
            x = (stddev == 0.0) ? 0.0 : (x - mean) / stddev;
    } else {
        throw std::invalid_argument(
            "unknown scaler \"" + scaler +
            "\" (expected one of MinMax, Max, Mean, Log, StdScore)");
    }
}
