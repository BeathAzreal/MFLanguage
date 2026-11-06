/* units.c - dimensions and unit registry. SPDX-License-Identifier: MIT */
#include "runtime/units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { L_ = 0, M_ = 1, T_ = 2, I_ = 3, TH_ = 4, N_ = 5, J_ = 6 };

static const char *const base_symbol[MFL_DIM_COUNT] = {"m", "kg", "s", "A", "K", "mol", "cd"};

MflDim mfl_dim_none(void)
{
    MflDim d;
    memset(&d, 0, sizeof d);
    return d;
}

int mfl_dim_is_none(MflDim d) { return mfl_dim_equal(d, mfl_dim_none()); }

int mfl_dim_equal(MflDim a, MflDim b) { return memcmp(a.exp, b.exp, sizeof a.exp) == 0; }

static int combine(MflDim a, MflDim b, int sign, MflDim *out)
{
    int i;
    MflDim r = mfl_dim_none();
    for (i = 0; i < MFL_DIM_COUNT; i++) {
        int e = a.exp[i] + sign * b.exp[i];
        if (e > MFL_DIM_EXP_MAX || e < -MFL_DIM_EXP_MAX) {
            return 0;
        }
        r.exp[i] = (signed char)e;
    }
    *out = r;
    return 1;
}

int mfl_dim_mul(MflDim a, MflDim b, MflDim *out) { return combine(a, b, 1, out); }

int mfl_dim_div(MflDim a, MflDim b, MflDim *out) { return combine(a, b, -1, out); }

int mfl_dim_pow(MflDim a, long n, MflDim *out)
{
    int i;
    MflDim r = mfl_dim_none();
    for (i = 0; i < MFL_DIM_COUNT; i++) {
        long e = (long)a.exp[i] * n;
        if (a.exp[i] != 0 && (n > MFL_DIM_EXP_MAX || n < -MFL_DIM_EXP_MAX ||
                              e > MFL_DIM_EXP_MAX || e < -MFL_DIM_EXP_MAX)) {
            return 0;
        }
        r.exp[i] = (signed char)(a.exp[i] == 0 ? 0 : e);
    }
    *out = r;
    return 1;
}

/* Appends "sym" or "sym^n" for each exponent with the given sign. */
static size_t format_side(MflDim d, int positive, char *buf, size_t size, size_t pos, int *count)
{
    /* Conventional order: kg m s A K mol cd. */
    static const int order[MFL_DIM_COUNT] = {M_, L_, T_, I_, TH_, N_, J_};
    int k;
    for (k = 0; k < MFL_DIM_COUNT; k++) {
        int i = order[k];
        int e = positive ? d.exp[i] : -d.exp[i];
        int n;
        if (e <= 0) {
            continue;
        }
        if (pos >= size) {
            break;
        }
        n = e == 1 ? snprintf(buf + pos, size - pos, "%s%s", *count ? "*" : "", base_symbol[i])
                   : snprintf(buf + pos, size - pos, "%s%s^%d", *count ? "*" : "",
                              base_symbol[i], e);
        if (n > 0) {
            pos += (size_t)n;
        }
        (*count)++;
    }
    return pos;
}

void mfl_dim_format(MflDim d, char *buf, size_t size)
{
    char num[96];
    char den[96];
    int nnum = 0;
    int nden = 0;

    if (size == 0) {
        return;
    }
    num[0] = den[0] = '\0';
    format_side(d, 1, num, sizeof num, 0, &nnum);
    format_side(d, 0, den, sizeof den, 0, &nden);
    if (nnum == 0 && nden == 0) {
        snprintf(buf, size, "1");
    } else if (nden == 0) {
        snprintf(buf, size, "%s", num);
    } else {
        snprintf(buf, size, "%s/%s%s%s", nnum ? num : "1", nden > 1 ? "(" : "", den,
                 nden > 1 ? ")" : "");
    }
}

/* ------------------------------------------------------------------------ */
/* Registry                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *symbol;
    double      num;   /* scale = num * 10^exp10 */
    int         exp10;
    signed char exp[MFL_DIM_COUNT]; /* m kg s A K mol cd */
    int         prefixable;
    int         affine; /* offset scale (degC) */
} UnitDef;

static const UnitDef units[] = {
    /* SI base units (kg takes no prefix: prefixes go on g) */
    {"m", 1, 0, {1, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"kg", 1, 0, {0, 1, 0, 0, 0, 0, 0}, 0, 0},
    {"g", 1, -3, {0, 1, 0, 0, 0, 0, 0}, 1, 0},
    {"s", 1, 0, {0, 0, 1, 0, 0, 0, 0}, 1, 0},
    {"A", 1, 0, {0, 0, 0, 1, 0, 0, 0}, 1, 0},
    {"K", 1, 0, {0, 0, 0, 0, 1, 0, 0}, 1, 0},
    {"mol", 1, 0, {0, 0, 0, 0, 0, 1, 0}, 1, 0},
    {"cd", 1, 0, {0, 0, 0, 0, 0, 0, 1}, 1, 0},
    /* SI derived units */
    {"N", 1, 0, {1, 1, -2, 0, 0, 0, 0}, 1, 0},
    {"Pa", 1, 0, {-1, 1, -2, 0, 0, 0, 0}, 1, 0},
    {"J", 1, 0, {2, 1, -2, 0, 0, 0, 0}, 1, 0},
    {"W", 1, 0, {2, 1, -3, 0, 0, 0, 0}, 1, 0},
    {"C", 1, 0, {0, 0, 1, 1, 0, 0, 0}, 1, 0},
    {"V", 1, 0, {2, 1, -3, -1, 0, 0, 0}, 1, 0},
    {"ohm", 1, 0, {2, 1, -3, -2, 0, 0, 0}, 1, 0},
    {"Hz", 1, 0, {0, 0, -1, 0, 0, 0, 0}, 1, 0},
    /* Non-SI units accepted in welding and materials work */
    {"min", 60, 0, {0, 0, 1, 0, 0, 0, 0}, 0, 0},
    {"h", 3600, 0, {0, 0, 1, 0, 0, 0, 0}, 0, 0},
    {"L", 1, -3, {3, 0, 0, 0, 0, 0, 0}, 1, 0},
    {"bar", 1, 5, {-1, 1, -2, 0, 0, 0, 0}, 0, 0},
    /* Temperature on the Celsius scale (v0.2, docs/units.md section 5) */
    {"degC", 1, 0, {0, 0, 0, 0, 1, 0, 0}, 0, 1},       /* absolute temperature */
    {"delta_degC", 1, 0, {0, 0, 0, 0, 1, 0, 0}, 0, 0}, /* temperature difference = 1 K */
    /* Dimensionless */
    {"percent", 1, -2, {0, 0, 0, 0, 0, 0, 0}, 0, 0},
};

static const struct {
    char symbol;
    int  exp10;
} prefixes[] = {
    {'p', -12}, {'n', -9}, {'u', -6}, {'m', -3}, {'c', -2},
    {'d', -1},  {'k', 3},  {'M', 6},  {'G', 9},
};

void mfl_dim_display_name(MflDim d, char *buf, size_t size)
{
    size_t i;
    const UnitDef *match = NULL;
    int matches = 0;
    for (i = 0; i < sizeof units / sizeof units[0]; i++) {
        if (units[i].num == 1.0 && units[i].exp10 == 0 && !units[i].affine &&
            strcmp(units[i].symbol, "delta_degC") != 0 &&
            memcmp(units[i].exp, d.exp, sizeof d.exp) == 0) {
            match = &units[i];
            matches++;
        }
    }
    if (matches == 1) {
        snprintf(buf, size, "%s", match->symbol);
    } else {
        mfl_dim_format(d, buf, size);
    }
}

static const UnitDef *find_exact(const char *name, size_t length)
{
    size_t i;
    for (i = 0; i < sizeof units / sizeof units[0]; i++) {
        if (strlen(units[i].symbol) == length && memcmp(units[i].symbol, name, length) == 0) {
            return &units[i];
        }
    }
    return NULL;
}

static MflUnitStatus found(const UnitDef *u, int prefix_exp10, MflScale *scale, MflDim *dim,
                           int *affine)
{
    scale->num = u->num;
    scale->den = 1.0;
    scale->exp10 = u->exp10 + prefix_exp10;
    memcpy(dim->exp, u->exp, sizeof dim->exp);
    if (affine != NULL) {
        *affine = u->affine;
    }
    return MFL_UNIT_OK;
}

MflUnitStatus mfl_unit_lookup(const char *name, size_t length, MflScale *scale, MflDim *dim,
                              int *affine)
{
    const UnitDef *u = find_exact(name, length);
    size_t i;

    if (affine != NULL) {
        *affine = 0;
    }
    if (u != NULL) {
        return found(u, 0, scale, dim, affine);
    }
    if (length >= 2) {
        for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            if (name[0] == prefixes[i].symbol) {
                u = find_exact(name + 1, length - 1);
                if (u != NULL && u->prefixable) {
                    return found(u, prefixes[i].exp10, scale, dim, affine);
                }
            }
        }
    }
    if ((length == 4 && memcmp(name, "degF", 4) == 0) ||
        (length == 10 && memcmp(name, "delta_degF", 10) == 0)) {
        return MFL_UNIT_UNSUPPORTED;
    }
    return MFL_UNIT_UNKNOWN;
}

/* ------------------------------------------------------------------------ */
/* Exact scales                                                              */
/* ------------------------------------------------------------------------ */

MflScale mfl_scale_one(void)
{
    MflScale s;
    s.num = 1.0;
    s.den = 1.0;
    s.exp10 = 0;
    return s;
}

MflScale mfl_scale_mul(MflScale a, MflScale b)
{
    MflScale s;
    s.num = a.num * b.num;
    s.den = a.den * b.den;
    s.exp10 = a.exp10 + b.exp10;
    return s;
}

MflScale mfl_scale_div(MflScale a, MflScale b)
{
    MflScale s;
    s.num = a.num * b.den;
    s.den = a.den * b.num;
    s.exp10 = a.exp10 - b.exp10;
    return s;
}

MflScale mfl_scale_pow(MflScale a, long n)
{
    MflScale s = mfl_scale_one();
    long k = n < 0 ? -n : n;
    long i;
    for (i = 0; i < k; i++) {
        s = mfl_scale_mul(s, a);
    }
    if (n < 0) {
        s = mfl_scale_div(mfl_scale_one(), s);
    }
    return s;
}

int mfl_scale_equal(MflScale a, MflScale b)
{
    return a.num * b.den == b.num * a.den && a.exp10 == b.exp10;
}

/* x * 10^e with exact powers of ten (10^22 is the largest exact double). */
static double times_pow10(double x, int e)
{
    static const double p10[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };
    while (e > 22) {
        x *= 1e22;
        e -= 22;
    }
    while (e < -22) {
        x /= 1e22;
        e += 22;
    }
    return e >= 0 ? x * p10[e] : x / p10[-e];
}

double mfl_scale_to_double(MflScale s) { return times_pow10(s.num / s.den, s.exp10); }

double mfl_scale_convert(double mag, MflScale from, MflScale to)
{
    double num = from.num * to.den;
    double den = from.den * to.num;
    int e = from.exp10 - to.exp10;
    double r = mag;
    if (num == den) {
        return times_pow10(r, e); /* pure decimal: one rounding */
    }
    if (num != 1.0) {
        r *= num;
    }
    if (den != 1.0) {
        r /= den;
    }
    return times_pow10(r, e);
}
