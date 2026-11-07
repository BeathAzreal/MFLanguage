/*
 * heat_natives.c - the `heat` module (see heat_natives.h). Each function
 * converts its arguments to SI (temperatures to absolute K), forwards them
 * to modules/numerical/heat1d.c and reports that code's messages as
 * run-time errors at the call ("heat.run: unstable time step: ...").
 *
 * SPDX-License-Identifier: MIT
 */
#include "modules/heat_natives.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"

#define MSG 600

void mfl_native_session_init(MflNativeSession *s, size_t memory_limit)
{
    memset(s, 0, sizeof *s);
    mfl_h1_config_init(&s->config);
    s->memory_limit = memory_limit;
}

void mfl_native_session_free(MflNativeSession *s)
{
    mfl_h1_config_free(&s->config);
    mfl_h1_result_free(&s->result);
}

static MflNativeSession *session(MflNativeEnv *env)
{
    if (env->session == NULL) {
        mfl_native_fail(env, "heat.%s: simulations are not available here (no simulation session)",
                        env->native->name);
    }
    return env->session;
}

/* Reports a status of heat1d.c. Out of memory (real or over the limit)
 * becomes the VM's out-of-memory status. */
static int report(MflNativeEnv *env, MflH1Status st, const char *msg)
{
    if (st == MFL_H1_OK) {
        return 0;
    }
    if (st == MFL_H1_NO_MEMORY) {
        return 2;
    }
    return mfl_native_fail(env, "heat.%s: %s", env->native->name, msg);
}

static void text_of(MflNativeEnv *env, const MflValue *v, char *buf, size_t size)
{
    mfl_native_value_text(env, v, buf, size);
}

static int set_scalar(MflNativeEnv *env, MflH1Param p, const MflValue *v)
{
    char given[MFL_H1_GIVEN];
    char msg[MSG];
    text_of(env, v, given, sizeof given);
    return report(env, mfl_h1_set_positive(&env->session->config, p, mfl_native_si(env, v), given,
                                           msg, sizeof msg),
                  msg);
}

static MflDim dim_of(int m, int s, int k)
{
    MflDim d = mfl_dim_none();
    d.exp[0] = (signed char)m;
    d.exp[2] = (signed char)s;
    d.exp[4] = (signed char)k;
    return d;
}

static int nil_result(MflValue *r)
{
    memset(r, 0, sizeof *r);
    r->kind = MFL_VAL_NIL;
    r->unit = MFL_NO_UNIT;
    return 0;
}

static int array_result(MflNativeEnv *env, const double *x, size_t n, MflDim dim, MflValue *r)
{
    MflArray *a = mfl_vm_alloc_array(env->vm, n);
    size_t i;
    if (a == NULL) {
        return 2;
    }
    for (i = 0; i < n; i++) {
        a->items[i] = mfl_native_number(x[i], dim);
    }
    memset(r, 0, sizeof *r);
    r->kind = MFL_VAL_ARRAY;
    r->unit = MFL_NO_UNIT;
    r->as.array = a;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

int mfl_heat_bar(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    MflH1Param p[4] = {MFL_H1_P_LENGTH, MFL_H1_P_CONDUCTIVITY, MFL_H1_P_DENSITY,
                       MFL_H1_P_SPECIFIC_HEAT};
    int i;
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    for (i = 0; i < 4; i++) {
        if ((rc = set_scalar(env, p[i], &a[i])) != 0) {
            return rc;
        }
    }
    return nil_result(r);
}

int mfl_heat_nodes(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    rc = report(env, mfl_h1_set_nodes(&env->session->config, a[0].as.number, msg, sizeof msg), msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_mesh(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    MflH1Config *c;
    const MflArray *x = a[0].as.array;
    size_t n = x->length;
    size_t j;
    double x0;
    double xn;
    double dx;
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    c = &env->session->config;
    if (!(c->set & (1u << MFL_H1_P_LENGTH))) {
        return mfl_native_fail(env, "heat.mesh: set the bar first with heat.bar(length, "
                                    "conductivity, density, specific_heat)");
    }
    if (n < 3) {
        return mfl_native_fail(env, "heat.mesh: the mesh needs at least 3 nodes, found %lu",
                               (unsigned long)n);
    }
    x0 = mfl_native_si(env, &x->items[0]);
    xn = mfl_native_si(env, &x->items[n - 1]);
    dx = c->length / (double)(n - 1);
    if (fabs(x0) > 1e-9 * dx) {
        char t[64];
        text_of(env, &x->items[0], t, sizeof t);
        return mfl_native_fail(env, "heat.mesh: the first node must be at x = 0, found %s", t);
    }
    if (fabs(xn - c->length) > 1e-9 * dx) {
        char t[64];
        text_of(env, &x->items[n - 1], t, sizeof t);
        return mfl_native_fail(env, "heat.mesh: the last node must be at the end of the bar (%s, "
                                    "the length given to heat.bar), found %s",
                               c->given[MFL_H1_P_LENGTH], t);
    }
    for (j = 1; j < n; j++) {
        double xj = mfl_native_si(env, &x->items[j]);
        if (!(fabs(xj - (double)j * dx) <= 1e-9 * dx)) {
            char t[64];
            char d[MFL_NUMBER_BUF];
            text_of(env, &x->items[j], t, sizeof t);
            mfl_number_format(dx, d, sizeof d);
            return mfl_native_fail(env, "heat.mesh: the mesh must be uniform (spacing %s m); node "
                                        "%lu is at %s (use numerical.linspace)",
                                   d, (unsigned long)j, t);
        }
    }
    rc = report(env, mfl_h1_set_nodes(c, (double)n, msg, sizeof msg), msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_initial_temperature(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char given[MFL_H1_GIVEN];
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    text_of(env, &a[0], given, sizeof given);
    rc = report(env, mfl_h1_set_initial_uniform(&env->session->config, mfl_native_si(env, &a[0]),
                                                given, msg, sizeof msg),
                msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_initial_profile(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflArray *arr = a[0].as.array;
    double *k;
    size_t i;
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    k = (double *)malloc((arr->length ? arr->length : 1) * sizeof *k);
    if (k == NULL) {
        return 2;
    }
    for (i = 0; i < arr->length; i++) {
        k[i] = mfl_native_si(env, &arr->items[i]);
    }
    rc = report(env, mfl_h1_set_initial_profile(&env->session->config, k, arr->length, msg,
                                                sizeof msg),
                msg);
    free(k);
    return rc != 0 ? rc : nil_result(r);
}

static int boundary(MflNativeEnv *env, MflH1Side side, const MflValue *T, MflValue *r)
{
    char given[MFL_H1_GIVEN] = "";
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    if (T != NULL) {
        text_of(env, T, given, sizeof given);
    }
    rc = report(env, mfl_h1_set_boundary(&env->session->config, side,
                                         T != NULL ? MFL_H1_BC_TEMPERATURE : MFL_H1_BC_INSULATED,
                                         T != NULL ? mfl_native_si(env, T) : 0.0, given, msg,
                                         sizeof msg),
                msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_left_temperature(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return boundary(env, MFL_H1_LEFT, &a[0], r);
}

int mfl_heat_right_temperature(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return boundary(env, MFL_H1_RIGHT, &a[0], r);
}

int mfl_heat_left_insulated(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)a;
    return boundary(env, MFL_H1_LEFT, NULL, r);
}

int mfl_heat_right_insulated(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    (void)a;
    return boundary(env, MFL_H1_RIGHT, NULL, r);
}

int mfl_heat_method(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char name[64];
    char msg[MSG];
    MflH1Method method;
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    mfl_native_string(&a[0], name, sizeof name);
    if (strcmp(name, "ftcs") == 0) {
        method = MFL_H1_FTCS;
    } else if (strcmp(name, "backward_euler") == 0) {
        method = MFL_H1_BACKWARD_EULER;
    } else {
        return mfl_native_fail(env, "heat.method: use ftcs or backward_euler");
    }
    rc = report(env, mfl_h1_set_method(&env->session->config, method, msg, sizeof msg), msg);
    return rc != 0 ? rc : nil_result(r);
}

static int convection(MflNativeEnv *env, MflH1Side side, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    rc = report(env, mfl_h1_set_convection(&env->session->config, side,
                    mfl_native_si(env, &a[0]), mfl_native_si(env, &a[1]), msg, sizeof msg), msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_left_convection(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return convection(env, MFL_H1_LEFT, a, r);
}

int mfl_heat_right_convection(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    return convection(env, MFL_H1_RIGHT, a, r);
}

int mfl_heat_time_step(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    rc = set_scalar(env, MFL_H1_P_TIME_STEP, &a[0]);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_end_time(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    rc = set_scalar(env, MFL_H1_P_END_TIME, &a[0]);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_sample_every(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    int rc;
    if (session(env) == NULL) {
        return 1;
    }
    rc = report(env, mfl_h1_set_sample_every(&env->session->config, a[0].as.number, msg, sizeof msg),
                msg);
    return rc != 0 ? rc : nil_result(r);
}

int mfl_heat_max_time_step(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    char msg[MSG];
    double dt = 0;
    int rc;
    (void)a;
    if (session(env) == NULL) {
        return 1;
    }
    rc = report(env, mfl_h1_max_time_step(&env->session->config, &dt, msg, sizeof msg), msg);
    if (rc != 0) {
        return rc;
    }
    *r = mfl_native_number(dt, dim_of(0, 1, 0));
    return 0;
}

int mfl_heat_run(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    MflNativeSession *s = session(env);
    MflH1Result res;
    char msg[MSG];
    MflH1Status st;
    (void)a;
    if (s == NULL) {
        return 1;
    }
    s->config.memory_limit = s->memory_limit;
    st = mfl_h1_run(&s->config, &res, msg, sizeof msg);
    if (st != MFL_H1_OK) {
        return report(env, st, msg);
    }
    mfl_h1_result_free(&s->result);
    s->result = res;
    s->runs++;
    return nil_result(r);
}

/* ------------------------------------------------------------------------ */
/* Results                                                                   */
/* ------------------------------------------------------------------------ */

static const MflH1Result *results(MflNativeEnv *env)
{
    if (session(env) == NULL) {
        return NULL;
    }
    if (env->session->result.T == NULL) {
        mfl_native_fail(env, "heat.%s: no simulation has run yet (call heat.run() first)",
                        env->native->name);
        return NULL;
    }
    return &env->session->result;
}

int mfl_heat_sample_count(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    (void)a;
    if (res == NULL) {
        return 1;
    }
    *r = mfl_native_number((double)res->samples, mfl_dim_none());
    return 0;
}

int mfl_heat_times(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    (void)a;
    return res == NULL ? 1 : array_result(env, res->times, res->samples, dim_of(0, 1, 0), r);
}

int mfl_heat_positions(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    (void)a;
    return res == NULL ? 1 : array_result(env, res->positions, res->nodes, dim_of(1, 0, 0), r);
}

int mfl_heat_profile(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    size_t s;
    if (res == NULL) {
        return 1;
    }
    if (!(a[0].as.number < (double)res->samples)) {
        return mfl_native_fail(env, "heat.profile: sample %.0f does not exist (there are %lu "
                                    "samples: 0 to %lu)",
                               a[0].as.number, (unsigned long)res->samples,
                               (unsigned long)(res->samples - 1));
    }
    s = (size_t)a[0].as.number;
    return array_result(env, res->T + s * res->nodes, res->nodes, dim_of(0, 0, 1), r);
}

int mfl_heat_final_profile(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    (void)a;
    if (res == NULL) {
        return 1;
    }
    return array_result(env, res->T + (res->samples - 1) * res->nodes, res->nodes, dim_of(0, 0, 1),
                        r);
}

/* Node at position x (within 1e-9 dx), or -1 after reporting. */
static long node_at(MflNativeEnv *env, const MflH1Result *res, const MflValue *x)
{
    double xs = mfl_native_si(env, x);
    double j = floor(xs / res->dx + 0.5);
    if (j >= 0 && j < (double)res->nodes && fabs(xs - res->positions[(size_t)j]) <= 1e-9 * res->dx) {
        return (long)j;
    }
    {
        char t[64];
        char d[MFL_NUMBER_BUF];
        char l[MFL_NUMBER_BUF];
        mfl_native_value_text(env, x, t, sizeof t);
        mfl_number_format(res->dx, d, sizeof d);
        mfl_number_format(res->config.length, l, sizeof l);
        mfl_native_fail(env, "heat.%s: position %s is not a node of the mesh (nodes every %s m "
                             "from 0 to %s m); results exist only at the nodes",
                        env->native->name, t, d, l);
    }
    return -1;
}

int mfl_heat_temperature(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    long j;
    size_t s;
    double t;
    if (res == NULL) {
        return 1;
    }
    j = node_at(env, res, &a[0]);
    if (j < 0) {
        return 1;
    }
    t = mfl_native_si(env, &a[1]);
    for (s = 0; s < res->samples; s++) {
        if (fabs(res->times[s] - t) <= 1e-9 * res->config.time_step) {
            *r = mfl_native_number(res->T[s * res->nodes + (size_t)j], dim_of(0, 0, 1));
            return 0;
        }
    }
    {
        char tt[64];
        char e[MFL_NUMBER_BUF];
        mfl_native_value_text(env, &a[1], tt, sizeof tt);
        mfl_number_format(res->config.end_time, e, sizeof e);
        return mfl_native_fail(env, "heat.temperature: t = %s is not a sample time (samples every "
                                    "%lu steps from 0 s, and the final time %s s; see heat.times())",
                               tt, (unsigned long)(res->config.sample_every ? res->config.sample_every : 1),
                               e);
    }
}

int mfl_heat_first_time_reaching(MflNativeEnv *env, const MflValue *a, MflValue *r)
{
    const MflH1Result *res = results(env);
    long j;
    size_t s = 0;
    if (res == NULL) {
        return 1;
    }
    j = node_at(env, res, &a[0]);
    if (j < 0) {
        return 1;
    }
    if (!mfl_h1_first_sample_reaching(res, (size_t)j, mfl_native_si(env, &a[1]), &s)) {
        return nil_result(r);
    }
    *r = mfl_native_number(res->times[s], dim_of(0, 1, 0));
    return 0;
}
