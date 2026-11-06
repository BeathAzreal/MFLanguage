/*
 * heat1d.c - explicit (FTCS) 1D heat conduction. See heat1d.h and
 * docs/heat-conduction.md.
 *
 * Mesh: nodes x_j = j dx, j = 0 .. N-1, dx = L / (N - 1) (x_{N-1} = L).
 * Update of one step of length h (Fo = alpha h / dx^2):
 *
 *   interior    T_j' = T_j + Fo (T_{j-1} - 2 T_j + T_{j+1})
 *   insulated   T_0' = T_0 + 2 Fo (T_1 - T_0)              (mirror node T_{-1} = T_1)
 *               T_{N-1}' = T_{N-1} + 2 Fo (T_{N-2} - T_{N-1})
 *   prescribed  T_0' = T_left (also at t = 0: it replaces the initial value)
 *
 * Stability (von Neumann, and positivity of the update coefficients,
 * insulated ends included): Fo <= 1/2.
 *
 * SPDX-License-Identifier: MIT
 */
#include "heat1d.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"

static const char *const param_names[MFL_H1_P_COUNT_] = {
    "length",    "conductivity", "density",   "specific_heat", "nodes",       "initial",
    "left",      "right",        "time_step", "end_time",      "sample_every"};

static const char *const param_units[MFL_H1_P_COUNT_] = {
    "m", "W/(m*K)", "kg/m^3", "J/(kg*K)", "", "K", "K", "K", "s", "s", ""};

static const char *const param_meaning[MFL_H1_P_COUNT_] = {
    "length of the bar",
    "thermal conductivity k",
    "density rho",
    "specific heat cp",
    "number of mesh nodes",
    "initial temperature (uniform or a nodal profile)",
    "boundary condition at x = 0",
    "boundary condition at x = L",
    "time step",
    "end time",
    "sampling interval in steps"};

const char *mfl_h1_param_name(MflH1Param p)
{
    return (unsigned)p < MFL_H1_P_COUNT_ ? param_names[p] : "?";
}

static MflH1Status fail(char *msg, size_t size, MflH1Status st, const char *fmt, ...)
{
    va_list ap;
    if (msg != NULL && size > 0) {
        va_start(ap, fmt);
        vsnprintf(msg, size, fmt, ap);
        va_end(ap);
    }
    return st;
}

/* Shortest round-trip decimal (locale independent). */
typedef struct {
    char s[MFL_NUMBER_BUF];
} Num;

static Num num(double x)
{
    Num n;
    mfl_number_format(x, n.s, sizeof n.s);
    return n;
}

static void set_given(MflH1Config *c, MflH1Param p, const char *given, double si)
{
    if (given != NULL && given[0] != '\0') {
        snprintf(c->given[p], MFL_H1_GIVEN, "%s", given);
    } else if (param_units[p][0] != '\0') {
        snprintf(c->given[p], MFL_H1_GIVEN, "%s %s", num(si).s, param_units[p]);
    } else {
        snprintf(c->given[p], MFL_H1_GIVEN, "%s", num(si).s);
    }
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

void mfl_h1_config_init(MflH1Config *c)
{
    memset(c, 0, sizeof *c);
}

void mfl_h1_config_free(MflH1Config *c)
{
    free(c->initial_profile);
    c->initial_profile = NULL;
    c->initial_count = 0;
}

MflH1Status mfl_h1_config_copy(MflH1Config *dst, const MflH1Config *src)
{
    double *p = NULL;
    if (src->initial_profile != NULL && src->initial_count > 0) {
        p = (double *)malloc(src->initial_count * sizeof *p);
        if (p == NULL) {
            return MFL_H1_NO_MEMORY;
        }
        memcpy(p, src->initial_profile, src->initial_count * sizeof *p);
    }
    *dst = *src;
    dst->initial_profile = p;
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_positive(MflH1Config *c, MflH1Param p, double value, const char *given,
                                char *msg, size_t msg_size)
{
    if (p != MFL_H1_P_LENGTH && p != MFL_H1_P_CONDUCTIVITY && p != MFL_H1_P_DENSITY &&
        p != MFL_H1_P_SPECIFIC_HEAT && p != MFL_H1_P_TIME_STEP && p != MFL_H1_P_END_TIME) {
        return fail(msg, msg_size, MFL_H1_INVALID, "internal error: '%s' is not a scalar parameter",
                    mfl_h1_param_name(p));
    }
    if (!isfinite(value)) {
        return fail(msg, msg_size, MFL_H1_INVALID, "%s (%s) must be finite, found %s",
                    param_names[p], param_meaning[p], given != NULL && given[0] ? given : num(value).s);
    }
    if (!(value > 0)) {
        return fail(msg, msg_size, MFL_H1_INVALID, "%s (%s) must be positive, found %s",
                    param_names[p], param_meaning[p],
                    given != NULL && given[0] ? given : num(value).s);
    }
    switch (p) {
    case MFL_H1_P_LENGTH: c->length = value; break;
    case MFL_H1_P_CONDUCTIVITY: c->conductivity = value; break;
    case MFL_H1_P_DENSITY: c->density = value; break;
    case MFL_H1_P_SPECIFIC_HEAT: c->specific_heat = value; break;
    case MFL_H1_P_TIME_STEP: c->time_step = value; break;
    default: c->end_time = value; break;
    }
    c->set |= 1u << p;
    set_given(c, p, given, value);
    return MFL_H1_OK;
}

static MflH1Status check_count(const char *name, double v, double lo, double hi, char *msg,
                               size_t msg_size)
{
    if (!isfinite(v) || v != floor(v)) {
        return fail(msg, msg_size, MFL_H1_INVALID, "%s must be a whole number, found %s", name,
                    num(v).s);
    }
    if (v < lo) {
        return fail(msg, msg_size, MFL_H1_INVALID, "%s must be at least %s, found %s", name,
                    num(lo).s, num(v).s);
    }
    if (v > hi) {
        return fail(msg, msg_size, MFL_H1_LIMIT, "%s = %s is above the limit of %s", name, num(v).s,
                    num(hi).s);
    }
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_nodes(MflH1Config *c, double nodes, char *msg, size_t msg_size)
{
    MflH1Status st = check_count("nodes (number of mesh nodes, both ends included)", nodes, 3,
                                 (double)MFL_H1_MAX_NODES, msg, msg_size);
    if (st != MFL_H1_OK) {
        return st;
    }
    c->nodes = (size_t)nodes;
    c->set |= 1u << MFL_H1_P_NODES;
    set_given(c, MFL_H1_P_NODES, NULL, nodes);
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_sample_every(MflH1Config *c, double steps, char *msg, size_t msg_size)
{
    MflH1Status st = check_count("sample_every (time steps between samples)", steps, 1,
                                 (double)MFL_H1_MAX_STEPS, msg, msg_size);
    if (st != MFL_H1_OK) {
        return st;
    }
    c->sample_every = (size_t)steps;
    c->set |= 1u << MFL_H1_P_SAMPLE_EVERY;
    set_given(c, MFL_H1_P_SAMPLE_EVERY, NULL, steps);
    return MFL_H1_OK;
}

static MflH1Status check_kelvin(const char *what, double k, const char *given, char *msg,
                                size_t msg_size)
{
    if (!isfinite(k)) {
        return fail(msg, msg_size, MFL_H1_INVALID, "%s must be finite, found %s", what,
                    given != NULL && given[0] ? given : num(k).s);
    }
    if (!(k > 0)) {
        return fail(msg, msg_size, MFL_H1_INVALID,
                    "%s must be an absolute temperature above 0 K, found %s", what,
                    given != NULL && given[0] ? given : num(k).s);
    }
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_initial_uniform(MflH1Config *c, double kelvin, const char *given,
                                       char *msg, size_t msg_size)
{
    MflH1Status st = check_kelvin("the initial temperature", kelvin, given, msg, msg_size);
    if (st != MFL_H1_OK) {
        return st;
    }
    free(c->initial_profile);
    c->initial_profile = NULL;
    c->initial_count = 0;
    c->initial = MFL_H1_INIT_UNIFORM;
    c->initial_uniform = kelvin;
    c->set |= 1u << MFL_H1_P_INITIAL;
    set_given(c, MFL_H1_P_INITIAL, given, kelvin);
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_initial_profile(MflH1Config *c, const double *kelvin, size_t count,
                                       char *msg, size_t msg_size)
{
    double *p;
    size_t i;
    MflH1Status st;
    if (kelvin == NULL && count > 0) {
        return fail(msg, msg_size, MFL_H1_INVALID, "the initial profile is NULL");
    }
    st = check_count("the number of values of the initial profile", (double)count, 3,
                     (double)MFL_H1_MAX_NODES, msg, msg_size);
    if (st != MFL_H1_OK) {
        return st;
    }
    for (i = 0; i < count; i++) {
        char what[96];
        snprintf(what, sizeof what, "value %lu of the initial profile (node %lu)",
                 (unsigned long)(i + 1), (unsigned long)i);
        st = check_kelvin(what, kelvin[i], NULL, msg, msg_size);
        if (st != MFL_H1_OK) {
            return st;
        }
    }
    p = (double *)malloc(count * sizeof *p);
    if (p == NULL) {
        return fail(msg, msg_size, MFL_H1_NO_MEMORY, "out of memory for the initial profile");
    }
    memcpy(p, kelvin, count * sizeof *p);
    free(c->initial_profile);
    c->initial_profile = p;
    c->initial_count = count;
    c->initial = MFL_H1_INIT_PROFILE;
    c->set |= 1u << MFL_H1_P_INITIAL;
    snprintf(c->given[MFL_H1_P_INITIAL], MFL_H1_GIVEN, "profile of %lu values",
             (unsigned long)count);
    return MFL_H1_OK;
}

MflH1Status mfl_h1_set_boundary(MflH1Config *c, MflH1Side side, MflH1Boundary kind,
                                double kelvin, const char *given, char *msg, size_t msg_size)
{
    MflH1Param p = side == MFL_H1_LEFT ? MFL_H1_P_LEFT : MFL_H1_P_RIGHT;
    if (side != MFL_H1_LEFT && side != MFL_H1_RIGHT) {
        return fail(msg, msg_size, MFL_H1_INVALID, "invalid side (use left or right)");
    }
    if (kind == MFL_H1_BC_TEMPERATURE) {
        char what[96];
        MflH1Status st;
        snprintf(what, sizeof what, "the %s boundary temperature",
                 side == MFL_H1_LEFT ? "left (x = 0)" : "right (x = L)");
        st = check_kelvin(what, kelvin, given, msg, msg_size);
        if (st != MFL_H1_OK) {
            return st;
        }
        c->boundary_temperature[side] = kelvin;
        set_given(c, p, given, kelvin);
    } else if (kind == MFL_H1_BC_INSULATED) {
        c->boundary_temperature[side] = 0;
        snprintf(c->given[p], MFL_H1_GIVEN, "insulated");
    } else {
        return fail(msg, msg_size, MFL_H1_INVALID, "invalid boundary condition kind");
    }
    c->boundary[side] = kind;
    c->set |= 1u << p;
    return MFL_H1_OK;
}

/* ------------------------------------------------------------------------ */
/* Running                                                                   */
/* ------------------------------------------------------------------------ */

static int has(const MflH1Config *c, MflH1Param p) { return (c->set >> p) & 1u; }

static MflH1Status missing(char *msg, size_t size, MflH1Param p)
{
    return fail(msg, size, MFL_H1_INVALID, "missing parameter '%s' (%s)", param_names[p],
                param_meaning[p]);
}

static size_t node_count(const MflH1Config *c)
{
    if (has(c, MFL_H1_P_NODES)) {
        return c->nodes;
    }
    if (c->initial == MFL_H1_INIT_PROFILE) {
        return c->initial_count;
    }
    return 0;
}

MflH1Status mfl_h1_max_time_step(const MflH1Config *c, double *dt_max, char *msg, size_t msg_size)
{
    MflH1Param need[4] = {MFL_H1_P_LENGTH, MFL_H1_P_CONDUCTIVITY, MFL_H1_P_DENSITY,
                          MFL_H1_P_SPECIFIC_HEAT};
    size_t n = node_count(c);
    double dx;
    double alpha;
    int i;
    for (i = 0; i < 4; i++) {
        if (!has(c, need[i])) {
            return missing(msg, msg_size, need[i]);
        }
    }
    if (n < 3) {
        return missing(msg, msg_size, MFL_H1_P_NODES);
    }
    dx = c->length / (double)(n - 1);
    alpha = c->conductivity / (c->density * c->specific_heat);
    *dt_max = dx * dx / (2.0 * alpha);
    if (!isfinite(*dt_max) || !(*dt_max > 0)) {
        return fail(msg, msg_size, MFL_H1_INVALID,
                    "the stability limit dx^2 / (2 alpha) = %s s is not a positive finite number "
                    "(dx = %s m, alpha = %s m^2/s): check the properties and the mesh",
                    num(*dt_max).s, num(dx).s, num(alpha).s);
    }
    return MFL_H1_OK;
}

void mfl_h1_result_free(MflH1Result *r)
{
    mfl_h1_config_free(&r->config);
    free(r->times);
    free(r->sample_step);
    free(r->positions);
    free(r->T);
    memset(r, 0, sizeof *r);
}

double mfl_h1_discrete_energy(const double *T, size_t nodes, double dx)
{
    double s = 0.5 * (T[0] + T[nodes - 1]);
    double c = 0.0; /* Neumaier compensation */
    size_t j;
    for (j = 1; j + 1 < nodes; j++) {
        double t = s + T[j];
        c += fabs(s) >= fabs(T[j]) ? (s - t) + T[j] : (T[j] - t) + s;
        s = t;
    }
    return (s + c) * dx;
}

/* One explicit step of Fourier number fo from u to v. */
static void step(const MflH1Config *c, size_t n, double fo, const double *u, double *v)
{
    size_t j;
    for (j = 1; j + 1 < n; j++) {
        v[j] = u[j] + fo * (u[j - 1] - 2.0 * u[j] + u[j + 1]);
    }
    v[0] = c->boundary[MFL_H1_LEFT] == MFL_H1_BC_TEMPERATURE
               ? c->boundary_temperature[MFL_H1_LEFT]
               : u[0] + 2.0 * fo * (u[1] - u[0]);
    v[n - 1] = c->boundary[MFL_H1_RIGHT] == MFL_H1_BC_TEMPERATURE
                   ? c->boundary_temperature[MFL_H1_RIGHT]
                   : u[n - 1] + 2.0 * fo * (u[n - 2] - u[n - 1]);
}

MflH1Status mfl_h1_run(const MflH1Config *c, MflH1Result *out, char *msg, size_t msg_size)
{
    MflH1Param need[] = {MFL_H1_P_LENGTH,    MFL_H1_P_CONDUCTIVITY, MFL_H1_P_DENSITY,
                         MFL_H1_P_SPECIFIC_HEAT, MFL_H1_P_INITIAL,   MFL_H1_P_LEFT,
                         MFL_H1_P_RIGHT,     MFL_H1_P_TIME_STEP,    MFL_H1_P_END_TIME};
    size_t n;
    size_t every;
    size_t steps;
    size_t samples;
    size_t s;
    size_t k;
    size_t j;
    double dx;
    double alpha;
    double dt_max;
    double fo;
    double full;
    double rem;
    double *u = NULL;
    double *v = NULL;
    MflH1Status st;
    unsigned i;

    memset(out, 0, sizeof *out);
    for (i = 0; i < sizeof need / sizeof need[0]; i++) {
        if (!has(c, need[i])) {
            return missing(msg, msg_size, need[i]);
        }
    }
    n = node_count(c);
    if (n == 0) {
        return missing(msg, msg_size, MFL_H1_P_NODES);
    }
    if (c->initial == MFL_H1_INIT_PROFILE && c->initial_count != n) {
        return fail(msg, msg_size, MFL_H1_INVALID,
                    "the initial profile has %lu values, but the mesh has %lu nodes (give one "
                    "temperature per node)",
                    (unsigned long)c->initial_count, (unsigned long)n);
    }
    st = mfl_h1_max_time_step(c, &dt_max, msg, msg_size);
    if (st != MFL_H1_OK) {
        return st;
    }
    dx = c->length / (double)(n - 1);
    alpha = c->conductivity / (c->density * c->specific_heat);
    fo = alpha * c->time_step / (dx * dx);
    if (!(c->time_step <= dt_max * (1.0 + MFL_H1_STABILITY_TOLERANCE))) {
        return fail(msg, msg_size, MFL_H1_UNSTABLE,
                    "unstable time step: time_step = %s s is above the stability limit of the "
                    "explicit scheme, time_step <= dx^2 / (2 alpha) = %s s (Fourier number "
                    "alpha*dt/dx^2 = %s > 1/2; dx = %s m, alpha = %s m^2/s). Use a smaller "
                    "time_step or fewer nodes",
                    num(c->time_step).s, num(dt_max).s, num(fo).s, num(dx).s, num(alpha).s);
    }

    /* Steps: as many full steps as fit; a remainder below
     * MFL_H1_STEP_TOLERANCE * dt is rounding noise and is dropped, otherwise
     * one shorter last step ends exactly at end_time. */
    full = floor(c->end_time / c->time_step);
    if (full > (double)MFL_H1_MAX_STEPS) {
        return fail(msg, msg_size, MFL_H1_LIMIT,
                    "end_time / time_step = %s steps is above the limit of %s steps per run",
                    num(full).s, num((double)MFL_H1_MAX_STEPS).s);
    }
    rem = c->end_time - full * c->time_step;
    if (rem <= MFL_H1_STEP_TOLERANCE * c->time_step) {
        steps = (size_t)full;
        rem = 0.0;
    } else {
        steps = (size_t)full + 1;
    }
    if (steps == 0) { /* end_time within rounding of zero steps: one step of end_time */
        steps = 1;
        rem = c->end_time;
    }
    if (steps > MFL_H1_MAX_STEPS) {
        return fail(msg, msg_size, MFL_H1_LIMIT, "%lu time steps are above the limit of %s per run",
                    (unsigned long)steps, num((double)MFL_H1_MAX_STEPS).s);
    }
    every = has(c, MFL_H1_P_SAMPLE_EVERY) ? c->sample_every : 1;
    samples = steps / every + 1 + (steps % every != 0 ? 1 : 0);
    if ((double)samples * (double)n > (double)MFL_H1_MAX_VALUES) {
        return fail(msg, msg_size, MFL_H1_LIMIT,
                    "the output would hold %lu samples x %lu nodes = %s temperatures, above the "
                    "limit of %s; increase sample_every (now %lu) or use fewer nodes",
                    (unsigned long)samples, (unsigned long)n, num((double)samples * (double)n).s,
                    num((double)MFL_H1_MAX_VALUES).s, (unsigned long)every);
    }
    if (c->memory_limit != 0) {
        double bytes = ((double)samples * (double)n + 2.0 * (double)samples + 3.0 * (double)n) *
                       (double)sizeof(double);
        if (bytes > (double)c->memory_limit) {
            return fail(msg, msg_size, MFL_H1_NO_MEMORY,
                        "the results need %s bytes, above the memory limit of %s bytes; increase "
                        "sample_every or use fewer nodes",
                        num(bytes).s, num((double)c->memory_limit).s);
        }
    }

    if (mfl_h1_config_copy(&out->config, c) != MFL_H1_OK) {
        goto oom;
    }
    out->times = (double *)malloc(samples * sizeof(double));
    out->sample_step = (size_t *)malloc(samples * sizeof(size_t));
    out->positions = (double *)malloc(n * sizeof(double));
    out->T = (double *)malloc(samples * n * sizeof(double));
    u = (double *)malloc(n * sizeof(double));
    v = (double *)malloc(n * sizeof(double));
    if (out->times == NULL || out->sample_step == NULL || out->positions == NULL ||
        out->T == NULL || u == NULL || v == NULL) {
        goto oom;
    }
    out->nodes = n;
    out->samples = samples;
    out->steps = steps;
    out->dx = dx;
    out->alpha = alpha;
    out->fourier = fo;
    out->max_time_step = dt_max;
    out->last_step = rem > 0 ? rem : c->time_step;
    for (j = 0; j < n; j++) {
        out->positions[j] = (double)j * dx;
    }
    out->positions[n - 1] = c->length;

    /* t = 0: the initial condition, with prescribed boundary temperatures
     * applied to the end nodes. */
    for (j = 0; j < n; j++) {
        u[j] = c->initial == MFL_H1_INIT_PROFILE ? c->initial_profile[j] : c->initial_uniform;
    }
    if (c->boundary[MFL_H1_LEFT] == MFL_H1_BC_TEMPERATURE) {
        u[0] = c->boundary_temperature[MFL_H1_LEFT];
    }
    if (c->boundary[MFL_H1_RIGHT] == MFL_H1_BC_TEMPERATURE) {
        u[n - 1] = c->boundary_temperature[MFL_H1_RIGHT];
    }
    memcpy(out->T, u, n * sizeof(double));
    out->times[0] = 0.0;
    out->sample_step[0] = 0;
    s = 1;
    for (k = 1; k <= steps; k++) {
        double *w;
        int last = k == steps;
        double f = last && rem > 0 ? alpha * rem / (dx * dx) : fo;
        step(c, n, f, u, v);
        w = u;
        u = v;
        v = w;
        if (k % every == 0 || last) {
            memcpy(out->T + s * n, u, n * sizeof(double));
            out->times[s] = last ? c->end_time : (double)k * c->time_step;
            out->sample_step[s] = k;
            s++;
        }
    }
    free(u);
    free(v);
    return MFL_H1_OK;

oom:
    free(u);
    free(v);
    mfl_h1_result_free(out);
    return fail(msg, msg_size, MFL_H1_NO_MEMORY, "out of memory for the simulation results");
}

int mfl_h1_first_sample_reaching(const MflH1Result *r, size_t node, double threshold,
                                 size_t *sample)
{
    size_t s;
    if (r == NULL || r->T == NULL || node >= r->nodes) {
        return 0;
    }
    for (s = 0; s < r->samples; s++) {
        if (r->T[s * r->nodes + node] >= threshold) {
            *sample = s;
            return 1;
        }
    }
    return 0;
}
