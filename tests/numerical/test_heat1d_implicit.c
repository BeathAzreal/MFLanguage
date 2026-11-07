/* Analytical Robin eigenmode, convergence, energy and boundary regressions.
 * T = 300 + 50 cos(mu x) exp(-mu^2 t), mu tan(mu) = 1,
 * L = k = rho = cp = h_right = 1, left insulated, T_ambient = 300 K.
 * SPDX-License-Identifier: MIT
 */
#include "heat1d.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define OK(x) CHECK((x) == MFL_H1_OK)

static double root(void)
{
    double lo = 0, hi = 1.5;
    int i;
    for (i = 0; i < 80; i++) {
        double mid = (lo + hi) / 2;
        if (mid * tan(mid) < 1) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2;
}

static void configure(MflH1Config *c, size_t n, double dt, double end)
{
    double profile[201];
    double mu = root();
    size_t j;
    char msg[512];
    mfl_h1_config_init(c);
    CHECK(c->method == MFL_H1_FTCS);
    OK(mfl_h1_set_positive(c, MFL_H1_P_LENGTH, 1, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(c, MFL_H1_P_CONDUCTIVITY, 1, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(c, MFL_H1_P_DENSITY, 1, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(c, MFL_H1_P_SPECIFIC_HEAT, 1, NULL, msg, sizeof msg));
    OK(mfl_h1_set_nodes(c, (double)n, msg, sizeof msg));
    for (j = 0; j < n; j++) profile[j] = 300 + 50 * cos(mu * (double)j / (double)(n - 1));
    OK(mfl_h1_set_initial_profile(c, profile, n, msg, sizeof msg));
    OK(mfl_h1_set_boundary(c, MFL_H1_LEFT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg));
    OK(mfl_h1_set_convection(c, MFL_H1_RIGHT, 1, 300, msg, sizeof msg));
    OK(mfl_h1_set_method(c, MFL_H1_BACKWARD_EULER, msg, sizeof msg));
    OK(mfl_h1_set_positive(c, MFL_H1_P_TIME_STEP, dt, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(c, MFL_H1_P_END_TIME, end, NULL, msg, sizeof msg));
}

static double mode_error(size_t n, double dt, double end)
{
    MflH1Config c;
    MflH1Result r;
    char msg[512];
    double err = 0, mu = root();
    size_t j;
    configure(&c, n, dt, end);
    OK(mfl_h1_set_sample_every(&c, 1000000, msg, sizeof msg));
    OK(mfl_h1_run(&c, &r, msg, sizeof msg));
    if (r.T != NULL) {
        for (j = 0; j < n; j++) {
            double exact = 300 + 50 * cos(mu * r.positions[j]) * exp(-mu * mu * end);
            double e = fabs(r.T[n + j] - exact);
            if (e > err) err = e;
        }
        CHECK(r.times[r.samples - 1] == end);
    }
    mfl_h1_result_free(&r);
    mfl_h1_config_free(&c);
    return err;
}

static void test_energy_and_boundaries(void)
{
    MflH1Config c;
    MflH1Result r;
    char msg[512];
    size_t s, j;
    double limit;
    configure(&c, 11, 0.03, 0.2); /* shortened final step */
    OK(mfl_h1_set_convection(&c, MFL_H1_LEFT, 2, 290, msg, sizeof msg));
    OK(mfl_h1_run(&c, &r, msg, sizeof msg));
    CHECK(fabs(r.last_step - 0.02) < 1e-14);
    for (s = 1; s < r.samples; s++) {
        const double *old = r.T + (s - 1) * r.nodes;
        const double *now = r.T + s * r.nodes;
        double change = mfl_h1_discrete_energy(now, r.nodes, r.dx) -
                        mfl_h1_discrete_energy(old, r.nodes, r.dx);
        double flux = 2 * (now[0] - 290) + now[r.nodes - 1] - 300;
        CHECK(fabs(change + (r.times[s] - r.times[s - 1]) * flux) < 2e-12);
        for (j = 0; j < r.nodes; j++) CHECK(now[j] >= 290 && now[j] <= 350);
    }
    mfl_h1_result_free(&r);
    /* h = 0 is an insulated end, and uniform temperature is stationary. */
    OK(mfl_h1_set_convection(&c, MFL_H1_LEFT, 0, 290, msg, sizeof msg));
    OK(mfl_h1_set_convection(&c, MFL_H1_RIGHT, 0, 310, msg, sizeof msg));
    OK(mfl_h1_set_initial_uniform(&c, 340, NULL, msg, sizeof msg));
    OK(mfl_h1_run(&c, &r, msg, sizeof msg));
    for (j = 0; j < r.samples * r.nodes; j++) CHECK(fabs(r.T[j] - 340) < 2e-12);
    mfl_h1_result_free(&r);
    /* Dirichlet / Robin mixed case: exact steady linear profile. */
    OK(mfl_h1_set_boundary(&c, MFL_H1_LEFT, MFL_H1_BC_TEMPERATURE, 350, NULL, msg, sizeof msg));
    OK(mfl_h1_set_convection(&c, MFL_H1_RIGHT, 1, 300, msg, sizeof msg));
    OK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 10, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 200, NULL, msg, sizeof msg));
    OK(mfl_h1_run(&c, &r, msg, sizeof msg));
    for (j = 0; j < r.nodes; j++) CHECK(fabs(r.T[(r.samples - 1) * r.nodes + j] - (350 - 25 * r.positions[j])) < 1e-8);
    mfl_h1_result_free(&r);
    CHECK(mfl_h1_set_convection(&c, MFL_H1_LEFT, -1, 300, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_convection(&c, MFL_H1_LEFT, NAN, 300, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_convection(&c, MFL_H1_LEFT, 1, 0, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_method(&c, (MflH1Method)99, msg, sizeof msg) == MFL_H1_INVALID);
    OK(mfl_h1_set_method(&c, MFL_H1_FTCS, msg, sizeof msg));
    OK(mfl_h1_max_time_step(&c, &limit, msg, sizeof msg));
    CHECK(fabs(limit - 0.01 / 2.2) < 1e-15);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_UNSTABLE);
    CHECK(r.T == NULL);
    OK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, limit, NULL, msg, sizeof msg));
    OK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 1, NULL, msg, sizeof msg));
    OK(mfl_h1_run(&c, &r, msg, sizeof msg));
    for (j = 0; j < r.samples * r.nodes; j++) CHECK(r.T[j] >= 300 && r.T[j] <= 350);
    mfl_h1_result_free(&r);
    OK(mfl_h1_set_method(&c, MFL_H1_BACKWARD_EULER, msg, sizeof msg));
    c.memory_limit = 1;
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_NO_MEMORY);
    CHECK(r.T == NULL);
    c.memory_limit = 0;
    /* Overflow during matrix construction must free the partial result. */
    OK(mfl_h1_set_convection(&c, MFL_H1_RIGHT, 1e308, 300, msg, sizeof msg));
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(r.T == NULL);
    mfl_h1_config_free(&c);
}

int main(void)
{
    double e1 = mode_error(201, 0.02, 0.2);
    double e2 = mode_error(201, 0.01, 0.2);
    double e3 = mode_error(201, 0.005, 0.2);
    double s1 = mode_error(11, 0.000001, 0.05);
    double s2 = mode_error(21, 0.000001, 0.05);
    double s3 = mode_error(41, 0.000001, 0.05);
    printf("time errors %.9g %.9g %.9g; space errors %.9g %.9g %.9g\n", e1, e2, e3, s1, s2, s3);
    CHECK(e1 / e2 > 1.9 && e1 / e2 < 2.1);
    CHECK(e2 / e3 > 1.9 && e2 / e3 < 2.1);
    CHECK(s1 / s2 > 3.7 && s1 / s2 < 4.3);
    CHECK(s2 / s3 > 3.7 && s2 / s3 < 4.3);
    test_energy_and_boundaries();
    printf("implicit heat: %d failures\n", failures);
    return failures ? 1 : 0;
}
