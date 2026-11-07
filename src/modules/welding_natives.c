/*
 * welding_natives.c - the `welding` module (see welding_natives.h). Each
 * function converts its arguments to SI (temperatures to absolute K, or to
 * degC where the formula is written in degC), calls the numerics and
 * reports their messages as run-time errors at the call.
 *
 * SPDX-License-Identifier: MIT
 */
#include "modules/welding_natives.h"

#include <math.h>
#include <string.h>

#include "runtime/units.h"
#include "welding.h"

#define MSG 300

static MflDim dim(int m, int kg, int s, int k)
{
    MflDim d = mfl_dim_none();
    d.exp[0] = (signed char)m;
    d.exp[1] = (signed char)kg;
    d.exp[2] = (signed char)s;
    d.exp[4] = (signed char)k;
    return d;
}

#define SI(i) mfl_native_si(env, &a[i])
#define KELVIN(i) mfl_native_si(env, &a[i])               /* absolute temperature in K */
#define CELSIUS(i) (mfl_native_si(env, &a[i]) - MFL_DEGC_OFFSET)

static int ok_number(MflNativeEnv *env, MflWStatus st, const char *msg, double value, MflDim d,
                     MflValue *r)
{
    if (st != MFL_W_OK) {
        return mfl_native_fail(env, "welding.%s: %s", env->native->name, msg);
    }
    *r = mfl_native_number(value, d);
    return 0;
}

/* Material arguments: conductivity, density, specific heat -> k, a, rho_c. */
static int material(MflNativeEnv *env, const MflValue *a, int i, double *k, double *alpha,
                    double *rho_c)
{
    *alpha = 0;
    *k = SI(i);
    *rho_c = SI(i + 1) * SI(i + 2);
    if (!(*k > 0 && *rho_c > 0 && isfinite(*k) && isfinite(*rho_c))) {
        return mfl_native_fail(env, "welding.%s: conductivity, density and specific_heat must be "
                                    "positive and finite", env->native->name);
    }
    *alpha = *k / *rho_c;
    return 0;
}

int mfl_welding_efficiency(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char name[32];
    double k;
    mfl_native_string(&a[0], name, sizeof name);
    k = mfl_w_efficiency(name);
    if (k < 0) {
        return mfl_native_fail(env, "welding.efficiency: unknown process \"%s\" (known: SAW, "
                                    "SMAW, MMA, GMAW, MIG, MAG, FCAW, GTAW, TIG, PAW, plasma)",
                               name);
    }
    *r = mfl_native_number(k, mfl_dim_none());
    return 0;
}

int mfl_welding_heat_input(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    double U = SI(0), I = SI(1), v = SI(2), k = a[3].as.number;
    if (!(v > 0) || !isfinite(v)) {
        return mfl_native_fail(env, "welding.heat_input: speed must be positive and finite");
    }
    if (!(k > 0 && k <= 1)) {
        return mfl_native_fail(env, "welding.heat_input: efficiency must be between 0 and 1, found %g", k);
    }
    *r = mfl_native_number(mfl_w_heat_input(k, U, I, v), dim(1, 1, -2, 0)); /* J/m */
    return 0;
}

int mfl_welding_t85_thick(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double t = 0;
    MflWStatus st = mfl_w_t85_thick(SI(0), SI(1), CELSIUS(2), &t, msg, sizeof msg);
    return ok_number(env, st, msg, t, dim(0, 0, 1, 0), r);
}

int mfl_welding_t85_thin(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double t = 0;
    MflWStatus st = mfl_w_t85_thin(SI(0), SI(1), SI(2), SI(3) * SI(4), CELSIUS(5), &t, msg, sizeof msg);
    return ok_number(env, st, msg, t, dim(0, 0, 1, 0), r);
}

static int en1011(MflNativeEnv *env, const MflValue *a, double f2, double f3, int want_dt,
                  MflValue *r)
{
    char msg[MSG];
    double t = 0, dt = 0;
    int mode = 0;
    /* the standard's units: kJ/mm, mm, degC */
    MflWStatus st = mfl_w_t85_en1011(SI(0) / 1e6, SI(1) * 1000, CELSIUS(2), f2, f3, &t, &mode, &dt,
                                      msg, sizeof msg);
    if (want_dt) {
        return ok_number(env, st, msg, dt / 1000, dim(1, 0, 0, 0), r);
    }
    return ok_number(env, st, msg, t, dim(0, 0, 1, 0), r);
}

int mfl_welding_t85_en1011(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return en1011(env, a, 1, 1, 0, r);
}

int mfl_welding_t85_en1011_joint(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return en1011(env, a, a[3].as.number, a[4].as.number, 0, r);
}

int mfl_welding_en1011_transition(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    /* transition thickness: the thickness argument is irrelevant; use 1 mm */
    MflValue b[3];
    b[0] = a[0];
    b[1] = mfl_native_number(0.001, dim(1, 0, 0, 0));
    b[2] = a[1];
    return en1011(env, b, 1, 1, 1, r);
}

int mfl_welding_temperature_thick(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    double k, alpha, rho_c;
    if (material(env, a, 2, &k, &alpha, &rho_c)) {
        return 1;
    }
    if (!(SI(0) > 0 && SI(1) > 0)) {
        return mfl_native_fail(env, "welding.temperature_thick: power and speed must be positive");
    }
    *r = mfl_native_number(mfl_w_rosenthal_thick(SI(0), SI(1), k, alpha, KELVIN(5), SI(6), SI(7), SI(8)),
                           dim(0, 0, 0, 1));
    return 0;
}

int mfl_welding_temperature_thin(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    double k, alpha, rho_c;
    if (material(env, a, 3, &k, &alpha, &rho_c)) {
        return 1;
    }
    if (!(SI(0) > 0 && SI(1) > 0 && SI(2) > 0)) {
        return mfl_native_fail(env, "welding.temperature_thin: power, speed and thickness must be "
                                    "positive");
    }
    *r = mfl_native_number(mfl_w_rosenthal_thin(SI(0), SI(1), SI(2), k, alpha, KELVIN(6), SI(7), SI(8)),
                           dim(0, 0, 0, 1));
    return 0;
}

int mfl_welding_peak_thick(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double k, alpha, rho_c, Tp = 0;
    MflWStatus st;
    if (material(env, a, 2, &k, &alpha, &rho_c)) {
        return 1;
    }
    st = mfl_w_peak_thick(SI(0), SI(1), k, alpha, KELVIN(5), SI(6), &Tp, msg, sizeof msg);
    return ok_number(env, st, msg, Tp, dim(0, 0, 0, 1), r);
}

int mfl_welding_peak_thin(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double k, alpha, rho_c, Tp = 0;
    MflWStatus st;
    if (material(env, a, 3, &k, &alpha, &rho_c)) {
        return 1;
    }
    st = mfl_w_peak_thin(SI(0), SI(1), SI(2), k, alpha, KELVIN(6), SI(7), &Tp, msg, sizeof msg);
    return ok_number(env, st, msg, Tp, dim(0, 0, 0, 1), r);
}

int mfl_welding_distance_for_peak_thick(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double k, alpha, rho_c, y = 0;
    MflWStatus st;
    if (material(env, a, 2, &k, &alpha, &rho_c)) {
        return 1;
    }
    st = mfl_w_distance_for_peak(SI(0), SI(1), 0, k, alpha, KELVIN(5), KELVIN(6), &y, msg, sizeof msg);
    return ok_number(env, st, msg, y, dim(1, 0, 0, 0), r);
}

int mfl_welding_distance_for_peak_thin(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double k, alpha, rho_c, y = 0;
    MflWStatus st;
    if (material(env, a, 3, &k, &alpha, &rho_c)) {
        return 1;
    }
    if (!(SI(2) > 0)) {
        return mfl_native_fail(env, "welding.distance_for_peak_thin: thickness must be positive");
    }
    st = mfl_w_distance_for_peak(SI(0), SI(1), SI(2), k, alpha, KELVIN(6), KELVIN(7), &y, msg, sizeof msg);
    return ok_number(env, st, msg, y, dim(1, 0, 0, 0), r);
}
