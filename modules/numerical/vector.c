/* vector.c - see vector.h. SPDX-License-Identifier: MIT */
#include "vector.h"

#include <math.h>

void mfl_vec_linspace(double a, double b, size_t n, double *out)
{
    size_t i;
    double span = b - a;
    double last = (double)(n - 1);
    for (i = 0; i < n; i++) {
        out[i] = a + span * ((double)i / last);
    }
    out[0] = a;
    out[n - 1] = b;
}

double mfl_vec_sum(const double *x, size_t n)
{
    double s = 0.0;
    double c = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double t = s + x[i];
        if (!isfinite(t)) {
            s = t;
            c = 0.0;
            continue;
        }
        if (fabs(s) >= fabs(x[i])) {
            c += (s - t) + x[i];
        } else {
            c += (x[i] - t) + s;
        }
        s = t;
    }
    return isfinite(s) ? s + c : s;
}

double mfl_vec_mean(const double *x, size_t n)
{
    return mfl_vec_sum(x, n) / (double)n;
}
