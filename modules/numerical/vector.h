/*
 * vector.h - operations on arrays of doubles used by the `numerical` module
 * (v0.3). Pure C11 numerics: no units, no interpreter types. The language
 * binding (src/modules/natives.c) converts values to plain doubles, checks
 * dimensions and calls these functions.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NUMERICAL_VECTOR_H
#define MFL_NUMERICAL_VECTOR_H

#include <stddef.h>

/* out[i] = a + (b - a) * i / (n - 1) for i = 0 .. n-1, with out[0] = a
 * and out[n-1] = b exactly. n >= 2. */
void mfl_vec_linspace(double a, double b, size_t n, double *out);

/* Sum with Neumaier's compensated summation: the result is as accurate as
 * if it were computed in about twice the working precision, then rounded
 * (error bound ~ 2u|sum| + O(n u^2) sum|x_i|). n == 0 gives 0. Infinities
 * and NaN propagate as in plain summation. */
double mfl_vec_sum(const double *x, size_t n);

/* mfl_vec_sum(x, n) / n. n must be > 0. */
double mfl_vec_mean(const double *x, size_t n);

#endif /* MFL_NUMERICAL_VECTOR_H */
