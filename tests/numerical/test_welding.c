/*
 * test_welding.c - verification of modules/numerical/welding.c against
 * references computed independently of the code under test:
 *
 *   - Bessel functions: e^x K0(x), e^x K1(x) from the integral
 *     K_n(x) = int_0^inf exp(-x cosh t) cosh(n t) dt (trapezoid rule,
 *     2e5 points, computed in Python), not from the A&S polynomials;
 *   - t8/5 closed forms: cooling times measured on the Rosenthal fields
 *     themselves (bisection for the 800 and 500 degC crossings);
 *   - peak temperatures: the classical far-field results (Rykalin):
 *     thick Tp - T0 = 2 Q / (pi e rho c y^2), thin Tp - T0 = Q / (sqrt(2 pi e) rho c d y);
 *   - EN 1011-2: values of the Annex D formulas computed separately.
 *
 * With --table, prints the comparison used in docs/welding.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "welding.h"

static int g_checks = 0;
static int g_failures = 0;
static int g_table = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CLOSE(got, want, rel)                                                                      \
    do {                                                                                           \
        double g_ = (got), w_ = (want);                                                            \
        g_checks++;                                                                                \
        if (!(fabs(g_ - w_) <= (rel) * fabs(w_))) {                                                \
            g_failures++;                                                                          \
            fprintf(stderr, "%s:%d: %s = %.17g, want %.17g (rel %.2e > %.1e)\n", __FILE__, __LINE__, \
                    #got, g_, w_, fabs(g_ - w_) / fabs(w_), (double)(rel));                         \
        }                                                                                          \
    } while (0)

static const double PI = 3.14159265358979323846;
static const double E = 2.71828182845904523536;

/* A GMAW pass on structural steel (properties typical of the 500-800 degC
 * range, as used in t8/5 estimates). */
#define Q_POWER 5000.0  /* W: 0.8 x 25 V x 250 A */
#define SPEED 0.005     /* m/s */
#define K_STEEL 30.0    /* W/(m*K) */
#define RHO_C 5.0e6     /* J/(m^3*K) */
#define ALPHA (K_STEEL / RHO_C)
#define T0 20.0         /* degC */

static void test_bessel(void)
{
    /* x, e^x K0(x), e^x K1(x): integral representation (see the header) */
    static const double ref[][3] = {
        {0.1, 2.682326102262782, 10.89018268304957},   {0.5, 1.5241093857738313, 2.731009708211685},
        {1, 1.1444630798068443, 1.636153486263182},    {2, 0.8415682150707325, 1.0334768470686344},
        {3, 0.6977615980438159, 0.806563480128736},    {5, 0.5478075643134644, 0.6002738587882565},
        {10, 0.3916319344365755, 0.410766570595762},   {20, 0.2785448766571723, 0.28542549694071384},
    };
    size_t i;
    for (i = 0; i < sizeof ref / sizeof ref[0]; i++) {
        /* A&S 9.8.5-9.8.8: absolute 1e-8 (small x), relative 2.2e-7 (x >= 2) */
        CLOSE(mfl_w_k0_scaled(ref[i][0]), ref[i][1], 3e-7);
        CLOSE(mfl_w_k1_scaled(ref[i][0]), ref[i][2], 3e-7);
    }
}

static void test_efficiency(void)
{
    CHECK(mfl_w_efficiency("SAW") == 1.0);
    CHECK(mfl_w_efficiency("smaw") == 0.8 && mfl_w_efficiency("MIG") == 0.8);
    CHECK(mfl_w_efficiency("mag") == 0.8 && mfl_w_efficiency("FCAW") == 0.8);
    CHECK(mfl_w_efficiency("TIG") == 0.6 && mfl_w_efficiency("GTAW") == 0.6);
    CHECK(mfl_w_efficiency("Plasma") == 0.6);
    CHECK(mfl_w_efficiency("laser") == -1 && mfl_w_efficiency(NULL) == -1);
    /* 0.8 x 20 V x 150 A / 5 mm/s = 480 J/mm */
    CLOSE(mfl_w_heat_input(0.8, 20, 150, 0.005), 480000.0, 1e-15);
}

/* Time (s) after which a point on the weld centre line, behind the arc,
 * has cooled to Tc: bisection on the field itself. */
static double crossing(int thin, double d, double Tc)
{
    double lo = -100.0, hi = -1e-7; /* x in m; T decreases as x goes back */
    int i;
    for (i = 0; i < 300; i++) {
        double mid = 0.5 * (lo + hi);
        double T = thin ? mfl_w_rosenthal_thin(Q_POWER, SPEED, d, K_STEEL, ALPHA, T0, mid, 1e-7)
                        : mfl_w_rosenthal_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, mid, 1e-7, 0);
        if (T > Tc) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return -0.5 * (lo + hi) / SPEED;
}

static void test_t85(void)
{
    double Q = Q_POWER / SPEED;
    double t = 0;
    char msg[200];
    double d;

    /* Thick plate: the closed form IS the centre-line field (exact). */
    CHECK(mfl_w_t85_thick(Q, K_STEEL, T0, &t, msg, sizeof msg) == MFL_W_OK);
    CLOSE(t, crossing(0, 0, 500) - crossing(0, 0, 800), 1e-6);
    CLOSE(t, Q / (2 * PI * K_STEEL) * (1.0 / 480 - 1.0 / 780), 1e-15);
    if (g_table) {
        printf("| plate | t8/5 closed form (s) | t8/5 from the Rosenthal field (s) | relative difference |\n|---|---|---|---|\n");
        printf("| thick | %.6g | %.6g | %.1e |\n", t, crossing(0, 0, 500) - crossing(0, 0, 800),
               fabs(t / (crossing(0, 0, 500) - crossing(0, 0, 800)) - 1));
    }

    /* Thin plate: the closed form uses K0's far-field limit. Measured
     * difference to the exact K0 field: at most 2.0e-4 for these plates
     * (3 to 12 mm; it shrinks as the plate gets thinner and the cooling
     * slower). Tolerance 5e-4. */
    for (d = 0.003; d <= 0.0121; d += 0.003) {
        double exact = crossing(1, d, 500) - crossing(1, d, 800);
        CHECK(mfl_w_t85_thin(Q, d, K_STEEL, RHO_C, T0, &t, msg, sizeof msg) == MFL_W_OK);
        CLOSE(t, exact, 5e-4);
        if (g_table) {
            printf("| thin, d = %g mm | %.6g | %.6g | %.1e |\n", d * 1000, t, exact, fabs(t / exact - 1));
        }
    }

    /* Invalid inputs name the parameter. */
    CHECK(mfl_w_t85_thick(Q, K_STEEL, 500, &t, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(strstr(msg, "below 500 degC") != NULL);
    CHECK(mfl_w_t85_thin(Q, 0, K_STEEL, RHO_C, T0, &t, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(strstr(msg, "thickness") != NULL);
    CHECK(mfl_w_t85_thick(-Q, K_STEEL, T0, &t, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(strstr(msg, "heat_input") != NULL);
    CHECK(mfl_w_t85_thick(Q, NAN, T0, &t, msg, sizeof msg) == MFL_W_INVALID);
}

static void test_peak(void)
{
    double Q = Q_POWER / SPEED;
    double Tp = 0;
    double prev = INFINITY;
    double y;
    char msg[200];

    /* Far from the weld, the classical results hold (Rykalin): ratio -> 1.
     * w = v y / (2 alpha) = 250 at y = 0.6 m; the tolerance covers the
     * next term of the expansion (order 1/w). */
    CHECK(mfl_w_peak_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, 0.6, &Tp, msg, sizeof msg) == MFL_W_OK);
    CLOSE(Tp - T0, 2 * Q / (PI * E * RHO_C * 0.6 * 0.6), 1e-2);
    CHECK(mfl_w_peak_thin(Q_POWER, SPEED, 0.005, K_STEEL, ALPHA, T0, 0.6, &Tp, msg, sizeof msg) == MFL_W_OK);
    CLOSE(Tp - T0, Q / (sqrt(2 * PI * E) * RHO_C * 0.005 * 0.6), 1e-2);
    if (g_table) {
        printf("\n| y (mm) | thick: peak (degC) | far-field 2Q/(pi e rho c y^2) | thin d = 5 mm: peak (degC) | far-field Q/(sqrt(2 pi e) rho c d y) |\n|---|---|---|---|---|\n");
        for (y = 0.002; y < 0.7; y *= 3) {
            double a = 0, b = 0;
            mfl_w_peak_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, y, &a, NULL, 0);
            mfl_w_peak_thin(Q_POWER, SPEED, 0.005, K_STEEL, ALPHA, T0, y, &b, NULL, 0);
            printf("| %.4g | %.1f | %.1f | %.1f | %.1f |\n", y * 1000, a,
                   T0 + 2 * Q / (PI * E * RHO_C * y * y), b, T0 + Q / (sqrt(2 * PI * E) * RHO_C * 0.005 * y));
        }
    }

    /* The peak decreases with the distance, and is a maximum of the field. */
    for (y = 0.0005; y < 0.05; y *= 1.5) {
        double x;
        CHECK(mfl_w_peak_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, y, &Tp, msg, sizeof msg) == MFL_W_OK);
        CHECK(Tp < prev);
        prev = Tp;
        for (x = -0.2; x < 0.01; x += 0.0007) {
            CHECK(mfl_w_rosenthal_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, x, y, 0) <= Tp * (1 + 1e-12));
        }
    }

    /* Inverse: the distance at which the peak equals T gives back T. */
    {
        double Ts[] = {1500, 1200, 727, 400};
        size_t i;
        for (i = 0; i < 4; i++) {
            double yy = 0;
            CHECK(mfl_w_distance_for_peak(Q_POWER, SPEED, 0, K_STEEL, ALPHA, T0, Ts[i], &yy, msg, sizeof msg) == MFL_W_OK);
            mfl_w_peak_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, yy, &Tp, NULL, 0);
            CLOSE(Tp, Ts[i], 1e-9);
            CHECK(mfl_w_distance_for_peak(Q_POWER, SPEED, 0.006, K_STEEL, ALPHA, T0, Ts[i], &yy, msg, sizeof msg) == MFL_W_OK);
            mfl_w_peak_thin(Q_POWER, SPEED, 0.006, K_STEEL, ALPHA, T0, yy, &Tp, NULL, 0);
            CLOSE(Tp, Ts[i], 1e-9);
        }
    }
    CHECK(mfl_w_distance_for_peak(Q_POWER, SPEED, 0, K_STEEL, ALPHA, T0, 10, &y, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(mfl_w_peak_thick(Q_POWER, SPEED, K_STEEL, ALPHA, T0, 0, &Tp, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(strstr(msg, "distance") != NULL);
    CHECK(mfl_w_peak_thin(Q_POWER, 0, 0.005, K_STEEL, ALPHA, T0, 0.01, &Tp, msg, sizeof msg) == MFL_W_INVALID);
    CHECK(strstr(msg, "speed") != NULL);
}

static void test_en1011(void)
{
    /* Q (kJ/mm), d (mm), T0 (degC), F2, F3 -> t85 (s), mode, d_t (mm): Annex D, computed separately */
    static const double ref[][8] = {
        {1.0, 20, 20, 1, 1, 5.2884615384615383, 3, 14.658605275561131},
        {1.0, 8, 20, 1, 1, 17.755572377342208, 2, 14.658605275561131},
        {1.5, 10, 100, 1, 1, 36.651466836734699, 2, 19.178851399753452},
        {0.5, 4, 20, 0.9, 0.67, 15.980015139607987, 2, 12.013277410886827},
    };
    size_t i;
    char msg[200];
    for (i = 0; i < sizeof ref / sizeof ref[0]; i++) {
        double t = 0, dt = 0;
        int mode = 0;
        CHECK(mfl_w_t85_en1011(ref[i][0], ref[i][1], ref[i][2], ref[i][3], ref[i][4], &t, &mode, &dt,
                               msg, sizeof msg) == MFL_W_OK);
        CLOSE(t, ref[i][5], 1e-14);
        CHECK(mode == (int)ref[i][6]);
        CLOSE(dt, ref[i][7], 1e-14);
    }
    /* Continuity at the transition thickness. */
    {
        double t2 = 0, t3 = 0, dt = 0;
        int m2 = 0, m3 = 0;
        mfl_w_t85_en1011(1.0, 14.658605275561131 * (1 - 1e-9), 20, 1, 1, &t2, &m2, &dt, msg, sizeof msg);
        mfl_w_t85_en1011(1.0, 14.658605275561131 * (1 + 1e-9), 20, 1, 1, &t3, &m3, &dt, msg, sizeof msg);
        CHECK(m2 == 2 && m3 == 3);
        CLOSE(t2, t3, 1e-8);
    }
    {
        double t = 0, dt = 0;
        int mode = 0;
        CHECK(mfl_w_t85_en1011(1.0, 10, 600, 1, 1, &t, &mode, &dt, msg, sizeof msg) == MFL_W_INVALID);
        CHECK(mfl_w_t85_en1011(1.0, -1, 20, 1, 1, &t, &mode, &dt, msg, sizeof msg) == MFL_W_INVALID);
    }
}

int main(int argc, char **argv)
{
    g_table = argc > 1 && strcmp(argv[1], "--table") == 0;
    test_bessel();
    test_efficiency();
    test_t85();
    test_peak();
    test_en1011();
    printf("test_welding: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
