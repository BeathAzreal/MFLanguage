/*
 * welding.c - Rosenthal fields, t8/5, peak temperatures and EN 1011-2.
 * See welding.h and docs/welding.md.
 *
 * Peak temperatures: along a line parallel to the weld at lateral distance
 * y, the steady Rosenthal field has a single maximum behind the arc. With
 * u = v x / (2a), w = v y / (2a), rho = sqrt(u^2 + w^2), the maximum is the
 * root of d/du ln T = 0:
 *   thick:  1 + u/rho + u/rho^2 = 0
 *   thin:  -1 - (K1(rho)/K0(rho)) u/rho = 0
 * found by bisection on a bracket in u < 0 (no step size to tune).
 * Exponentials are evaluated as exp(-(u + rho)) with scaled Bessel
 * functions, so nothing overflows far behind the arc.
 *
 * SPDX-License-Identifier: MIT
 */
#include "welding.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const double PI = 3.14159265358979323846;

static MflWStatus fail(char *msg, size_t n, const char *fmt, ...)
{
    va_list ap;
    if (msg != NULL && n > 0) {
        va_start(ap, fmt);
        vsnprintf(msg, n, fmt, ap);
        va_end(ap);
    }
    return MFL_W_INVALID;
}

#define POSITIVE(x, name)                                                                   \
    do {                                                                                    \
        if (!(isfinite(x) && (x) > 0)) {                                                    \
            return fail(msg, n, "%s must be positive and finite, found %g", (name), (x));   \
        }                                                                                   \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Efficiency and heat input                                                 */
/* ------------------------------------------------------------------------ */

static int same(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

double mfl_w_efficiency(const char *p)
{
    static const struct {
        const char *name;
        double      k;
    } table[] = {{"SAW", 1.0},  {"SMAW", 0.8}, {"MMA", 0.8}, {"GMAW", 0.8},   {"MIG", 0.8},
                 {"MAG", 0.8},  {"FCAW", 0.8}, {"GTAW", 0.6}, {"TIG", 0.6},   {"PAW", 0.6},
                 {"plasma", 0.6}};
    size_t i;
    if (p == NULL) {
        return -1;
    }
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (same(p, table[i].name)) {
            return table[i].k;
        }
    }
    return -1;
}

double mfl_w_heat_input(double k, double voltage, double current, double speed)
{
    return k * voltage * current / speed;
}

/* ------------------------------------------------------------------------ */
/* Bessel functions (Abramowitz & Stegun 9.8.1-9.8.8)                         */
/* ------------------------------------------------------------------------ */

static double i0_small(double x) /* |x| <= 3.75 */
{
    double t = (x / 3.75) * (x / 3.75);
    return 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 +
           t * (0.0360768 + t * 0.0045813)))));
}

static double i1_small(double x) /* |x| <= 3.75 */
{
    double t = (x / 3.75) * (x / 3.75);
    return x * (0.5 + t * (0.87890594 + t * (0.51498869 + t * (0.15084934 + t * (0.02658733 +
           t * (0.00301532 + t * 0.00032411))))));
}

double mfl_w_k0_scaled(double x)
{
    if (x <= 2.0) {
        double t = (x / 2.0) * (x / 2.0);
        double k0 = -log(x / 2.0) * i0_small(x) - 0.57721566 +
                    t * (0.42278420 + t * (0.23069756 + t * (0.03488590 + t * (0.00262698 +
                    t * (0.00010750 + t * 0.00000740)))));
        return k0 * exp(x);
    } else {
        double t = 2.0 / x;
        return (1.25331414 + t * (-0.07832358 + t * (0.02189568 + t * (-0.01062446 +
                t * (0.00587872 + t * (-0.00251540 + t * 0.00053208)))))) / sqrt(x);
    }
}

double mfl_w_k1_scaled(double x)
{
    if (x <= 2.0) {
        double t = (x / 2.0) * (x / 2.0);
        double xk1 = x * log(x / 2.0) * i1_small(x) + 1.0 +
                     t * (0.15443144 + t * (-0.67278579 + t * (-0.18156897 + t * (-0.01919402 +
                     t * (-0.00110404 + t * -0.00004686)))));
        return xk1 / x * exp(x);
    } else {
        double t = 2.0 / x;
        return (1.25331414 + t * (0.23498619 + t * (-0.03655620 + t * (0.01504268 +
                t * (-0.00780353 + t * (0.00325614 + t * -0.00068245)))))) / sqrt(x);
    }
}

/* ------------------------------------------------------------------------ */
/* Rosenthal fields                                                          */
/* ------------------------------------------------------------------------ */

double mfl_w_rosenthal_thick(double q, double v, double k, double a, double T0, double x,
                             double y, double z)
{
    double R = sqrt(x * x + y * y + z * z);
    if (R == 0) {
        return INFINITY;
    }
    return T0 + q / (2 * PI * k * R) * exp(-v * (x + R) / (2 * a));
}

double mfl_w_rosenthal_thin(double q, double v, double d, double k, double a, double T0,
                            double x, double y)
{
    double r = sqrt(x * x + y * y);
    double s = v / (2 * a);
    if (r == 0) {
        return INFINITY;
    }
    return T0 + q / (2 * PI * k * d) * exp(-s * (x + r)) * mfl_w_k0_scaled(s * r);
}

/* ------------------------------------------------------------------------ */
/* t8/5                                                                      */
/* ------------------------------------------------------------------------ */

static MflWStatus check_preheat(double T0c, char *msg, size_t n)
{
    if (!isfinite(T0c) || T0c >= 500) {
        return fail(msg, n, "the preheat/initial temperature must be below 500 degC for t8/5, "
                            "found %g degC", T0c);
    }
    if (T0c < -273.15) {
        return fail(msg, n, "the preheat temperature %g degC is below absolute zero", T0c);
    }
    return MFL_W_OK;
}

MflWStatus mfl_w_t85_thick(double Q, double k, double T0c, double *t85, char *msg, size_t n)
{
    MflWStatus st;
    POSITIVE(Q, "heat_input");
    POSITIVE(k, "conductivity");
    if ((st = check_preheat(T0c, msg, n)) != MFL_W_OK) {
        return st;
    }
    *t85 = Q / (2 * PI * k) * (1 / (500 - T0c) - 1 / (800 - T0c));
    return MFL_W_OK;
}

MflWStatus mfl_w_t85_thin(double Q, double d, double k, double rho_c, double T0c, double *t85,
                          char *msg, size_t n)
{
    MflWStatus st;
    double b5;
    double b8;
    POSITIVE(Q, "heat_input");
    POSITIVE(d, "thickness");
    POSITIVE(k, "conductivity");
    POSITIVE(rho_c, "density * specific_heat");
    if ((st = check_preheat(T0c, msg, n)) != MFL_W_OK) {
        return st;
    }
    b5 = 1 / (500 - T0c);
    b8 = 1 / (800 - T0c);
    *t85 = Q * Q / (4 * PI * k * rho_c * d * d) * (b5 * b5 - b8 * b8);
    return MFL_W_OK;
}

/* ------------------------------------------------------------------------ */
/* Peak temperatures                                                         */
/* ------------------------------------------------------------------------ */

/* d/du ln T along the line, thick (g) and thin (h); both change sign once in u < 0. */
static double slope_thick(double u, double w)
{
    double rho = sqrt(u * u + w * w);
    return -(1 + u / rho + u / (rho * rho));
}

static double slope_thin(double u, double w)
{
    double rho = sqrt(u * u + w * w);
    return -1 - mfl_w_k1_scaled(rho) / mfl_w_k0_scaled(rho) * (u / rho);
}

/* Position u* < 0 of the maximum: the slope is negative at u = 0 (thick:
 * -(1)... ) and positive far behind the arc. */
static double argmax(double (*slope)(double, double), double w)
{
    double hi = 0.0;
    double lo = -1.0;
    int i;
    while (slope(lo, w) <= 0 && lo > -1e12) {
        hi = lo;
        lo *= 2;
    }
    for (i = 0; i < 200; i++) {
        double mid = 0.5 * (lo + hi);
        if (slope(mid, w) > 0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

static MflWStatus check_field(double q, double v, double k, double a, double y, char *msg, size_t n)
{
    POSITIVE(q, "power");
    POSITIVE(v, "speed");
    POSITIVE(k, "conductivity");
    POSITIVE(a, "diffusivity");
    POSITIVE(y, "distance");
    return MFL_W_OK;
}

MflWStatus mfl_w_peak_thick(double q, double v, double k, double a, double T0, double y,
                            double *Tp, char *msg, size_t n)
{
    MflWStatus st = check_field(q, v, k, a, y, msg, n);
    double s;
    double u;
    if (st != MFL_W_OK) {
        return st;
    }
    s = v / (2 * a);
    u = argmax(slope_thick, s * y);
    *Tp = mfl_w_rosenthal_thick(q, v, k, a, T0, u / s, y, 0.0);
    return MFL_W_OK;
}

MflWStatus mfl_w_peak_thin(double q, double v, double d, double k, double a, double T0, double y,
                           double *Tp, char *msg, size_t n)
{
    MflWStatus st = check_field(q, v, k, a, y, msg, n);
    double s;
    double u;
    if (st != MFL_W_OK) {
        return st;
    }
    POSITIVE(d, "thickness");
    s = v / (2 * a);
    u = argmax(slope_thin, s * y);
    *Tp = mfl_w_rosenthal_thin(q, v, d, k, a, T0, u / s, y);
    return MFL_W_OK;
}

static double peak(double q, double v, double d, double k, double a, double T0, double y)
{
    double Tp = 0;
    if (d > 0) {
        mfl_w_peak_thin(q, v, d, k, a, T0, y, &Tp, NULL, 0);
    } else {
        mfl_w_peak_thick(q, v, k, a, T0, y, &Tp, NULL, 0);
    }
    return Tp;
}

MflWStatus mfl_w_distance_for_peak(double q, double v, double d, double k, double a, double T0,
                                   double Tpeak, double *y, char *msg, size_t n)
{
    MflWStatus st = check_field(q, v, k, a, 1.0, msg, n);
    double lo = 1e-9;
    double hi = 1e-3;
    int i;
    if (st != MFL_W_OK) {
        return st;
    }
    if (!(d >= 0) || !isfinite(d)) {
        return fail(msg, n, "thickness must be positive (thin plate) or 0 (thick plate), found %g", d);
    }
    if (!(Tpeak > T0) || !isfinite(Tpeak)) {
        return fail(msg, n, "the peak temperature must be above the initial temperature");
    }
    if (peak(q, v, d, k, a, T0, lo) < Tpeak) {
        return fail(msg, n, "the peak temperature is not reached even 1 nm from the weld centre "
                            "line");
    }
    while (peak(q, v, d, k, a, T0, hi) >= Tpeak) {
        lo = hi;
        hi *= 2;
        if (hi > 1e6) {
            return fail(msg, n, "the peak temperature is reached everywhere (check the inputs)");
        }
    }
    for (i = 0; i < 200; i++) {
        double mid = sqrt(lo * hi); /* bisection on a log scale */
        if (peak(q, v, d, k, a, T0, mid) >= Tpeak) {
            lo = mid;
        } else {
            hi = mid;
        }
        if (hi / lo - 1 < 1e-15) {
            break;
        }
    }
    *y = sqrt(lo * hi);
    return MFL_W_OK;
}

/* ------------------------------------------------------------------------ */
/* EN 1011-2 (steels)                                                        */
/* ------------------------------------------------------------------------ */

MflWStatus mfl_w_t85_en1011(double Q, double d, double T0, double F2, double F3, double *t85,
                            int *mode, double *dt, char *msg, size_t n)
{
    MflWStatus st;
    double A3;
    double B3;
    double A2;
    double B2;
    double t2;
    double t3;
    POSITIVE(Q, "heat_input");
    POSITIVE(d, "thickness");
    POSITIVE(F2, "shape factor F2");
    POSITIVE(F3, "shape factor F3");
    if ((st = check_preheat(T0, msg, n)) != MFL_W_OK) {
        return st;
    }
    A3 = 6700 - 5 * T0;
    B3 = 1 / (500 - T0) - 1 / (800 - T0);
    A2 = (4300 - 4.3 * T0) * 1e5;
    B2 = 1 / ((500 - T0) * (500 - T0)) - 1 / ((800 - T0) * (800 - T0));
    t3 = A3 * Q * B3 * F3;
    t2 = A2 * Q * Q / (d * d) * B2 * F2;
    *dt = sqrt(A2 * Q * B2 * F2 / (A3 * B3 * F3));
    if (d > *dt) {
        *t85 = t3;
        *mode = 3;
    } else {
        *t85 = t2;
        *mode = 2;
    }
    return MFL_W_OK;
}
