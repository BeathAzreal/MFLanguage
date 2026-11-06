/*
 * units.h - physical dimensions and the v0.1 unit registry (internal API).
 *
 * A dimension is a vector of integer exponents over the seven SI base
 * dimensions, in this order: length (m), mass (kg), time (s), electric
 * current (A), temperature (K), amount of substance (mol), luminous
 * intensity (cd). See docs/units.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_UNITS_H
#define MFL_UNITS_H

#include <stddef.h>

#define MFL_DIM_COUNT 7
#define MFL_DIM_EXP_MAX 127

typedef struct {
    signed char exp[MFL_DIM_COUNT];
} MflDim;

/* Result of combining dimensions: 1 = ok, 0 = an exponent overflowed. */
MflDim mfl_dim_none(void);
int    mfl_dim_is_none(MflDim d);
int    mfl_dim_equal(MflDim a, MflDim b);
int    mfl_dim_mul(MflDim a, MflDim b, MflDim *out);
int    mfl_dim_div(MflDim a, MflDim b, MflDim *out);
int    mfl_dim_pow(MflDim a, long n, MflDim *out);

/* Writes the dimension in SI base units, e.g. "kg*m^2/(s^3*A)", "m/s",
 * "1/s", or "1" when dimensionless. Always NUL-terminates. */
void mfl_dim_format(MflDim d, char *buf, size_t size);

/* Exact factor from a unit to coherent SI: num / den * 10^exp10.
 * num and den are integers held in doubles (exact up to 2^53); prefixes
 * and decimal units only change exp10, so mm, kJ/mm, g/cm^3 are exact. */
typedef struct {
    double num;
    double den;
    int    exp10;
} MflScale;

MflScale mfl_scale_one(void);
MflScale mfl_scale_mul(MflScale a, MflScale b);
MflScale mfl_scale_div(MflScale a, MflScale b);
MflScale mfl_scale_pow(MflScale a, long n);
int      mfl_scale_equal(MflScale a, MflScale b);
double   mfl_scale_to_double(MflScale s); /* approximate value */

/* Converts a magnitude from unit scale `from` to unit scale `to`:
 * mag * (from / to), with the decimal part applied as one exact power of
 * ten, so pure prefix conversions (mm -> m, g/cm^3 -> kg/m^3) are
 * correctly rounded. */
double mfl_scale_convert(double mag, MflScale from, MflScale to);

typedef enum {
    MFL_UNIT_OK,
    MFL_UNIT_UNKNOWN,
    MFL_UNIT_UNSUPPORTED /* known name, not available (reserved) */
} MflUnitStatus;

/* Offset of the Celsius scale: T[K] = T[degC] + 273.15. */
#define MFL_DEGC_OFFSET 273.15

/* Looks up a unit symbol, with an optional SI prefix (p n u m c d k M G).
 * An exact symbol always wins over a prefixed reading ("min" is minute,
 * "cd" is candela). On success sets *scale (factor to coherent SI) and
 * *dim; *affine (may be NULL) is set to 1 for an offset scale (degC: an
 * absolute temperature, T[K] = T[degC] + MFL_DEGC_OFFSET). */
MflUnitStatus mfl_unit_lookup(const char *name, size_t length, MflScale *scale, MflDim *dim,
                              int *affine);

/* Name used to display a value of dimension d that has no unit of its
 * own (units.md section 4): the coherent SI symbol when exactly one
 * matches (e.g. "W", "V", "Pa"), otherwise the base-unit form
 * ("kg*m/s^2" is N, so that one prints as "N"). */
void mfl_dim_display_name(MflDim d, char *buf, size_t size);

#endif /* MFL_UNITS_H */
