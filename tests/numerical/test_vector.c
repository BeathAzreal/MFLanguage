/*
 * test_vector.c - modules/numerical/vector.c (linspace, compensated sum,
 * mean). References are computed independently of the code under test.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "vector.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

static void test_linspace(void)
{
    double x[11];
    size_t i;
    mfl_vec_linspace(0.0, 0.1, 11, x);
    CHECK(x[0] == 0.0 && x[10] == 0.1); /* end points exact */
    for (i = 0; i < 11; i++) {
        /* tolerance: one rounding of the product and one of the sum */
        CHECK(fabs(x[i] - 0.01 * (double)i) <= 2e-17 * 10);
    }
    for (i = 1; i < 11; i++) {
        CHECK(x[i] > x[i - 1]); /* strictly increasing */
    }
    mfl_vec_linspace(5.0, -5.0, 3, x);
    CHECK(x[0] == 5.0 && x[1] == 0.0 && x[2] == -5.0);
    mfl_vec_linspace(1.0, 2.0, 2, x);
    CHECK(x[0] == 1.0 && x[1] == 2.0);
}

static void test_sum(void)
{
    double tenth[10];
    double big[3] = {1e16, 1.0, -1e16};
    double *many;
    size_t i;
    double naive = 0.0;

    CHECK(mfl_vec_sum(NULL, 0) == 0.0); /* empty sum */
    for (i = 0; i < 10; i++) {
        tenth[i] = 0.1;
        naive += 0.1;
    }
    CHECK(naive != 1.0);                 /* plain summation rounds ... */
    CHECK(mfl_vec_sum(tenth, 10) == 1.0); /* ... the compensated sum does not */
    CHECK(mfl_vec_sum(big, 3) == 1.0);   /* catastrophic cancellation recovered */
    CHECK(mfl_vec_mean(tenth, 10) == 0.1);

    /* 1e6 copies of 0.1: exact reference 100000 (0.1 is 0.1000000000000000055511151231257827
     * in binary, so the exact sum is 100000.0000000000055511..., which rounds to 100000). */
    many = (double *)malloc(1000000 * sizeof *many);
    if (many != NULL) {
        for (i = 0; i < 1000000; i++) {
            many[i] = 0.1;
        }
        CHECK(mfl_vec_sum(many, 1000000) == 100000.0);
        free(many);
    }
    {
        double inf[3] = {1.0, INFINITY, 2.0};
        double nan3[2] = {1.0, NAN};
        CHECK(isinf(mfl_vec_sum(inf, 3)));
        CHECK(isnan(mfl_vec_sum(nan3, 2)));
    }
}

int main(void)
{
    test_linspace();
    test_sum();
    printf("test_vector: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
