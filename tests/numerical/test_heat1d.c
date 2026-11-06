/*
 * test_heat1d.c - verification of the 1D FTCS heat conduction solver
 * (modules/numerical/heat1d.c) against references computed independently
 * of the solver. Every tolerance is declared next to its test.
 *
 * With an argument, also prints the mesh refinement table (used in the
 * documentation): test_heat1d --table
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heat1d.h"

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

#define CHECK_MSG(cond, msg)                                                                  \
    do {                                                                                      \
        g_checks++;                                                                           \
        if (!(cond)) {                                                                        \
            g_failures++;                                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s (message: %s)\n", __FILE__, __LINE__,     \
                    #cond, msg);                                                              \
        }                                                                                     \
    } while (0)

static const double PI = 3.14159265358979323846;

/* Steel bar used by most tests (SI). alpha = 45 / (7850 * 460) m^2/s. */
#define L_BAR 0.1
#define K_BAR 45.0
#define RHO_BAR 7850.0
#define CP_BAR 460.0

static void bar(MflH1Config *c, double length)
{
    char msg[512];
    mfl_h1_config_init(c);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_LENGTH, length, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_CONDUCTIVITY, K_BAR, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_DENSITY, RHO_BAR, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_SPECIFIC_HEAT, CP_BAR, NULL, msg, sizeof msg) == MFL_H1_OK);
}

static double alpha_bar(void) { return K_BAR / (RHO_BAR * CP_BAR); }

/* ------------------------------------------------------------------------ */
/* 1. Analytical solution: both ends at T0, T(x,0) = T0 + A sin(pi x / L)   */
/*    T(x,t) = T0 + A sin(pi x / L) exp(-alpha pi^2 t / L^2)                */
/* ------------------------------------------------------------------------ */

#define T0_REF 300.0
#define A_REF 50.0
#define T_END_REF 60.0 /* s; alpha pi^2 t / L^2 = 0.73: the amplitude decays to 48 % */

static double exact_sine(double x, double t)
{
    double a = alpha_bar();
    return T0_REF + A_REF * sin(PI * x / L_BAR) * exp(-a * PI * PI * t / (L_BAR * L_BAR));
}

typedef struct {
    size_t nodes;
    double dt;
    double err_inf;     /* max over all nodes and samples, K */
    double err_quarter; /* x = L/4 at t = T_END_REF */
    double err_center;  /* x = L/2 at t = T_END_REF */
    double err_mid_time; /* x = L/2 at the sample nearest T_END_REF / 2 */
    double mid_time;
} SineRun;

/* Fourier number kept constant while refining: dt = FO_REF dx^2 / alpha,
 * so halving dx divides dt by 4 (stable: FO_REF < 1/2). */
#define FO_REF 0.25

static SineRun run_sine(size_t nodes)
{
    MflH1Config c;
    MflH1Result r;
    char msg[512];
    double *T;
    double dx = L_BAR / (double)(nodes - 1);
    double dt = FO_REF * dx * dx / alpha_bar();
    SineRun out;
    size_t j;
    size_t s;
    size_t mid = 0;

    memset(&out, 0, sizeof out);
    out.nodes = nodes;
    out.dt = dt;
    bar(&c, L_BAR);
    T = (double *)malloc(nodes * sizeof *T);
    for (j = 0; j < nodes; j++) {
        T[j] = exact_sine((double)j * dx, 0.0);
    }
    T[0] = T0_REF;
    T[nodes - 1] = T0_REF;
    CHECK(mfl_h1_set_initial_profile(&c, T, nodes, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_LEFT, MFL_H1_BC_TEMPERATURE, T0_REF, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_RIGHT, MFL_H1_BC_TEMPERATURE, T0_REF, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, dt, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, T_END_REF, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    free(T);
    if (r.T == NULL) {
        mfl_h1_config_free(&c);
        return out;
    }
    for (s = 0; s < r.samples; s++) {
        if (fabs(r.times[s] - T_END_REF / 2) < fabs(r.times[mid] - T_END_REF / 2)) {
            mid = s;
        }
        for (j = 0; j < nodes; j++) {
            double e = fabs(r.T[s * nodes + j] - exact_sine(r.positions[j], r.times[s]));
            if (e > out.err_inf) {
                out.err_inf = e;
            }
        }
    }
    s = r.samples - 1;
    CHECK(r.times[s] == T_END_REF); /* the run ends exactly at end_time */
    out.err_quarter = fabs(r.T[s * nodes + (nodes - 1) / 4] - exact_sine(L_BAR / 4, T_END_REF));
    out.err_center = fabs(r.T[s * nodes + (nodes - 1) / 2] - exact_sine(L_BAR / 2, T_END_REF));
    out.mid_time = r.times[mid];
    out.err_mid_time = fabs(r.T[mid * nodes + (nodes - 1) / 2] - exact_sine(L_BAR / 2, r.times[mid]));
    mfl_h1_result_free(&r);
    mfl_h1_config_free(&c);
    return out;
}

static void test_analytical(int print_table)
{
    /* Meshes with a node at L/4 and L/2: dx = L/8, L/16, L/32. */
    SineRun r[3];
    size_t nodes[3] = {9, 17, 33};
    /* Tolerances (K) for the maximum error over all nodes and samples, for
     * A = 50 K. FTCS is O(dt) + O(dx^2); with Fo fixed both terms scale as
     * dx^2, so each halving of dx must divide the error by about 4. The
     * bounds are about 1.2 x the measured errors (documented in
     * docs/heat-conduction.md). */
    double tol_inf[3] = {0.14, 0.035, 0.0087};
    int i;

    for (i = 0; i < 3; i++) {
        r[i] = run_sine(nodes[i]);
        CHECK(r[i].err_inf < tol_inf[i]);
        CHECK(r[i].err_quarter <= r[i].err_inf && r[i].err_center <= r[i].err_inf);
    }
    for (i = 1; i < 3; i++) {
        double order = log(r[i - 1].err_inf / r[i].err_inf) / log(2.0);
        /* observed order of convergence: 2 +- 0.1 */
        CHECK(order > 1.9 && order < 2.1);
        order = log(r[i - 1].err_center / r[i].err_center) / log(2.0);
        CHECK(order > 1.9 && order < 2.1);
    }
    if (print_table) {
        printf("| nodes | dx (mm) | dt (s) | max error, all nodes and samples (K) | x = L/4, t = %g s (K) | x = L/2, t = %g s (K) | x = L/2, t = %.4g s (K) | observed order (max error) |\n",
               T_END_REF, T_END_REF, r[0].mid_time);
        printf("|---|---|---|---|---|---|---|---|\n");
        for (i = 0; i < 3; i++) {
            char ord[32] = "-";
            if (i > 0) {
                snprintf(ord, sizeof ord, "%.3f", log(r[i - 1].err_inf / r[i].err_inf) / log(2.0));
            }
            printf("| %lu | %.4g | %.6g | %.3e | %.3e | %.3e | %.3e | %s |\n",
                   (unsigned long)r[i].nodes, 1000.0 * L_BAR / (double)(r[i].nodes - 1), r[i].dt,
                   r[i].err_inf, r[i].err_quarter, r[i].err_center, r[i].err_mid_time, ord);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* 2. Insulated ends                                                         */
/* ------------------------------------------------------------------------ */

static void insulated_bar(MflH1Config *c, size_t nodes, double dt, double t_end)
{
    char msg[512];
    bar(c, L_BAR);
    CHECK(mfl_h1_set_nodes(c, (double)nodes, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(c, MFL_H1_LEFT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(c, MFL_H1_RIGHT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_TIME_STEP, dt, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_END_TIME, t_end, NULL, msg, sizeof msg) == MFL_H1_OK);
}

static void test_insulated(void)
{
    MflH1Config c;
    MflH1Result r;
    char msg[512];
    size_t s;
    size_t j;
    double dt_max = 0;

    /* Uniform temperature stays uniform, exactly (T + 2 Fo (T - T) = T). */
    insulated_bar(&c, 21, 1.0, 600.0);
    CHECK(mfl_h1_set_initial_uniform(&c, 473.15, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    for (s = 0; s < r.samples; s++) {
        for (j = 0; j < r.nodes; j++) {
            CHECK(r.T[s * r.nodes + j] == 473.15);
        }
    }
    mfl_h1_result_free(&r);
    mfl_h1_config_free(&c);

    /* Non-uniform profile: the discrete energy dx (T0/2 + T1 + ... + T_{N-1}/2)
     * is conserved by the scheme (the sum of the updates telescopes to
     * zero). Tolerance: 1e-13 relative (rounding only) over 20000 steps,
     * both at the stability limit (Fo = 1/2) and at Fo = 0.4.
     *
     * At Fo < 1/2 the profile tends to the uniform temperature E / L:
     * 20000 steps at Fo = 0.4 are t = 5 L^2 / alpha, where the slowest mode
     * has decayed by exp(-5 pi^2) = 4e-22. At Fo = 1/2 exactly, the
     * shortest wave on the mesh (alternating +-, k dx = pi) is amplified by
     * 1 - 4 Fo = -1 per step: it neither grows nor decays, so the profile
     * oscillates around the mean instead of converging (documented in
     * docs/heat-conduction.md). */
    {
        double fos[2] = {0.5, 0.4};
        double T[41];
        int f;
        for (j = 0; j < 41; j++) {
            double x = (double)j / 40.0;
            T[j] = 300.0 + 400.0 * x * x * (1.0 - 0.5 * x) + (j == 10 ? 75.0 : 0.0);
        }
        for (f = 0; f < 2; f++) {
            double e0;
            double mean;
            double spread = 0;
            insulated_bar(&c, 41, 1.0, 1.0);
            CHECK(mfl_h1_max_time_step(&c, &dt_max, msg, sizeof msg) == MFL_H1_OK);
            CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 2 * fos[f] * dt_max, NULL, msg, sizeof msg) == MFL_H1_OK);
            CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 20000 * 2 * fos[f] * dt_max, NULL, msg, sizeof msg) == MFL_H1_OK);
            CHECK(mfl_h1_set_sample_every(&c, 2000, msg, sizeof msg) == MFL_H1_OK);
            CHECK(mfl_h1_set_initial_profile(&c, T, 41, msg, sizeof msg) == MFL_H1_OK);
            CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
            CHECK(r.steps == 20000 && r.samples == 11);
            CHECK(fabs(r.fourier - fos[f]) <= 1e-15);
            e0 = mfl_h1_discrete_energy(r.T, r.nodes, r.dx);
            CHECK(fabs(e0 - mfl_h1_discrete_energy(T, 41, r.dx)) <= 1e-15 * e0);
            for (s = 1; s < r.samples; s++) {
                double e = mfl_h1_discrete_energy(r.T + s * r.nodes, r.nodes, r.dx);
                CHECK(fabs(e - e0) <= 1e-13 * e0);
            }
            mean = e0 / L_BAR;
            s = r.samples - 1;
            for (j = 0; j < r.nodes; j++) {
                double d = fabs(r.T[s * r.nodes + j] - mean);
                spread = d > spread ? d : spread;
            }
            if (f == 0) {
                CHECK(spread > 0.1); /* the alternating mode is still there (K) */
            } else {
                CHECK(spread < 1e-9); /* uniform at the mean (K) */
            }
            mfl_h1_result_free(&r);
            mfl_h1_config_free(&c);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* 3. Stability limit: Fo <= 1/2, equality accepted                          */
/* ------------------------------------------------------------------------ */

static void test_stability(void)
{
    MflH1Config c;
    MflH1Result r;
    char msg[600];
    double dt_max = 0;
    double dx = L_BAR / 20.0;
    double expected = dx * dx / (2.0 * alpha_bar());

    insulated_bar(&c, 21, 1.0, 10.0);
    CHECK(mfl_h1_set_initial_uniform(&c, 300.0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_max_time_step(&c, &dt_max, msg, sizeof msg) == MFL_H1_OK);
    CHECK(fabs(dt_max - expected) <= 1e-15 * expected);

    /* exactly at the limit: accepted, and its Fourier number is 1/2 */
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, dt_max, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(fabs(r.fourier - 0.5) <= 1e-15);
    mfl_h1_result_free(&r);

    /* within the documented relative tolerance (1e-12): accepted */
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, dt_max * (1 + 5e-13), NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    mfl_h1_result_free(&r);

    /* just above: rejected, with the requested and the maximum step */
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, dt_max * (1 + 1e-9), NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_UNSTABLE);
    CHECK(strstr(msg, "unstable time step") != NULL);
    CHECK(strstr(msg, "time_step <= dx^2 / (2 alpha) = ") != NULL);
    CHECK(r.T == NULL);
    {
        /* the maximum printed in the message reads back as an accepted step */
        const char *p = strstr(msg, "(2 alpha) = ");
        double printed = p != NULL ? strtod(p + 12, NULL) : 0;
        CHECK(printed == dt_max);
    }

    /* an unstable scheme really diverges: run it without the check */
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 2 * dt_max, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_UNSTABLE);
    CHECK(strstr(msg, "Fourier number alpha*dt/dx^2 = 1 > 1/2") != NULL);
    mfl_h1_config_free(&c);
}

/* ------------------------------------------------------------------------ */
/* 4. End time between steps, sampling                                       */
/* ------------------------------------------------------------------------ */

static void left_hot(MflH1Config *c, double dt, double t_end)
{
    char msg[512];
    bar(c, L_BAR);
    CHECK(mfl_h1_set_nodes(c, 11, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_initial_uniform(c, 293.15, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(c, MFL_H1_LEFT, MFL_H1_BC_TEMPERATURE, 773.15, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(c, MFL_H1_RIGHT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_TIME_STEP, dt, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(c, MFL_H1_P_END_TIME, t_end, NULL, msg, sizeof msg) == MFL_H1_OK);
}

static void test_end_time(void)
{
    MflH1Config c;
    MflH1Result a;
    MflH1Result b;
    char msg[512];
    size_t j;

    /* t_end = 10.5 dt: 10 full steps and a last step of 0.5 dt, ending
     * exactly at t_end. Reference: the 10-step run followed by one update
     * with half the Fourier number, written out here. */
    left_hot(&c, 1.0, 10.5);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.steps == 11 && a.last_step == 0.5);
    CHECK(a.samples == 12 && a.times[a.samples - 1] == 10.5 && a.times[10] == 10.0);
    mfl_h1_config_free(&c);
    left_hot(&c, 1.0, 10.0);
    CHECK_MSG(mfl_h1_run(&c, &b, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(b.steps == 10 && b.last_step == 1.0 && b.times[b.samples - 1] == 10.0);
    {
        const double *u = b.T + 10 * b.nodes;
        const double *got = a.T + 11 * a.nodes;
        double fo = b.alpha * 0.5 / (b.dx * b.dx);
        size_t n = b.nodes;
        CHECK(got[0] == 773.15); /* prescribed */
        for (j = 1; j + 1 < n; j++) {
            CHECK(got[j] == u[j] + fo * (u[j - 1] - 2.0 * u[j] + u[j + 1]));
        }
        CHECK(got[n - 1] == u[n - 1] + 2.0 * fo * (u[n - 2] - u[n - 1])); /* insulated */
        for (j = 0; j < 10 * n; j++) {
            CHECK(a.T[j] == b.T[j]); /* identical up to t = 10 s */
        }
    }
    mfl_h1_result_free(&a);
    mfl_h1_result_free(&b);
    mfl_h1_config_free(&c);

    /* 0.3 / 0.1 is 2.9999999999999996 in binary: 3 steps, the last one
     * 0.09999999999999998 s, ending exactly at 0.3 s. */
    left_hot(&c, 0.1, 0.3);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.steps == 3 && a.times[a.samples - 1] == 0.3);
    mfl_h1_result_free(&a);
    mfl_h1_config_free(&c);

    /* 1.0 / 0.1 = 10 (rounded): a remainder below 1e-9 dt is not a step. */
    left_hot(&c, 0.1, 1.0);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.steps == 10 && a.times[a.samples - 1] == 1.0);
    mfl_h1_result_free(&a);
    mfl_h1_config_free(&c);

    /* end_time shorter than one step: a single step of end_time */
    left_hot(&c, 1.0, 0.25);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.steps == 1 && a.samples == 2 && a.last_step == 0.25 && a.times[1] == 0.25);
    mfl_h1_result_free(&a);
    mfl_h1_config_free(&c);

    /* sampling every 4 steps of 10: t = 0, 4, 8 and the final state 10 */
    left_hot(&c, 1.0, 10.0);
    CHECK(mfl_h1_set_sample_every(&c, 4, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.samples == 4);
    CHECK(a.times[0] == 0 && a.times[1] == 4 && a.times[2] == 8 && a.times[3] == 10);
    CHECK(a.sample_step[1] == 4 && a.sample_step[3] == 10);
    /* initial state: prescribed boundary applied at t = 0 */
    CHECK(a.T[0] == 773.15 && a.T[1] == 293.15 && a.T[10] == 293.15);
    /* positions: uniform, last one exactly L */
    CHECK(a.positions[0] == 0 && a.positions[10] == L_BAR);
    for (j = 0; j < 11; j++) {
        CHECK(fabs(a.positions[j] - L_BAR * (double)j / 10.0) <= 1e-16); /* m, rounding of j*dx */
    }
    {
        size_t s = 99;
        CHECK(mfl_h1_first_sample_reaching(&a, 0, 773.15, &s) && s == 0);
        CHECK(mfl_h1_first_sample_reaching(&a, 1, 300.0, &s) && s == 1);
        CHECK(!mfl_h1_first_sample_reaching(&a, 10, 773.15, &s)); /* never within 10 s */
        CHECK(!mfl_h1_first_sample_reaching(&a, 11, 300.0, &s));  /* no such node */
    }
    mfl_h1_result_free(&a);
    mfl_h1_config_free(&c);

    /* every sample of a run with sample_every = 1 */
    left_hot(&c, 1.0, 3.0);
    CHECK_MSG(mfl_h1_run(&c, &a, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(a.samples == 4 && a.times[3] == 3.0);
    mfl_h1_result_free(&a);
    mfl_h1_config_free(&c);
}

/* ------------------------------------------------------------------------ */
/* 5. Invalid configurations: status and the parameter named                 */
/* ------------------------------------------------------------------------ */

static void test_invalid(void)
{
    MflH1Config c;
    MflH1Result r;
    char msg[600];
    double prof[5] = {300, 300, 0, 300, 300};

    mfl_h1_config_init(&c);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_CONDUCTIVITY, -45, "-45 W/(m*K)", msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strcmp(msg, "conductivity (thermal conductivity k) must be positive, found -45 W/(m*K)") == 0);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_LENGTH, NAN, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strcmp(msg, "length (length of the bar) must be finite, found nan") == 0);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_DENSITY, INFINITY, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "density") != NULL);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 0, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "time_step (time step) must be positive, found 0") != NULL);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, -1, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "end_time") != NULL);
    CHECK(mfl_h1_set_nodes(&c, 2, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "nodes (number of mesh nodes, both ends included) must be at least 3, found 2") != NULL);
    CHECK(mfl_h1_set_nodes(&c, 10.5, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "must be a whole number, found 10.5") != NULL);
    CHECK(mfl_h1_set_nodes(&c, 2e6, msg, sizeof msg) == MFL_H1_LIMIT);
    CHECK(mfl_h1_set_sample_every(&c, 0, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_initial_uniform(&c, -5, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "absolute temperature above 0 K") != NULL);
    CHECK(mfl_h1_set_initial_uniform(&c, NAN, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_initial_profile(&c, prof, 5, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "value 3 of the initial profile (node 2)") != NULL);
    CHECK(mfl_h1_set_initial_profile(&c, prof, 2, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_RIGHT, MFL_H1_BC_TEMPERATURE, 0, NULL, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "right (x = L) boundary temperature") != NULL);
    CHECK(c.set == 0); /* nothing invalid was stored */

    /* missing parameters, in order */
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strcmp(msg, "missing parameter 'length' (length of the bar)") == 0);
    mfl_h1_config_free(&c);
    bar(&c, L_BAR);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "missing parameter 'initial'") != NULL);
    CHECK(mfl_h1_set_initial_uniform(&c, 300, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_LEFT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_RIGHT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 0.1, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 1, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "missing parameter 'nodes'") != NULL);
    /* profile and mesh disagree */
    CHECK(mfl_h1_set_nodes(&c, 7, msg, sizeof msg) == MFL_H1_OK);
    prof[2] = 310;
    CHECK(mfl_h1_set_initial_profile(&c, prof, 5, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_INVALID);
    CHECK(strstr(msg, "the initial profile has 5 values, but the mesh has 7 nodes") != NULL);
    /* nodes taken from the profile when not set */
    c.set &= ~(1u << MFL_H1_P_NODES);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(r.nodes == 5);
    mfl_h1_result_free(&r);

    /* limits: steps, stored values, memory */
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 1e9, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_LIMIT);
    CHECK(strstr(msg, "steps") != NULL && strstr(msg, "limit") != NULL);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 1e6, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_nodes(&c, 1000, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_initial_uniform(&c, 300, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 0.001, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 1, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_LENGTH, 100, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_sample_every(&c, 1, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg); /* 1001 x 1000 values */
    mfl_h1_result_free(&r);
    CHECK(mfl_h1_set_nodes(&c, 30000, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_LENGTH, 10000, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_LIMIT);
    CHECK(strstr(msg, "increase sample_every") != NULL);
    CHECK(mfl_h1_set_nodes(&c, 1000, msg, sizeof msg) == MFL_H1_OK);
    c.memory_limit = 100000;
    CHECK(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_NO_MEMORY);
    CHECK(strstr(msg, "above the memory limit of 100000 bytes") != NULL);
    mfl_h1_config_free(&c);

    /* a degenerate configuration whose limit is not finite */
    bar(&c, 1e300);
    CHECK(mfl_h1_set_nodes(&c, 3, msg, sizeof msg) == MFL_H1_OK);
    {
        double dt = 0;
        CHECK(mfl_h1_max_time_step(&c, &dt, msg, sizeof msg) == MFL_H1_INVALID);
        CHECK(strstr(msg, "not a positive finite number") != NULL);
    }
    mfl_h1_config_free(&c);
}

/* ------------------------------------------------------------------------ */
/* 6. Physics sanity: steady state of a bar heated at one end                */
/* ------------------------------------------------------------------------ */

static void test_steady_state(void)
{
    MflH1Config c;
    MflH1Result r;
    char msg[512];
    size_t j;
    /* both ends prescribed: the steady state is linear; after 20 diffusion
     * times (t = 20 L^2 / alpha) the transient is below 1e-6 K. */
    bar(&c, L_BAR);
    CHECK(mfl_h1_set_nodes(&c, 11, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_initial_uniform(&c, 300, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_LEFT, MFL_H1_BC_TEMPERATURE, 400, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_boundary(&c, MFL_H1_RIGHT, MFL_H1_BC_TEMPERATURE, 300, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 2.0, NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 20 * L_BAR * L_BAR / alpha_bar(), NULL, msg, sizeof msg) == MFL_H1_OK);
    CHECK(mfl_h1_set_sample_every(&c, 1000000, msg, sizeof msg) == MFL_H1_OK);
    CHECK_MSG(mfl_h1_run(&c, &r, msg, sizeof msg) == MFL_H1_OK, msg);
    CHECK(r.samples == 2);
    for (j = 0; j < 11; j++) {
        CHECK(fabs(r.T[11 + j] - (400.0 - 10.0 * (double)j)) < 1e-6);
    }
    mfl_h1_result_free(&r);
    mfl_h1_config_free(&c);
}

int main(int argc, char **argv)
{
    test_analytical(argc > 1 && strcmp(argv[1], "--table") == 0);
    test_insulated();
    test_stability();
    test_end_time();
    test_invalid();
    test_steady_state();
    printf("test_heat1d: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
