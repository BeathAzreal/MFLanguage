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

typedef enum {
    MFL_UNIT_OK,
    MFL_UNIT_UNKNOWN,
    MFL_UNIT_UNSUPPORTED /* known name, not available in v0.1 (degC) */
} MflUnitStatus;

/* Looks up a unit symbol, with an optional SI prefix (p n u m c d k M G).
 * An exact symbol always wins over a prefixed reading ("min" is minute,
 * "cd" is candela). On success sets *scale (factor to coherent SI) and
 * *dim. */
MflUnitStatus mfl_unit_lookup(const char *name, size_t length, double *scale, MflDim *dim);

#endif /* MFL_UNITS_H */
