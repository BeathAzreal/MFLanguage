/*
 * natives.c - descriptors and run-time binding of the C modules (math,
 * numerical, heat). See natives.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "modules/natives.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/heat_natives.h"
#include "modules/welding_natives.h"
#include "runtime/number.h"
#include "runtime/units.h"
#include "vector.h"
#include "vm/vm.h"

#define MAX_LINSPACE 10000000u

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static MflDim dim7(int m, int kg, int s, int a, int k)
{
    MflDim d = mfl_dim_none();
    d.exp[0] = (signed char)m;
    d.exp[1] = (signed char)kg;
    d.exp[2] = (signed char)s;
    d.exp[3] = (signed char)a;
    d.exp[4] = (signed char)k;
    return d;
}

int mfl_native_fail(MflNativeEnv *env, const char *fmt, ...)
{
    va_list ap;
    if (env->message != NULL && env->message_size > 0) {
        va_start(ap, fmt);
        vsnprintf(env->message, env->message_size, fmt, ap);
        va_end(ap);
    }
    return 1;
}

void mfl_native_string(const MflValue *v, char *buf, size_t size)
{
    size_t n = v->as.string.length < size - 1 ? v->as.string.length : size - 1;
    memcpy(buf, v->as.string.chars, n);
    buf[n] = '\0';
}

double mfl_native_si(const MflNativeEnv *env, const MflValue *v)
{
    const MflUnitEntry *u;
    double r;
    if (v->unit == MFL_NO_UNIT || (size_t)v->unit >= env->chunk->unit_len) {
        return v->as.number;
    }
    u = &env->chunk->units[v->unit];
    r = mfl_scale_convert(v->as.number, u->scale, mfl_scale_one());
    if (u->affine) {
        r += u->offset;
    }
    return r;
}

MflValue mfl_native_number(double si, MflDim dim)
{
    MflValue v;
    memset(&v, 0, sizeof v);
    v.kind = MFL_VAL_NUMBER;
    v.unit = MFL_NO_UNIT;
    v.dim = dim;
    v.as.number = si;
    return v;
}

typedef struct {
    char  *buf;
    size_t size;
    size_t len;
} TextSink;

static void sink_write(void *ctx, const char *data, size_t n)
{
    TextSink *s = (TextSink *)ctx;
    size_t room = s->size > s->len + 1 ? s->size - s->len - 1 : 0;
    if (n > room) {
        n = room;
    }
    memcpy(s->buf + s->len, data, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

int mfl_native_value_text(const MflNativeEnv *env, const MflValue *v, char *buf, size_t size)
{
    TextSink s;
    if (size == 0) {
        return 0;
    }
    s.buf = buf;
    s.size = size;
    s.len = 0;
    buf[0] = '\0';
    mfl_vm_format_value(env->chunk, v, sink_write, &s);
    return 1;
}

/* Magnitude of v expressed in the unit `unit` of the chunk (or SI). */
static double in_unit(const MflNativeEnv *env, const MflValue *v, int unit)
{
    const MflUnitEntry *to;
    if (v->unit == unit) {
        return v->as.number;
    }
    if (unit == MFL_NO_UNIT || (size_t)unit >= env->chunk->unit_len) {
        return mfl_native_si(env, v);
    }
    to = &env->chunk->units[unit];
    if (!to->affine && !v->affine) {
        MflScale from = v->unit == MFL_NO_UNIT || (size_t)v->unit >= env->chunk->unit_len
                            ? mfl_scale_one()
                            : env->chunk->units[v->unit].scale;
        return mfl_scale_convert(v->as.number, from, to->scale);
    }
    return mfl_scale_convert(mfl_native_si(env, v) - (to->affine ? to->offset : 0.0),
                             mfl_scale_one(), to->scale);
}

static const char *unit_text(const MflNativeEnv *env, const MflValue *v)
{
    if (v->unit == MFL_NO_UNIT || (size_t)v->unit >= env->chunk->unit_len) {
        return "";
    }
    return env->chunk->units[v->unit].text;
}

/* ------------------------------------------------------------------------ */
/* math                                                                      */
/* ------------------------------------------------------------------------ */

static int math_sin(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)env;
    *r = mfl_native_number(sin(a[0].as.number), mfl_dim_none());
    return 0;
}

static int math_cos(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)env;
    *r = mfl_native_number(cos(a[0].as.number), mfl_dim_none());
    return 0;
}

static int math_exp(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)env;
    *r = mfl_native_number(exp(a[0].as.number), mfl_dim_none());
    return 0;
}

static int math_log(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)env;
    *r = mfl_native_number(log(a[0].as.number), mfl_dim_none());
    return 0;
}

static int math_sqrt(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    MflDim d = mfl_dim_none();
    int k;
    for (k = 0; k < MFL_DIM_COUNT; k++) {
        d.exp[k] = (signed char)(a[0].dim.exp[k] / 2);
    }
    *r = mfl_native_number(sqrt(mfl_native_si(env, &a[0])), d);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* numerical                                                                 */
/* ------------------------------------------------------------------------ */

static int num_linspace(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    size_t n = (size_t)a[2].as.number;
    double lo = a[0].as.number;
    double hi = in_unit(env, &a[1], a[0].unit);
    double *x;
    MflArray *arr;
    size_t i;

    if (a[2].as.number > (double)MAX_LINSPACE) {
        return mfl_native_fail(env, "numerical.linspace: at most %u points, %.0f requested",
                               MAX_LINSPACE, a[2].as.number);
    }
    if (!isfinite(lo) || !isfinite(hi)) {
        return mfl_native_fail(env, "numerical.linspace: the end points must be finite");
    }
    x = (double *)malloc(n * sizeof *x);
    if (x == NULL) {
        return 2;
    }
    arr = mfl_vm_alloc_array(env->vm, n);
    if (arr == NULL) {
        free(x);
        return 2;
    }
    mfl_vec_linspace(lo, hi, n, x);
    for (i = 0; i < n; i++) {
        MflValue v = a[0];
        v.as.number = x[i];
        arr->items[i] = v;
    }
    free(x);
    memset(r, 0, sizeof *r);
    r->kind = MFL_VAL_ARRAY;
    r->unit = MFL_NO_UNIT;
    r->as.array = arr;
    return 0;
}

/* sum (mean = 0) or mean (mean = 1) of an array of numbers. */
static int sum_or_mean(MflNativeEnv *env, const MflValue *a, MflValue *r, int mean)
{
    const MflArray *arr = a[0].as.array;
    const char *what = mean ? "numerical.mean" : "numerical.sum";
    size_t n = arr->length;
    size_t i;
    int same_unit = 1;
    double *x;
    double s;

    if (n == 0) {
        return mfl_native_fail(env,
                               "%s of an empty array is not defined (its unit and dimension are "
                               "unknown); check len(values) > 0 first",
                               what);
    }
    if (arr->items[0].kind != MFL_VAL_NUMBER) {
        char ts[160];
        mfl_value_type_name(&a[0], ts, sizeof ts);
        return mfl_native_fail(env, "argument 1 of '%s' ('values') must be an array of numbers, "
                                    "found %s", what, ts);
    }
    if (!mean && arr->items[0].affine) {
        return mfl_native_fail(env, "numerical.sum cannot add absolute temperatures (degC); "
                                    "convert them with .to(K) or use numerical.mean");
    }
    for (i = 1; i < n; i++) {
        if (arr->items[i].unit != arr->items[0].unit) {
            same_unit = 0;
            break;
        }
    }
    x = (double *)malloc(n * sizeof *x);
    if (x == NULL) {
        return 2;
    }
    for (i = 0; i < n; i++) {
        /* Same unit: magnitudes (the result keeps the unit, like 1 mm + 2 mm).
         * Mixed units: coherent SI (absolute temperatures: in the first
         * element's unit, so the result stays absolute). */
        x[i] = same_unit ? arr->items[i].as.number
               : arr->items[0].affine ? in_unit(env, &arr->items[i], arr->items[0].unit)
                                      : mfl_native_si(env, &arr->items[i]);
    }
    s = mean ? mfl_vec_mean(x, n) : mfl_vec_sum(x, n);
    free(x);
    *r = arr->items[0];
    r->as.number = s;
    if (!same_unit && !arr->items[0].affine) {
        r->unit = MFL_NO_UNIT;
    }
    return 0;
}

static int num_sum(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return sum_or_mean(env, a, r, 0);
}

static int num_mean(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return sum_or_mean(env, a, r, 1);
}

/* ------------------------------------------------------------------------ */
/* Table (constant: no global mutable state, safe across threads)            */
/* ------------------------------------------------------------------------ */

#define DIM(m, kg, s, a, k) {{(m), (kg), (s), (a), (k), 0, 0}}
#define NODIM DIM(0, 0, 0, 0, 0)
#define P(name, kind) {(name), (kind), NODIM, NULL, 0}
#define PC(name, min) {(name), MFL_NP_COUNT, NODIM, NULL, (min)}
#define PD(name, kind, dim, unit) {(name), (kind), dim, (unit), 0}
#define NOARGS {P(NULL, MFL_NP_NUMBER)}
#define LENGTH DIM(1, 0, 0, 0, 0)
#define TIME DIM(0, 0, 1, 0, 0)
#define KELVIN DIM(0, 0, 0, 0, 1)
#define CONDUCTIVITY DIM(1, 1, -3, 0, -1)  /* W/(m*K) = kg*m/(s^3*K) */
#define DENSITY DIM(-3, 1, 0, 0, 0)        /* kg/m^3 */
#define SPECIFIC_HEAT DIM(2, 0, -2, 0, -1) /* J/(kg*K) = m^2/(s^2*K) */
#define VOLT DIM(2, 1, -3, -1, 0)          /* V = kg*m^2/(s^3*A) */
#define AMPERE DIM(0, 0, 0, 1, 0)
#define WATT DIM(2, 1, -3, 0, 0)
#define SPEED_ DIM(1, 0, -1, 0, 0)
#define PER_LENGTH_J DIM(1, 1, -2, 0, 0)   /* J/m = N */
#define PW PD("power", MFL_NP_DIM, WATT, "W")
#define PV PD("speed", MFL_NP_DIM, SPEED_, "m/s")
#define PK PD("conductivity", MFL_NP_DIM, CONDUCTIVITY, "W/(m*K)")
#define PRHO PD("density", MFL_NP_DIM, DENSITY, "kg/m^3")
#define PCP PD("specific_heat", MFL_NP_DIM, SPECIFIC_HEAT, "J/(kg*K)")
#define PT0 P("preheat", MFL_NP_TEMPERATURE)
#define PD_(n) PD((n), MFL_NP_DIM, LENGTH, "m")
#define PQ PD("heat_input", MFL_NP_DIM, PER_LENGTH_J, "J/m")

static const MflNative natives[] = {
    /* math */
    {"math", "sin", 1, {P("x", MFL_NP_NUMBER)}, MFL_NR_NUMBER, NODIM, 0, 0.0, math_sin},
    {"math", "cos", 1, {P("x", MFL_NP_NUMBER)}, MFL_NR_NUMBER, NODIM, 0, 0.0, math_cos},
    {"math", "exp", 1, {P("x", MFL_NP_NUMBER)}, MFL_NR_NUMBER, NODIM, 0, 0.0, math_exp},
    {"math", "log", 1, {P("x", MFL_NP_NUMBER)}, MFL_NR_NUMBER, NODIM, 0, 0.0, math_log},
    {"math", "sqrt", 1, {P("x", MFL_NP_QUANTITY)}, MFL_NR_SQRT_FIRST, NODIM, MFL_NF_EVEN_DIM, 0.0,
     math_sqrt},
    {"math", "pi", 0, NOARGS, MFL_NR_CONSTANT, NODIM, 0, 3.14159265358979323846, NULL},

    /* numerical */
    {"numerical", "linspace", 3,
     {P("start", MFL_NP_ANY_NUMBER), P("stop", MFL_NP_SAME_AS_FIRST), PC("count", 2)},
     MFL_NR_ARRAY_OF_FIRST, NODIM, 0, 0.0, num_linspace},
    {"numerical", "sum", 1, {P("values", MFL_NP_NUMBERS)}, MFL_NR_ELEM_OF_FIRST, NODIM,
     MFL_NF_NO_AFFINE, 0.0, num_sum},
    {"numerical", "mean", 1, {P("values", MFL_NP_NUMBERS)}, MFL_NR_ELEM_OF_FIRST, NODIM, 0, 0.0,
     num_mean},

    /* heat (heat_natives.c; docs/heat-conduction.md) */
    {"heat", "bar", 4,
     {PD("length", MFL_NP_DIM, LENGTH, "m"), PD("conductivity", MFL_NP_DIM, CONDUCTIVITY, "W/(m*K)"),
      PD("density", MFL_NP_DIM, DENSITY, "kg/m^3"),
      PD("specific_heat", MFL_NP_DIM, SPECIFIC_HEAT, "J/(kg*K)")},
     MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_bar},
    {"heat", "nodes", 1, {PC("count", 3)}, MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_nodes},
    {"heat", "mesh", 1, {PD("positions", MFL_NP_DIM_ARRAY, LENGTH, "m")}, MFL_NR_NIL, NODIM, 0, 0.0,
     mfl_heat_mesh},
    {"heat", "initial_temperature", 1, {P("temperature", MFL_NP_TEMPERATURE)}, MFL_NR_NIL, NODIM, 0,
     0.0, mfl_heat_initial_temperature},
    {"heat", "initial_profile", 1, {P("temperatures", MFL_NP_TEMPERATURES)}, MFL_NR_NIL, NODIM, 0,
     0.0, mfl_heat_initial_profile},
    {"heat", "left_temperature", 1, {P("temperature", MFL_NP_TEMPERATURE)}, MFL_NR_NIL, NODIM, 0,
     0.0, mfl_heat_left_temperature},
    {"heat", "right_temperature", 1, {P("temperature", MFL_NP_TEMPERATURE)}, MFL_NR_NIL, NODIM, 0,
     0.0, mfl_heat_right_temperature},
    {"heat", "left_insulated", 0, NOARGS, MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_left_insulated},
    {"heat", "right_insulated", 0, NOARGS, MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_right_insulated},
    {"heat", "time_step", 1, {PD("time_step", MFL_NP_DIM, TIME, "s")}, MFL_NR_NIL, NODIM, 0, 0.0,
     mfl_heat_time_step},
    {"heat", "end_time", 1, {PD("end_time", MFL_NP_DIM, TIME, "s")}, MFL_NR_NIL, NODIM, 0, 0.0,
     mfl_heat_end_time},
    {"heat", "sample_every", 1, {PC("steps", 1)}, MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_sample_every},
    {"heat", "max_time_step", 0, NOARGS, MFL_NR_DIM, TIME, 0, 0.0, mfl_heat_max_time_step},
    {"heat", "run", 0, NOARGS, MFL_NR_NIL, NODIM, 0, 0.0, mfl_heat_run},
    {"heat", "sample_count", 0, NOARGS, MFL_NR_NUMBER, NODIM, 0, 0.0, mfl_heat_sample_count},
    {"heat", "times", 0, NOARGS, MFL_NR_DIM_ARRAY, TIME, 0, 0.0, mfl_heat_times},
    {"heat", "positions", 0, NOARGS, MFL_NR_DIM_ARRAY, LENGTH, 0, 0.0, mfl_heat_positions},
    {"heat", "profile", 1, {PC("sample", 0)}, MFL_NR_DIM_ARRAY, KELVIN, 0, 0.0, mfl_heat_profile},
    {"heat", "final_profile", 0, NOARGS, MFL_NR_DIM_ARRAY, KELVIN, 0, 0.0, mfl_heat_final_profile},
    {"heat", "temperature", 2,
     {PD("position", MFL_NP_DIM, LENGTH, "m"), PD("time", MFL_NP_DIM, TIME, "s")}, MFL_NR_DIM,
     KELVIN, 0, 0.0, mfl_heat_temperature},
    {"heat", "first_time_reaching", 2,
     {PD("position", MFL_NP_DIM, LENGTH, "m"), P("temperature", MFL_NP_TEMPERATURE)}, MFL_NR_ANY,
     NODIM, 0, 0.0, mfl_heat_first_time_reaching},

    /* welding (welding_natives.c; docs/welding.md) */
    {"welding", "efficiency", 1, {P("process", MFL_NP_STRING)}, MFL_NR_NUMBER, NODIM, 0, 0.0,
     mfl_welding_efficiency},
    {"welding", "heat_input", 4,
     {PD("voltage", MFL_NP_DIM, VOLT, "V"), PD("current", MFL_NP_DIM, AMPERE, "A"), PV,
      P("efficiency", MFL_NP_NUMBER)},
     MFL_NR_DIM, PER_LENGTH_J, 0, 0.0, mfl_welding_heat_input},
    {"welding", "t85_thick", 3, {PQ, PK, PT0}, MFL_NR_DIM, TIME, 0, 0.0, mfl_welding_t85_thick},
    {"welding", "t85_thin", 6, {PQ, PD_("thickness"), PK, PRHO, PCP, PT0}, MFL_NR_DIM, TIME, 0, 0.0,
     mfl_welding_t85_thin},
    {"welding", "t85_en1011", 3, {PQ, PD_("thickness"), PT0}, MFL_NR_DIM, TIME, 0, 0.0,
     mfl_welding_t85_en1011},
    {"welding", "t85_en1011_joint", 5,
     {PQ, PD_("thickness"), PT0, P("f2", MFL_NP_NUMBER), P("f3", MFL_NP_NUMBER)}, MFL_NR_DIM, TIME,
     0, 0.0, mfl_welding_t85_en1011_joint},
    {"welding", "en1011_transition", 2, {PQ, PT0}, MFL_NR_DIM, LENGTH, 0, 0.0,
     mfl_welding_en1011_transition},
    {"welding", "temperature_thick", 9, {PW, PV, PK, PRHO, PCP, PT0, PD_("x"), PD_("y"), PD_("z")},
     MFL_NR_DIM, KELVIN, 0, 0.0, mfl_welding_temperature_thick},
    {"welding", "temperature_thin", 9,
     {PW, PV, PD_("thickness"), PK, PRHO, PCP, PT0, PD_("x"), PD_("y")}, MFL_NR_DIM, KELVIN, 0, 0.0,
     mfl_welding_temperature_thin},
    {"welding", "peak_thick", 7, {PW, PV, PK, PRHO, PCP, PT0, PD_("distance")}, MFL_NR_DIM, KELVIN,
     0, 0.0, mfl_welding_peak_thick},
    {"welding", "peak_thin", 8, {PW, PV, PD_("thickness"), PK, PRHO, PCP, PT0, PD_("distance")},
     MFL_NR_DIM, KELVIN, 0, 0.0, mfl_welding_peak_thin},
    {"welding", "distance_for_peak_thick", 7,
     {PW, PV, PK, PRHO, PCP, PT0, P("temperature", MFL_NP_TEMPERATURE)}, MFL_NR_DIM, LENGTH, 0, 0.0,
     mfl_welding_distance_for_peak_thick},
    {"welding", "distance_for_peak_thin", 8,
     {PW, PV, PD_("thickness"), PK, PRHO, PCP, PT0, P("temperature", MFL_NP_TEMPERATURE)},
     MFL_NR_DIM, LENGTH, 0, 0.0, mfl_welding_distance_for_peak_thin},
};

#define NATIVE_COUNT ((int)(sizeof natives / sizeof natives[0]))

int mfl_native_count(void) { return NATIVE_COUNT; }

const MflNative *mfl_native_get(int index)
{
    return index >= 0 && index < NATIVE_COUNT ? &natives[index] : NULL;
}

static int text_is(MflText t, const char *s)
{
    return t.length == strlen(s) && memcmp(t.start, s, t.length) == 0;
}

int mfl_native_find(MflText module, MflText name)
{
    int i;
    for (i = 0; i < NATIVE_COUNT; i++) {
        if (text_is(module, natives[i].module) && text_is(name, natives[i].name)) {
            return i;
        }
    }
    return -1;
}

int mfl_native_module_exists(MflText name)
{
    int i;
    for (i = 0; i < NATIVE_COUNT; i++) {
        if (text_is(name, natives[i].module)) {
            return 1;
        }
    }
    return 0;
}

const char *mfl_native_module_list(void) { return "math, numerical, heat, welding"; }

void mfl_native_module_members(MflText module, char *buf, size_t size)
{
    size_t pos = 0;
    int i;
    if (size == 0) {
        return;
    }
    buf[0] = '\0';
    for (i = 0; i < NATIVE_COUNT; i++) {
        if (text_is(module, natives[i].module)) {
            int w = snprintf(buf + pos, size - pos, "%s%s", pos > 0 ? ", " : "", natives[i].name);
            if (w < 0 || (size_t)w >= size - pos) {
                break;
            }
            pos += (size_t)w;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Run-time argument checks                                                  */
/* ------------------------------------------------------------------------ */

static int is_difference_unit(const MflNativeEnv *env, const MflValue *v)
{
    const char *u = unit_text(env, v);
    return strstr(u, "delta_deg") != NULL;
}

static int arg_error(MflNativeEnv *env, int i, const char *want, const MflValue *v)
{
    char ts[160];
    const MflNative *n = env->native;
    mfl_value_type_name(v, ts, sizeof ts);
    return mfl_native_fail(env, "argument %d of '%s.%s' ('%s') must be %s, found %s", i + 1,
                           n->module, n->name, n->params[i].name, want, ts);
}

static void dim_want(const MflNativeParam *p, char *buf, size_t size)
{
    char d[96];
    mfl_dim_format(p->dim, d, sizeof d);
    if (p->unit != NULL && strcmp(p->unit, d) != 0) {
        snprintf(buf, size, "Quantity[%s] (%s)", d, p->unit);
    } else {
        snprintf(buf, size, "Quantity[%s]", d);
    }
}

static int check_temperature(MflNativeEnv *env, int i, const MflValue *v, const char *what)
{
    char vt[120];
    if (v->kind != MFL_VAL_NUMBER || !mfl_dim_equal(v->dim, dim7(0, 0, 0, 0, 1))) {
        return arg_error(env, i, what, v);
    }
    if (!v->affine && is_difference_unit(env, v)) {
        mfl_native_value_text(env, v, vt, sizeof vt);
        return mfl_native_fail(env,
                               "argument %d of '%s.%s' ('%s') must be an absolute temperature "
                               "(K or degC), found the temperature difference %s",
                               i + 1, env->native->module, env->native->name,
                               env->native->params[i].name, vt);
    }
    return 0;
}

static int check_arg(MflNativeEnv *env, int i, const MflValue *args)
{
    const MflNativeParam *p = &env->native->params[i];
    const MflValue *v = &args[i];
    char want[200];
    size_t k;

    switch (p->kind) {
    case MFL_NP_NUMBER:
        if (v->kind != MFL_VAL_NUMBER || !mfl_dim_is_none(v->dim)) {
            return arg_error(env, i, "a plain Number (dimensionless)", v);
        }
        return 0;
    case MFL_NP_COUNT:
        if (v->kind != MFL_VAL_NUMBER || !mfl_dim_is_none(v->dim)) {
            return arg_error(env, i, "a plain Number (a count)", v);
        }
        if (v->as.number != floor(v->as.number) || !(v->as.number >= p->min_count)) {
            char num[MFL_NUMBER_BUF];
            mfl_number_format(v->as.number, num, sizeof num);
            return mfl_native_fail(env, "argument %d of '%s.%s' ('%s') must be a whole number "
                                        ">= %d, found %s",
                                   i + 1, env->native->module, env->native->name, p->name,
                                   p->min_count, num);
        }
        return 0;
    case MFL_NP_QUANTITY:
        if (v->kind != MFL_VAL_NUMBER) {
            return arg_error(env, i, "a number or quantity", v);
        }
        if (v->affine) {
            return mfl_native_fail(env, "argument %d of '%s.%s' ('%s') cannot be an absolute "
                                        "temperature in degC; convert it with .to(K) first",
                                   i + 1, env->native->module, env->native->name, p->name);
        }
        if (env->native->flags & MFL_NF_EVEN_DIM) {
            for (k = 0; k < MFL_DIM_COUNT; k++) {
                if (v->dim.exp[k] % 2 != 0) {
                    return arg_error(env, i, "a quantity whose dimension has even exponents "
                                             "(e.g. m^2, m^2/s^2)", v);
                }
            }
        }
        return 0;
    case MFL_NP_ANY_NUMBER:
        if (v->kind != MFL_VAL_NUMBER) {
            return arg_error(env, i, "a number or quantity", v);
        }
        return 0;
    case MFL_NP_SAME_AS_FIRST:
        if (v->kind != MFL_VAL_NUMBER || !mfl_dim_equal(v->dim, args[0].dim) ||
            v->affine != args[0].affine) {
            mfl_value_type_name(&args[0], want, sizeof want);
            strncat(want, " (like argument 1)", sizeof want - strlen(want) - 1);
            return arg_error(env, i, want, v);
        }
        return 0;
    case MFL_NP_DIM:
        if (v->kind != MFL_VAL_NUMBER || !mfl_dim_equal(v->dim, p->dim) || v->affine) {
            dim_want(p, want, sizeof want);
            return arg_error(env, i, want, v);
        }
        return 0;
    case MFL_NP_TEMPERATURE:
        return check_temperature(env, i, v, "an absolute temperature (K or degC)");
    case MFL_NP_STRING:
        if (v->kind != MFL_VAL_STRING) {
            return arg_error(env, i, "a String", v);
        }
        return 0;
    case MFL_NP_NUMBERS:
    case MFL_NP_DIM_ARRAY:
    case MFL_NP_TEMPERATURES:
        if (v->kind != MFL_VAL_ARRAY) {
            return arg_error(env, i, "an array", v);
        }
        for (k = 0; k < v->as.array->length; k++) {
            const MflValue *e = &v->as.array->items[k];
            if (p->kind == MFL_NP_NUMBERS) {
                if (e->kind != MFL_VAL_NUMBER) {
                    return arg_error(env, i, "an array of numbers", v);
                }
                if ((env->native->flags & MFL_NF_NO_AFFINE) && e->affine) {
                    return mfl_native_fail(env,
                                           "%s.%s cannot add absolute temperatures (degC); "
                                           "convert them with .to(K) first",
                                           env->native->module, env->native->name);
                }
            } else if (p->kind == MFL_NP_DIM_ARRAY) {
                if (e->kind != MFL_VAL_NUMBER || !mfl_dim_equal(e->dim, p->dim) || e->affine) {
                    char d[150];
                    dim_want(p, d, sizeof d);
                    snprintf(want, sizeof want, "Array[%s]", d);
                    return arg_error(env, i, want, v);
                }
            } else if (check_temperature(env, i, e, "an array of absolute temperatures (K or degC)")) {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

int mfl_native_call(MflNativeEnv *env, int index, const MflValue *args, int argc, MflValue *result)
{
    const MflNative *n = mfl_native_get(index);
    int i;
    if (n == NULL || n->impl == NULL) {
        return mfl_native_fail(env, "internal error: unknown native function %d", index);
    }
    env->native = n;
    if (argc != n->arity) {
        return mfl_native_fail(env, "'%s.%s' takes %d argument%s, but %d %s given", n->module,
                               n->name, n->arity, n->arity == 1 ? "" : "s", argc,
                               argc == 1 ? "was" : "were");
    }
    for (i = 0; i < argc; i++) {
        if (check_arg(env, i, args)) {
            return 1;
        }
    }
    return n->impl(env, args, result);
}
