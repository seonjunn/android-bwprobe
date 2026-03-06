/*
 * stats.h — Basic descriptive statistics
 *
 * stat_mean(v, n)      : arithmetic mean of n doubles
 * stat_sd(v, n, mean)  : sample standard deviation (denominator n-1)
 */

#pragma once
#include <math.h>

static inline double stat_mean(const double *v, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) s += v[i];
    return s / n;
}

static inline double stat_sd(const double *v, int n, double m)
{
    if (n < 2) return 0.0;
    double s = 0.0;
    for (int i = 0; i < n; i++) { double d = v[i] - m; s += d * d; }
    return sqrt(s / (n - 1));
}
