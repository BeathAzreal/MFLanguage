/* units.c - dimensions and unit registry. SPDX-License-Identifier: MIT */
#include "runtime/units.h"

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
    double      scale;
    signed char exp[MFL_DIM_COUNT]; /* m kg s A K mol cd */
    int         prefixable;
} UnitDef;

static const UnitDef units[] = {
    /* SI base units (kg takes no prefix: prefixes go on g) */
    {"m", 1.0, {1, 0, 0, 0, 0, 0, 0}, 1},
    {"kg", 1.0, {0, 1, 0, 0, 0, 0, 0}, 0},
    {"g", 1e-3, {0, 1, 0, 0, 0, 0, 0}, 1},
    {"s", 1.0, {0, 0, 1, 0, 0, 0, 0}, 1},
    {"A", 1.0, {0, 0, 0, 1, 0, 0, 0}, 1},
    {"K", 1.0, {0, 0, 0, 0, 1, 0, 0}, 1},
    {"mol", 1.0, {0, 0, 0, 0, 0, 1, 0}, 1},
    {"cd", 1.0, {0, 0, 0, 0, 0, 0, 1}, 1},
    /* SI derived units */
    {"N", 1.0, {1, 1, -2, 0, 0, 0, 0}, 1},
    {"Pa", 1.0, {-1, 1, -2, 0, 0, 0, 0}, 1},
    {"J", 1.0, {2, 1, -2, 0, 0, 0, 0}, 1},
    {"W", 1.0, {2, 1, -3, 0, 0, 0, 0}, 1},
    {"C", 1.0, {0, 0, 1, 1, 0, 0, 0}, 1},
    {"V", 1.0, {2, 1, -3, -1, 0, 0, 0}, 1},
    {"ohm", 1.0, {2, 1, -3, -2, 0, 0, 0}, 1},
    {"Hz", 1.0, {0, 0, -1, 0, 0, 0, 0}, 1},
    /* Non-SI units accepted in welding and materials work */
    {"min", 60.0, {0, 0, 1, 0, 0, 0, 0}, 0},
    {"h", 3600.0, {0, 0, 1, 0, 0, 0, 0}, 0},
    {"L", 1e-3, {3, 0, 0, 0, 0, 0, 0}, 1},
    {"bar", 1e5, {-1, 1, -2, 0, 0, 0, 0}, 0},
};

static const struct {
    char   symbol;
    double scale;
} prefixes[] = {
    {'p', 1e-12}, {'n', 1e-9}, {'u', 1e-6}, {'m', 1e-3}, {'c', 1e-2},
    {'d', 1e-1},  {'k', 1e3},  {'M', 1e6},  {'G', 1e9},
};

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

static MflUnitStatus found(const UnitDef *u, double factor, double *scale, MflDim *dim)
{
    *scale = u->scale * factor;
    memcpy(dim->exp, u->exp, sizeof dim->exp);
    return MFL_UNIT_OK;
}

MflUnitStatus mfl_unit_lookup(const char *name, size_t length, double *scale, MflDim *dim)
{
    const UnitDef *u = find_exact(name, length);
    size_t i;

    if (u != NULL) {
        return found(u, 1.0, scale, dim);
    }
    if (length >= 2) {
        for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            if (name[0] == prefixes[i].symbol) {
                u = find_exact(name + 1, length - 1);
                if (u != NULL && u->prefixable) {
                    return found(u, prefixes[i].scale, scale, dim);
                }
            }
        }
    }
    if (length == 4 && memcmp(name, "degC", 4) == 0) {
        return MFL_UNIT_UNSUPPORTED;
    }
    return MFL_UNIT_UNKNOWN;
}
