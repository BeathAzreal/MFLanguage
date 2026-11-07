/*
 * welding.h - thermal models of arc welding (v0.4): Rosenthal's moving
 * heat sources, cooling time t8/5, peak temperatures, and the empirical
 * t8/5 formulas of EN 1011-2 for steels. See docs/welding.md.
 *
 * Coherent SI and plain doubles (W, m/s, m, W/(m*K), m^2/s, K or degC
 * where noted); units are checked by the callers (the `welding` module of
 * the language). Coordinates move with the source: the arc travels in +x
 * at speed v and sits at the origin, so points behind it have x < 0; y is
 * across the weld and z into the plate (thick plate, surface z = 0).
 *
 * The models assume a quasi-steady state, constant properties, no latent
 * heat and no losses from the surfaces (Rosenthal 1946). They describe the
 * heat-affected zone well away from the arc; they are singular at the
 * source and do not predict the shape of the weld pool.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NUMERICAL_WELDING_H
#define MFL_NUMERICAL_WELDING_H

#include <stddef.h>

typedef enum {
    MFL_W_OK = 0,
    MFL_W_INVALID /* a value outside its domain; the message names it */
} MflWStatus;

/* Thermal efficiency factor k of a process, EN 1011-1:2009 Table 1
 * (also ISO/TR 17671-1): "SAW" 1.0; "SMAW", "GMAW", "MIG", "MAG", "FCAW"
 * 0.8; "GTAW", "TIG", "PAW", "plasma" 0.6. Case-insensitive. Returns -1
 * for an unknown name. */
double mfl_w_efficiency(const char *process);

/* Heat input Q = k * U * I / v (J/m). */
double mfl_w_heat_input(double k, double voltage, double current, double speed);

/* Modified Bessel functions of the second kind K0, K1, scaled by e^x
 * (e^x K0(x), e^x K1(x)), x > 0. Polynomial approximations of Abramowitz &
 * Stegun 9.8.1-9.8.8 (relative error below 2e-7). */
double mfl_w_k0_scaled(double x);
double mfl_w_k1_scaled(double x);

/* Temperature (same unit as T0, K or degC) of the steady Rosenthal fields:
 *   thick plate, point source on the surface of a semi-infinite body:
 *     T - T0 = q / (2 pi k R) exp(-v (x + R) / (2 a)),  R = |(x, y, z)|
 *   thin plate of thickness d, line source through the thickness:
 *     T - T0 = q / (2 pi k d) exp(-v x / (2 a)) K0(v r / (2 a)),  r = |(x, y)|
 * q: net arc power (W), a: thermal diffusivity (m^2/s). +inf at the source. */
double mfl_w_rosenthal_thick(double q, double v, double k, double a, double T0, double x,
                             double y, double z);
double mfl_w_rosenthal_thin(double q, double v, double d, double k, double a, double T0,
                            double x, double y);

/* Cooling time from 800 to 500 degC on the weld centre line (Rosenthal):
 *   thick: t85 = Q / (2 pi k) [1/(500 - T0) - 1/(800 - T0)]
 *   thin:  t85 = Q^2 / (4 pi k rho_c d^2) [1/(500 - T0)^2 - 1/(800 - T0)^2]
 * Q: heat input (J/m), rho_c: volumetric heat capacity rho*c (J/(m^3*K)),
 * T0: preheat in degC (< 500). The thin-plate form is the large-distance
 * limit of the K0 field (error measured in tests/numerical/test_welding.c). */
MflWStatus mfl_w_t85_thick(double Q, double k, double T0c, double *t85, char *msg, size_t n);
MflWStatus mfl_w_t85_thin(double Q, double d, double k, double rho_c, double T0c, double *t85,
                          char *msg, size_t n);

/* Peak temperature reached at lateral distance y > 0 from the weld centre
 * line (thick: on the surface), found by maximising the Rosenthal field
 * along a line parallel to the weld (exact for the model, not Adams'
 * approximation). */
MflWStatus mfl_w_peak_thick(double q, double v, double k, double a, double T0, double y,
                            double *Tp, char *msg, size_t n);
MflWStatus mfl_w_peak_thin(double q, double v, double d, double k, double a, double T0, double y,
                           double *Tp, char *msg, size_t n);

/* Lateral distance y at which the peak temperature equals Tpeak (> T0):
 * half-width of the fused zone for Tpeak = melting temperature, outer edge
 * of the heat-affected zone for Tpeak = A1, and so on. thin: d > 0;
 * thick: d = 0. */
MflWStatus mfl_w_distance_for_peak(double q, double v, double d, double k, double a, double T0,
                                   double Tpeak, double *y, char *msg, size_t n);

/* EN 1011-2:2001 Annex D, t8/5 of steels (empirical), heat input Q in
 * kJ/mm, thickness d in mm, preheat T0 in degC, shape factors F2 / F3
 * (1 for a butt weld's single pass; see the standard for others):
 *   3-D: t85 = (6700 - 5 T0) Q [1/(500 - T0) - 1/(800 - T0)] F3
 *   2-D: t85 = (4300 - 4.3 T0) 1e5 Q^2/d^2 [1/(500-T0)^2 - 1/(800-T0)^2] F2
 * The plate is "thick" (3-D) above the transition thickness
 *   d_t = sqrt((4300 - 4.3 T0) 1e5 Q [..]^2-term / ((6700 - 5 T0) [..]-term))
 * where both formulas agree. *mode is set to 2 or 3. */
MflWStatus mfl_w_t85_en1011(double Q_kJmm, double d_mm, double T0c, double F2, double F3,
                            double *t85, int *mode, double *d_transition_mm, char *msg, size_t n);

#endif /* MFL_NUMERICAL_WELDING_H */
