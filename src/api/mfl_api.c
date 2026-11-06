/*
 * mfl_api.c - public embedding API (include/mfl/mfl.h).
 *
 * A state holds the host inputs (name, value, unit), the results of the
 * last successful run (every top-level variable, numbers kept in SI with
 * their dimension) and the last error message. A script holds its own
 * copy of the source plus the parse, check and compile results, and the
 * signature of the inputs it was compiled against (names, dimensions and
 * the unit-table entry used to pass each value to the VM).
 *
 * SPDX-License-Identifier: MIT
 */
#include "mfl/mfl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/checker.h"
#include "compiler/compiler.h"
#include "compiler/loader.h"
#include "heat1d.h"
#include "heat1d_export.h"
#include "modules/natives.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "runtime/number.h"
#include "runtime/units.h"
#include "vm/vm.h"

#define UNIT_TEXT_MAX 256

typedef struct {
    char    *name;
    double   value;                    /* as given, in `unit_text` */
    char     unit_text[UNIT_TEXT_MAX]; /* normalised; "" = plain number */
    MflScale scale;
    MflDim   dim;
} Input;

typedef struct {
    char       *name;
    MflTypeKind kind;
    double      si; /* NUMBER: value in coherent SI */
    MflDim      dim;
} Result;

struct MflState {
    MflConfig cfg;
    Input    *inputs;
    size_t    input_count;
    size_t    input_cap;
    Result   *results;
    size_t    result_count;
    int       has_run;
    char     *error;
    size_t    error_len;
    size_t    error_cap;
    int       error_oom; /* the message itself could not be stored */
    unsigned long long max_steps;
    int       file_imports; /* import "file.mfl" allowed (v0.3) */
    MflHeat1D *last_heat;   /* simulation of the last successful run (v0.3) */
};

struct MflHeat1D {
    MflState           *S;
    MflH1Config         config;
    MflH1Result         result; /* result.T == NULL before a successful run */
    int                 state_owned;
    char              **sources; /* provenance for the metadata (scripts only) */
    unsigned long long *hashes;
    size_t              source_count;
};


typedef struct {
    char  *name;
    MflDim dim;
    int    unit; /* chunk unit index, or MFL_NO_UNIT */
} InputSlot;

struct MflScript {
    char            *source;
    size_t           length;
    char            *name; /* for run-time error messages */
    MflLoadResult    load;
    MflCheckResult   check;
    MflCompileResult comp;
    InputSlot       *inputs;
    size_t           input_count;
};

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

static void error_clear(MflState *S)
{
    S->error_len = 0;
    S->error_oom = 0;
    if (S->error != NULL) {
        S->error[0] = '\0';
    }
}

static void error_append(MflState *S, const char *text, size_t n)
{
    if (S->error_len + n + 1 > S->error_cap) {
        size_t cap = S->error_cap ? S->error_cap * 2 : 256;
        char *p;
        while (cap < S->error_len + n + 1) {
            cap *= 2;
        }
        p = (char *)realloc(S->error, cap);
        if (p == NULL) {
            S->error_oom = 1;
            return;
        }
        S->error = p;
        S->error_cap = cap;
    }
    memcpy(S->error + S->error_len, text, n);
    S->error_len += n;
    S->error[S->error_len] = '\0';
}

static MflStatus fail(MflState *S, MflStatus status, const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    error_append(S, text, strlen(text));
    return status;
}

static const char *file_label(const char *source_name, const MflLoadResult *ld, int file)
{
    if (ld != NULL && ld->names != NULL && file >= 0 && file < ld->file_count) {
        return ld->names[file];
    }
    return source_name;
}

static void append_diagnostics(MflState *S, const char *source_name, const MflLoadResult *ld,
                               const MflDiagnostic *d)
{
    char line[1600];
    for (; d != NULL; d = d->next) {
        int n = snprintf(line, sizeof line, "%s:%d:%d: error: %s\n",
                         file_label(source_name, ld, d->file), d->line, d->column, d->message);
        if (n > 0) {
            error_append(S, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
    }
}

const char *mfl_error_message(const MflState *S)
{
    if (S == NULL) {
        return "";
    }
    if (S->error_oom) {
        return "out of memory (the error message could not be stored)";
    }
    return S->error != NULL ? S->error : "";
}

const char *mfl_status_name(MflStatus status)
{
    switch (status) {
    case MFL_OK: return "MFL_OK";
    case MFL_ERR_ARGUMENT: return "MFL_ERR_ARGUMENT";
    case MFL_ERR_MEMORY: return "MFL_ERR_MEMORY";
    case MFL_ERR_PROGRAM: return "MFL_ERR_PROGRAM";
    case MFL_ERR_UNIT: return "MFL_ERR_UNIT";
    case MFL_ERR_NOT_FOUND: return "MFL_ERR_NOT_FOUND";
    case MFL_ERR_TYPE: return "MFL_ERR_TYPE";
    case MFL_ERR_INPUTS_CHANGED: return "MFL_ERR_INPUTS_CHANGED";
    case MFL_ERR_NOT_RUN: return "MFL_ERR_NOT_RUN";
    case MFL_ERR_INTERNAL: return "MFL_ERR_INTERNAL";
    case MFL_ERR_RUNTIME: return "MFL_ERR_RUNTIME";
    case MFL_ERR_SIMULATION: return "MFL_ERR_SIMULATION";
    case MFL_ERR_IO: return "MFL_ERR_IO";
    }
    return "MFL_ERR_UNKNOWN";
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static char *dup_string(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p != NULL) {
        memcpy(p, s, n + 1);
    }
    return p;
}

/* A valid variable name: exactly one IDENT token (not a keyword), not a
 * built-in. */
static int valid_name(const char *name)
{
    MflLexer lx;
    MflToken t;
    size_t n = strlen(name);
    if (n == 0 || n > 200) {
        return 0;
    }
    mfl_lexer_init(&lx, name, n);
    t = mfl_lexer_next(&lx);
    if (t.kind != MFL_TOK_IDENT || t.length != n || mfl_lexer_next(&lx).kind != MFL_TOK_EOF) {
        return 0;
    }
    return strcmp(name, "print") != 0;
}

/* Resolves an API unit string. "" or NULL is a plain number. */
static MflStatus resolve_unit(MflState *S, const char *unit, MflScale *scale, MflDim *dim,
                              char *norm, size_t norm_size)
{
    MflParseResult pr;
    MflExpr *u = NULL;
    char msg[300];

    *scale = mfl_scale_one();
    *dim = mfl_dim_none();
    norm[0] = '\0';
    if (unit == NULL || unit[0] == '\0') {
        return MFL_OK;
    }
    if (!mfl_parse_unit(unit, strlen(unit), &pr, &u)) {
        MflStatus st = pr.out_of_memory ? MFL_ERR_MEMORY : MFL_ERR_UNIT;
        fail(S, st, "invalid unit '%s': %s", unit,
             pr.diagnostics != NULL ? pr.diagnostics->message : "out of memory");
        mfl_parse_free(&pr);
        return st;
    }
    if (!mfl_check_unit(u, scale, dim, msg, sizeof msg)) {
        mfl_parse_free(&pr);
        return fail(S, MFL_ERR_UNIT, "invalid unit '%s': %s", unit, msg);
    }
    if (!mfl_unit_text(u, norm, norm_size)) {
        mfl_parse_free(&pr);
        return fail(S, MFL_ERR_UNIT, "unit '%s' is too long", unit);
    }
    mfl_parse_free(&pr);
    return MFL_OK;
}

static Input *find_input(MflState *S, const char *name)
{
    size_t i;
    for (i = 0; i < S->input_count; i++) {
        if (strcmp(S->inputs[i].name, name) == 0) {
            return &S->inputs[i];
        }
    }
    return NULL;
}

static void free_results(MflState *S)
{
    size_t i;
    for (i = 0; i < S->result_count; i++) {
        free(S->results[i].name);
    }
    free(S->results);
    S->results = NULL;
    S->result_count = 0;
}

static void stdout_output(void *user, const char *text, size_t length)
{
    (void)user;
    fwrite(text, 1, length, stdout);
}

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

MflState *mfl_state_new(const MflConfig *config)
{
    MflState *S = (MflState *)calloc(1, sizeof *S);
    if (S == NULL) {
        return NULL;
    }
    if (config != NULL) {
        S->cfg = *config;
    }
    if (S->cfg.output == NULL) {
        S->cfg.output = stdout_output;
        S->cfg.user = NULL;
    }
    return S;
}

void mfl_set_file_imports(MflState *S, int enabled)
{
    if (S != NULL) {
        S->file_imports = enabled != 0;
    }
}

void mfl_set_instruction_limit(MflState *S, unsigned long long max_instructions)
{
    if (S != NULL) {
        S->max_steps = max_instructions;
    }
}

void mfl_clear_inputs(MflState *S)
{
    size_t i;
    if (S == NULL) {
        return;
    }
    for (i = 0; i < S->input_count; i++) {
        free(S->inputs[i].name);
    }
    S->input_count = 0;
    error_clear(S);
}

void mfl_state_free(MflState *S)
{
    if (S == NULL) {
        return;
    }
    mfl_clear_inputs(S);
    free(S->inputs);
    free_results(S);
    if (S->last_heat != NULL) {
        S->last_heat->state_owned = 0;
        mfl_heat1d_free(S->last_heat);
    }
    free(S->error);
    free(S);
}

MflStatus mfl_set_number(MflState *S, const char *name, double value, const char *unit)
{
    Input in;
    Input *slot;
    MflStatus st;

    if (S == NULL) {
        return MFL_ERR_ARGUMENT;
    }
    error_clear(S);
    if (name == NULL) {
        return fail(S, MFL_ERR_ARGUMENT, "input name is NULL");
    }
    if (!valid_name(name)) {
        return fail(S, MFL_ERR_ARGUMENT,
                    "invalid input name '%.60s' (use an ASCII identifier that is not a keyword "
                    "or 'print')",
                    name);
    }
    memset(&in, 0, sizeof in);
    st = resolve_unit(S, unit, &in.scale, &in.dim, in.unit_text, sizeof in.unit_text);
    if (st != MFL_OK) {
        return st;
    }
    in.value = value;

    slot = find_input(S, name);
    if (slot != NULL) {
        in.name = slot->name;
        *slot = in;
        return MFL_OK;
    }
    if (S->input_count == S->input_cap) {
        size_t cap = S->input_cap ? S->input_cap * 2 : 8;
        Input *p = (Input *)realloc(S->inputs, cap * sizeof *p);
        if (p == NULL) {
            return fail(S, MFL_ERR_MEMORY, "out of memory");
        }
        S->inputs = p;
        S->input_cap = cap;
    }
    in.name = dup_string(name);
    if (in.name == NULL) {
        return fail(S, MFL_ERR_MEMORY, "out of memory");
    }
    S->inputs[S->input_count++] = in;
    return MFL_OK;
}

MflStatus mfl_get_number(MflState *S, const char *name, const char *unit, double *out)
{
    size_t i;
    const Result *r = NULL;
    MflScale scale;
    MflDim dim;
    char norm[UNIT_TEXT_MAX];
    MflStatus st;

    if (S == NULL) {
        return MFL_ERR_ARGUMENT;
    }
    error_clear(S);
    if (name == NULL || out == NULL) {
        return fail(S, MFL_ERR_ARGUMENT, "name or output pointer is NULL");
    }
    if (!S->has_run) {
        return fail(S, MFL_ERR_NOT_RUN, "no program has run successfully on this state yet");
    }
    for (i = 0; i < S->result_count; i++) {
        if (strcmp(S->results[i].name, name) == 0) {
            r = &S->results[i];
            break;
        }
    }
    if (r == NULL) {
        return fail(S, MFL_ERR_NOT_FOUND, "no variable '%.60s' in the last run", name);
    }
    if (r->kind != MFL_TYPE_NUMBER) {
        MflType t;
        char ts[64];
        t.kind = r->kind;
        t.dim = mfl_dim_none();
        mfl_type_format(t, ts, sizeof ts);
        return fail(S, MFL_ERR_TYPE, "'%.60s' is a %s, not a number", name, ts);
    }
    st = resolve_unit(S, unit, &scale, &dim, norm, sizeof norm);
    if (st != MFL_OK) {
        return st;
    }
    if ((unit != NULL && unit[0] != '\0') && !mfl_dim_equal(dim, r->dim)) {
        char have[96];
        char want[96];
        mfl_dim_format(r->dim, have, sizeof have);
        mfl_dim_format(dim, want, sizeof want);
        return fail(S, MFL_ERR_UNIT,
                    "cannot express '%.60s' (dimension %s) in '%s' (dimension %s)", name, have,
                    unit, want);
    }
    *out = mfl_scale_convert(r->si, mfl_scale_one(), scale);
    return MFL_OK;
}

/* ------------------------------------------------------------------------ */
/* Scripts                                                                   */
/* ------------------------------------------------------------------------ */

void mfl_script_free(MflScript *script)
{
    size_t i;
    if (script == NULL) {
        return;
    }
    mfl_compile_free(&script->comp);
    mfl_check_free(&script->check);
    mfl_load_free(&script->load);
    for (i = 0; i < script->input_count; i++) {
        free(script->inputs[i].name);
    }
    free(script->inputs);
    free(script->source);
    free(script->name);
    free(script);
}

static MflScript *compile_script(MflState *S, const char *source, size_t length,
                                 const char *source_name, MflStatus *status)
{
    MflScript *sc;
    MflCheckInput *decl = NULL;
    size_t i;
    size_t limit;
    const char *name = source_name != NULL ? source_name : "<script>";

    error_clear(S);
    if (source == NULL && length > 0) {
        *status = fail(S, MFL_ERR_ARGUMENT, "source is NULL");
        return NULL;
    }
    limit = S->cfg.memory_limit;
    sc = (MflScript *)calloc(1, sizeof *sc);
    if (sc == NULL) {
        *status = fail(S, MFL_ERR_MEMORY, "out of memory");
        return NULL;
    }
    sc->source = (char *)malloc(length + 1);
    sc->name = dup_string(name);
    sc->inputs = (InputSlot *)calloc(S->input_count ? S->input_count : 1, sizeof *sc->inputs);
    decl = (MflCheckInput *)calloc(S->input_count ? S->input_count : 1, sizeof *decl);
    if (sc->source == NULL || sc->name == NULL || sc->inputs == NULL || decl == NULL) {
        goto oom;
    }
    if (length > 0) {
        memcpy(sc->source, source, length);
    }
    sc->source[length] = '\0';
    sc->length = length;

    /* The script keeps its own copy of the input names: the state's inputs
     * may change after compilation. */
    for (i = 0; i < S->input_count; i++) {
        sc->inputs[i].name = dup_string(S->inputs[i].name);
        if (sc->inputs[i].name == NULL) {
            goto oom;
        }
        sc->input_count++;
        sc->inputs[i].dim = S->inputs[i].dim;
        sc->inputs[i].unit = MFL_NO_UNIT;
        decl[i].name.start = sc->inputs[i].name;
        decl[i].name.length = strlen(sc->inputs[i].name);
        decl[i].type.kind = MFL_TYPE_NUMBER;
        decl[i].type.dim = S->inputs[i].dim;
    }

    if (!mfl_load(sc->source, length, sc->name, S->file_imports ? mfl_read_source_file : NULL,
                  NULL, limit, &sc->load)) {
        if (sc->load.out_of_memory) {
            goto oom;
        }
        append_diagnostics(S, name, &sc->load, sc->load.diagnostics);
        goto fail_program;
    }
    if (!mfl_check_with_inputs(&sc->load.program, decl, S->input_count, limit, &sc->check)) {
        if (sc->check.out_of_memory) {
            goto oom;
        }
        append_diagnostics(S, name, &sc->load, sc->check.diagnostics);
        goto fail_program;
    }
    if (!mfl_compile(&sc->load.program, &sc->check, limit, &sc->comp)) {
        if (sc->comp.out_of_memory) {
            goto oom;
        }
        append_diagnostics(S, name, &sc->load, sc->comp.diagnostics);
        goto fail_program;
    }
    /* Unit-table entries through which the host values enter the VM. */
    for (i = 0; i < S->input_count; i++) {
        if (S->inputs[i].unit_text[0] != '\0') {
            int u = mfl_chunk_add_unit(&sc->comp.chunk, S->inputs[i].unit_text, S->inputs[i].scale);
            if (u < 0) {
                goto oom;
            }
            sc->inputs[i].unit = u;
        }
    }
    free(decl);
    *status = MFL_OK;
    return sc;

oom:
    *status = fail(S, MFL_ERR_MEMORY, "out of memory while compiling '%s'", name);
    free(decl);
    mfl_script_free(sc);
    return NULL;
fail_program:
    *status = MFL_ERR_PROGRAM;
    free(decl);
    mfl_script_free(sc);
    return NULL;
}

MflScript *mfl_script_compile(MflState *S, const char *source, size_t length,
                              const char *source_name)
{
    MflStatus st;
    if (S == NULL) {
        return NULL;
    }
    return compile_script(S, source, length, source_name, &st);
}

static MflHeat1D *heat_from_session(MflState *S, MflNativeSession *sess, const MflLoadResult *ld);

MflStatus mfl_script_run(MflState *S, MflScript *sc)
{
    MflValue *init = NULL;
    MflValue *snap = NULL;
    Result *results = NULL;
    const MflChunk *chunk;
    const MflSymbol *sym;
    MflVmStatus vs;
    MflVmError verr;
    MflNativeSession sess;
    MflHeat1D *heat = NULL;
    size_t i;
    size_t n;

    if (S == NULL) {
        return MFL_ERR_ARGUMENT;
    }
    error_clear(S);
    if (sc == NULL) {
        return fail(S, MFL_ERR_ARGUMENT, "script is NULL");
    }
    chunk = &sc->comp.chunk;
    n = chunk->global_count;

    init = (MflValue *)calloc(sc->input_count ? sc->input_count : 1, sizeof *init);
    snap = (MflValue *)calloc(n ? n : 1, sizeof *snap);
    results = (Result *)calloc(n ? n : 1, sizeof *results);
    if (init == NULL || snap == NULL || results == NULL) {
        goto oom;
    }
    for (i = 0; i < sc->input_count; i++) {
        const Input *in = find_input(S, sc->inputs[i].name);
        MflScale to;
        if (in == NULL || !mfl_dim_equal(in->dim, sc->inputs[i].dim)) {
            char want[96];
            mfl_dim_format(sc->inputs[i].dim, want, sizeof want);
            fail(S, MFL_ERR_INPUTS_CHANGED,
                 in == NULL ? "input '%s' (dimension %s) was removed after the script was "
                              "compiled; compile it again"
                            : "input '%s' no longer has dimension %s; compile the script again",
                 sc->inputs[i].name, want);
            free(init);
            free(snap);
            free(results);
            return MFL_ERR_INPUTS_CHANGED;
        }
        to = sc->inputs[i].unit == MFL_NO_UNIT ? mfl_scale_one()
                                               : chunk->units[sc->inputs[i].unit].scale;
        memset(&init[i], 0, sizeof init[i]);
        init[i].kind = MFL_VAL_NUMBER;
        init[i].unit = sc->inputs[i].unit;
        init[i].dim = in->dim;
        init[i].affine = 0;
        init[i].as.number = mfl_scale_convert(in->value, in->scale, to);
    }

    {
        MflVmLimits limits;
        limits.memory_limit = S->cfg.memory_limit;
        limits.max_steps = S->max_steps;
        mfl_native_session_init(&sess, S->cfg.memory_limit);
        vs = mfl_vm_execute_ex(chunk, init, sc->input_count, snap, S->cfg.output, S->cfg.user,
                               &limits, &sess, &verr);
    }
    if (vs != MFL_VM_OK) {
        mfl_native_session_free(&sess);
    }
    if (vs == MFL_VM_OUT_OF_MEMORY) {
        goto oom;
    }
    if (vs == MFL_VM_RUNTIME_ERROR) {
        free(init);
        free(snap);
        free(results);
        return fail(S, MFL_ERR_RUNTIME, "%s:%d:%d: runtime error: %s\n",
                    file_label(sc->name, &sc->load, verr.file), verr.line, verr.column,
                    verr.message);
    }
    if (vs != MFL_VM_OK) {
        free(init);
        free(snap);
        free(results);
        return fail(S, MFL_ERR_INTERNAL, "internal error: invalid bytecode");
    }

    /* The last simulation of the run (heat.run()) moves to the state. */
    if (sess.result.T != NULL) {
        heat = heat_from_session(S, &sess, &sc->load);
        if (heat == NULL) {
            mfl_native_session_free(&sess);
            goto oom;
        }
    }
    mfl_native_session_free(&sess);

    /* Keep every top-level variable, numbers in SI. */
    for (i = 0, sym = sc->check.symbols; sym != NULL && i < n; sym = sym->next, i++) {
        Result *r = &results[i];
        r->name = (char *)malloc(sym->name.length + 1);
        if (r->name == NULL) {
            size_t k;
            for (k = 0; k < i; k++) {
                free(results[k].name);
            }
            if (heat != NULL) {
                heat->state_owned = 0;
                mfl_heat1d_free(heat);
            }
            goto oom;
        }
        memcpy(r->name, sym->name.start, sym->name.length);
        r->name[sym->name.length] = '\0';
        r->kind = sym->type.kind;
        r->dim = sym->type.dim;
        if (snap[i].kind == MFL_VAL_NUMBER) {
            MflScale from = snap[i].unit == MFL_NO_UNIT ? mfl_scale_one()
                                                        : chunk->units[snap[i].unit].scale;
            r->si = mfl_scale_convert(snap[i].as.number, from, mfl_scale_one());
            if (snap[i].unit != MFL_NO_UNIT && chunk->units[snap[i].unit].affine) {
                r->si += chunk->units[snap[i].unit].offset; /* degC: absolute value in K */
            }
        }
    }
    free_results(S);
    S->results = results;
    S->result_count = i;
    S->has_run = 1;
    if (S->last_heat != NULL) {
        S->last_heat->state_owned = 0;
        mfl_heat1d_free(S->last_heat);
    }
    S->last_heat = heat;
    free(init);
    free(snap);
    return MFL_OK;

oom:
    free(init);
    free(snap);
    free(results);
    return fail(S, MFL_ERR_MEMORY, "out of memory while running the script");
}

MflStatus mfl_eval(MflState *S, const char *source, size_t length, const char *source_name)
{
    MflScript *sc;
    MflStatus st;

    if (S == NULL) {
        return MFL_ERR_ARGUMENT;
    }
    sc = compile_script(S, source, length, source_name, &st);
    if (sc == NULL) {
        return st;
    }
    st = mfl_script_run(S, sc);
    mfl_script_free(sc);
    return st;
}

/* ------------------------------------------------------------------------ */
/* 1D heat conduction (v0.3)                                                 */
/* ------------------------------------------------------------------------ */

#define H1_MSG 700

static MflStatus h1_status(MflState *S, MflH1Status st, const char *msg, int running)
{
    switch (st) {
    case MFL_H1_OK: return MFL_OK;
    case MFL_H1_NO_MEMORY: return fail(S, MFL_ERR_MEMORY, "%s", msg);
    case MFL_H1_UNSTABLE: return fail(S, MFL_ERR_SIMULATION, "%s", msg);
    default: return fail(S, running ? MFL_ERR_SIMULATION : MFL_ERR_ARGUMENT, "%s", msg);
    }
}

static MflHeat1D *heat_alloc(MflState *S)
{
    MflHeat1D *h = (MflHeat1D *)calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }
    h->S = S;
    mfl_h1_config_init(&h->config);
    return h;
}

static MflHeat1D *heat_from_session(MflState *S, MflNativeSession *sess, const MflLoadResult *ld)
{
    MflHeat1D *h = heat_alloc(S);
    int i;
    if (h == NULL) {
        return NULL;
    }
    h->state_owned = 1;
    h->result = sess->result; /* moved */
    memset(&sess->result, 0, sizeof sess->result);
    if (ld != NULL && ld->file_count > 0) {
        h->sources = (char **)calloc((size_t)ld->file_count, sizeof *h->sources);
        h->hashes = (unsigned long long *)calloc((size_t)ld->file_count, sizeof *h->hashes);
        if (h->sources == NULL || h->hashes == NULL) {
            h->state_owned = 0;
            mfl_heat1d_free(h);
            return NULL;
        }
        for (i = 0; i < ld->file_count; i++) {
            h->sources[i] = dup_string(ld->names[i]);
            h->hashes[i] = ld->files[i].hash;
            if (h->sources[i] == NULL) {
                h->source_count = (size_t)i;
                h->state_owned = 0;
                mfl_heat1d_free(h);
                return NULL;
            }
        }
        h->source_count = (size_t)ld->file_count;
    }
    return h;
}

MflHeat1D *mfl_heat1d_new(MflState *S)
{
    MflHeat1D *h;
    if (S == NULL) {
        return NULL;
    }
    error_clear(S);
    h = heat_alloc(S);
    if (h == NULL) {
        fail(S, MFL_ERR_MEMORY, "out of memory");
    }
    return h;
}

void mfl_heat1d_free(MflHeat1D *h)
{
    size_t i;
    if (h == NULL || h->state_owned) {
        return; /* state-owned results are freed with their state */
    }
    mfl_h1_config_free(&h->config);
    mfl_h1_result_free(&h->result);
    for (i = 0; i < h->source_count; i++) {
        free(h->sources[i]);
    }
    free(h->sources);
    free(h->hashes);
    free(h);
}

const MflHeat1D *mfl_state_heat1d(MflState *S)
{
    if (S == NULL) {
        return NULL;
    }
    error_clear(S);
    return S->last_heat;
}

/* Resolves a unit (degC allowed). */
static MflStatus resolve_unit_abs(MflState *S, const char *what, const char *unit,
                                  MflScale *scale, MflDim *dim, int *affine, char *norm,
                                  size_t norm_size)
{
    MflParseResult pr;
    MflExpr *u = NULL;
    char msg[300];
    *scale = mfl_scale_one();
    *dim = mfl_dim_none();
    *affine = 0;
    norm[0] = '\0';
    if (unit == NULL || unit[0] == '\0') {
        return MFL_OK;
    }
    if (!mfl_parse_unit(unit, strlen(unit), &pr, &u)) {
        MflStatus st = pr.out_of_memory ? MFL_ERR_MEMORY : MFL_ERR_UNIT;
        fail(S, st, "%s: invalid unit '%s': %s", what, unit,
             pr.diagnostics != NULL ? pr.diagnostics->message : "out of memory");
        mfl_parse_free(&pr);
        return st;
    }
    if (!mfl_check_unit_ex(u, scale, dim, affine, msg, sizeof msg) ||
        !mfl_unit_text(u, norm, norm_size)) {
        mfl_parse_free(&pr);
        return fail(S, MFL_ERR_UNIT, "%s: invalid unit '%s': %s", what, unit, msg);
    }
    mfl_parse_free(&pr);
    return MFL_OK;
}

static MflDim dim3(int m, int kg, int s, int k)
{
    MflDim d = mfl_dim_none();
    d.exp[0] = (signed char)m;
    d.exp[1] = (signed char)kg;
    d.exp[2] = (signed char)s;
    d.exp[4] = (signed char)k;
    return d;
}

/* Converts value [unit] to SI, requiring dimension `want` (named `what`).
 * absolute: a temperature (K or degC; delta_degC rejected). */
static MflStatus to_si(MflState *S, const char *what, double value, const char *unit, MflDim want,
                       int absolute, double *si, char *given, size_t given_size)
{
    MflScale scale;
    MflDim dim;
    int affine;
    char norm[UNIT_TEXT_MAX];
    MflStatus st = resolve_unit_abs(S, what, unit, &scale, &dim, &affine, norm, sizeof norm);
    if (st != MFL_OK) {
        return st;
    }
    if (!mfl_dim_equal(dim, want) || (affine && !absolute)) {
        char have[96];
        char need[96];
        mfl_dim_format(dim, have, sizeof have);
        mfl_dim_format(want, need, sizeof need);
        return fail(S, MFL_ERR_UNIT, "%s: unit '%s' has dimension %s; %s needs %s%s", what,
                    unit != NULL ? unit : "", have, what, need,
                    absolute ? " (an absolute temperature: K or degC)" : "");
    }
    if (absolute && strstr(norm, "delta_deg") != NULL) {
        return fail(S, MFL_ERR_UNIT,
                    "%s: '%s' is a temperature difference; %s needs an absolute temperature (K or "
                    "degC)",
                    what, unit, what);
    }
    *si = mfl_scale_convert(value, scale, mfl_scale_one()) + (affine ? MFL_DEGC_OFFSET : 0.0);
    if (given != NULL) {
        char num[64];
        mfl_number_format(value, num, sizeof num);
        snprintf(given, given_size, "%s %s", num, unit);
    }
    return MFL_OK;
}

/* SI value -> unit (NULL/"": SI), requiring dimension `want`. */
static MflStatus from_si(MflState *S, const char *what, double si, const char *unit, MflDim want,
                         double *out)
{
    MflScale scale;
    MflDim dim;
    int affine;
    char norm[UNIT_TEXT_MAX];
    MflStatus st;
    if (unit == NULL || unit[0] == '\0') {
        *out = si;
        return MFL_OK;
    }
    st = resolve_unit_abs(S, what, unit, &scale, &dim, &affine, norm, sizeof norm);
    if (st != MFL_OK) {
        return st;
    }
    if (!mfl_dim_equal(dim, want)) {
        char have[96];
        char need[96];
        mfl_dim_format(dim, have, sizeof have);
        mfl_dim_format(want, need, sizeof need);
        return fail(S, MFL_ERR_UNIT, "%s: cannot express a value of dimension %s in '%s' (%s)", what,
                    need, unit, have);
    }
    if (strstr(norm, "delta_deg") != NULL) {
        return fail(S, MFL_ERR_UNIT, "%s: temperatures are absolute; use K or degC, not '%s'", what,
                    unit);
    }
    *out = mfl_scale_convert(si - (affine ? MFL_DEGC_OFFSET : 0.0), mfl_scale_one(), scale);
    return MFL_OK;
}

#define H1_BEGIN(h)                                                       \
    do {                                                                  \
        if ((h) == NULL || (h)->S == NULL) {                              \
            return MFL_ERR_ARGUMENT;                                      \
        }                                                                 \
        error_clear((h)->S);                                              \
    } while (0)

#define H1_CONFIGURABLE(h)                                                                    \
    do {                                                                                      \
        if ((h)->state_owned) {                                                               \
            return fail((h)->S, MFL_ERR_ARGUMENT,                                             \
                        "this simulation belongs to the state (mfl_state_heat1d): read its "  \
                        "results; configure a new one with mfl_heat1d_new()");                \
        }                                                                                     \
    } while (0)

MflStatus mfl_heat1d_set(MflHeat1D *h, const char *parameter, double value, const char *unit)
{
    static const struct {
        const char *name;
        MflH1Param  p;
        int         m, kg, s, k;
    } table[] = {{"length", MFL_H1_P_LENGTH, 1, 0, 0, 0},
                 {"conductivity", MFL_H1_P_CONDUCTIVITY, 1, 1, -3, -1},
                 {"density", MFL_H1_P_DENSITY, -3, 1, 0, 0},
                 {"specific_heat", MFL_H1_P_SPECIFIC_HEAT, 2, 0, -2, -1},
                 {"time_step", MFL_H1_P_TIME_STEP, 0, 0, 1, 0},
                 {"end_time", MFL_H1_P_END_TIME, 0, 0, 1, 0}};
    char given[MFL_H1_GIVEN];
    char msg[H1_MSG];
    double si = 0;
    size_t i;
    MflStatus st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    if (parameter == NULL) {
        return fail(h->S, MFL_ERR_ARGUMENT, "parameter name is NULL");
    }
    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(parameter, table[i].name) == 0) {
            st = to_si(h->S, parameter, value, unit,
                       dim3(table[i].m, table[i].kg, table[i].s, table[i].k), 0, &si, given,
                       sizeof given);
            if (st != MFL_OK) {
                return st;
            }
            return h1_status(h->S, mfl_h1_set_positive(&h->config, table[i].p, si, given, msg, sizeof msg),
                             msg, 0);
        }
    }
    return fail(h->S, MFL_ERR_ARGUMENT,
                "unknown parameter '%.60s' (use length, conductivity, density, specific_heat, "
                "time_step or end_time)",
                parameter);
}

MflStatus mfl_heat1d_set_nodes(MflHeat1D *h, size_t nodes)
{
    char msg[H1_MSG];
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    return h1_status(h->S, mfl_h1_set_nodes(&h->config, (double)nodes, msg, sizeof msg), msg, 0);
}

MflStatus mfl_heat1d_set_sample_every(MflHeat1D *h, size_t steps)
{
    char msg[H1_MSG];
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    return h1_status(h->S, mfl_h1_set_sample_every(&h->config, (double)steps, msg, sizeof msg), msg, 0);
}

MflStatus mfl_heat1d_set_initial_temperature(MflHeat1D *h, double temperature, const char *unit)
{
    char given[MFL_H1_GIVEN];
    char msg[H1_MSG];
    double k = 0;
    MflStatus st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    st = to_si(h->S, "the initial temperature", temperature, unit, dim3(0, 0, 0, 1), 1, &k, given,
               sizeof given);
    if (st != MFL_OK) {
        return st;
    }
    return h1_status(h->S, mfl_h1_set_initial_uniform(&h->config, k, given, msg, sizeof msg), msg, 0);
}

MflStatus mfl_heat1d_set_initial_profile(MflHeat1D *h, const double *temperatures, size_t count,
                                         const char *unit)
{
    char msg[H1_MSG];
    double *k;
    size_t i;
    MflStatus st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    if (temperatures == NULL) {
        return fail(h->S, MFL_ERR_ARGUMENT, "the initial profile is NULL");
    }
    if (count > MFL_H1_MAX_NODES) {
        return fail(h->S, MFL_ERR_ARGUMENT, "the initial profile has %lu values, above the limit "
                                            "of %u nodes", (unsigned long)count, MFL_H1_MAX_NODES);
    }
    k = (double *)malloc((count ? count : 1) * sizeof *k);
    if (k == NULL) {
        return fail(h->S, MFL_ERR_MEMORY, "out of memory");
    }
    for (i = 0; i < count; i++) {
        st = to_si(h->S, "the initial profile", temperatures[i], unit, dim3(0, 0, 0, 1), 1, &k[i],
                   NULL, 0);
        if (st != MFL_OK) {
            free(k);
            return st;
        }
    }
    st = h1_status(h->S, mfl_h1_set_initial_profile(&h->config, k, count, msg, sizeof msg), msg, 0);
    free(k);
    return st;
}

static MflStatus check_side(MflHeat1D *h, MflSide side)
{
    if (side != MFL_SIDE_LEFT && side != MFL_SIDE_RIGHT) {
        return fail(h->S, MFL_ERR_ARGUMENT, "invalid side %d (use MFL_SIDE_LEFT or MFL_SIDE_RIGHT)",
                    (int)side);
    }
    return MFL_OK;
}

MflStatus mfl_heat1d_set_boundary_temperature(MflHeat1D *h, MflSide side, double temperature,
                                              const char *unit)
{
    char given[MFL_H1_GIVEN];
    char msg[H1_MSG];
    double k = 0;
    MflStatus st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    if ((st = check_side(h, side)) != MFL_OK) {
        return st;
    }
    st = to_si(h->S, side == MFL_SIDE_LEFT ? "the left boundary temperature"
                                           : "the right boundary temperature",
               temperature, unit, dim3(0, 0, 0, 1), 1, &k, given, sizeof given);
    if (st != MFL_OK) {
        return st;
    }
    return h1_status(h->S, mfl_h1_set_boundary(&h->config, (MflH1Side)side, MFL_H1_BC_TEMPERATURE, k,
                                               given, msg, sizeof msg),
                     msg, 0);
}

MflStatus mfl_heat1d_set_boundary_insulated(MflHeat1D *h, MflSide side)
{
    char msg[H1_MSG];
    MflStatus st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    if ((st = check_side(h, side)) != MFL_OK) {
        return st;
    }
    return h1_status(h->S, mfl_h1_set_boundary(&h->config, (MflH1Side)side, MFL_H1_BC_INSULATED, 0,
                                               NULL, msg, sizeof msg),
                     msg, 0);
}

MflStatus mfl_heat1d_max_time_step(MflHeat1D *h, const char *unit, double *out)
{
    char msg[H1_MSG];
    double dt = 0;
    MflStatus st;
    H1_BEGIN(h);
    if (out == NULL) {
        return fail(h->S, MFL_ERR_ARGUMENT, "output pointer is NULL");
    }
    st = h1_status(h->S, mfl_h1_max_time_step(h->state_owned ? &h->result.config : &h->config, &dt,
                                              msg, sizeof msg),
                   msg, 1);
    if (st != MFL_OK) {
        return st;
    }
    return from_si(h->S, "max_time_step", dt, unit, dim3(0, 0, 1, 0), out);
}

MflStatus mfl_heat1d_run(MflHeat1D *h)
{
    MflH1Result res;
    char msg[H1_MSG];
    MflH1Status st;
    H1_BEGIN(h);
    H1_CONFIGURABLE(h);
    h->config.memory_limit = h->S->cfg.memory_limit;
    st = mfl_h1_run(&h->config, &res, msg, sizeof msg);
    if (st != MFL_H1_OK) {
        return h1_status(h->S, st, msg, 1); /* previous results are kept */
    }
    mfl_h1_result_free(&h->result);
    h->result = res;
    return MFL_OK;
}

size_t mfl_heat1d_sample_count(const MflHeat1D *h)
{
    return h != NULL && h->result.T != NULL ? h->result.samples : 0;
}

size_t mfl_heat1d_node_count(const MflHeat1D *h)
{
    return h != NULL && h->result.T != NULL ? h->result.nodes : 0;
}

static MflStatus need_results(const MflHeat1D *h)
{
    if (h->result.T == NULL) {
        return fail(h->S, MFL_ERR_NOT_RUN, "the simulation has not run successfully yet "
                                           "(call mfl_heat1d_run)");
    }
    return MFL_OK;
}

static MflStatus check_index(const MflHeat1D *h, const char *what, size_t i, size_t n)
{
    if (i >= n) {
        return fail(h->S, MFL_ERR_ARGUMENT, "%s %lu does not exist (valid: 0 to %lu)", what,
                    (unsigned long)i, (unsigned long)(n - 1));
    }
    return MFL_OK;
}

#define H1_READ(h, out)                                                                   \
    do {                                                                                  \
        MflStatus st_;                                                                    \
        H1_BEGIN(h);                                                                      \
        if ((out) == NULL) {                                                              \
            return fail((h)->S, MFL_ERR_ARGUMENT, "output pointer is NULL");              \
        }                                                                                 \
        if ((st_ = need_results(h)) != MFL_OK) {                                          \
            return st_;                                                                   \
        }                                                                                 \
    } while (0)

MflStatus mfl_heat1d_time(const MflHeat1D *h, size_t sample, const char *unit, double *out)
{
    MflStatus st;
    H1_READ(h, out);
    if ((st = check_index(h, "sample", sample, h->result.samples)) != MFL_OK) {
        return st;
    }
    return from_si(h->S, "time", h->result.times[sample], unit, dim3(0, 0, 1, 0), out);
}

MflStatus mfl_heat1d_position(const MflHeat1D *h, size_t node, const char *unit, double *out)
{
    MflStatus st;
    H1_READ(h, out);
    if ((st = check_index(h, "node", node, h->result.nodes)) != MFL_OK) {
        return st;
    }
    return from_si(h->S, "position", h->result.positions[node], unit, dim3(1, 0, 0, 0), out);
}

MflStatus mfl_heat1d_temperature(const MflHeat1D *h, size_t sample, size_t node, const char *unit,
                                 double *out)
{
    MflStatus st;
    H1_READ(h, out);
    if ((st = check_index(h, "sample", sample, h->result.samples)) != MFL_OK ||
        (st = check_index(h, "node", node, h->result.nodes)) != MFL_OK) {
        return st;
    }
    return from_si(h->S, "temperature", h->result.T[sample * h->result.nodes + node], unit,
                   dim3(0, 0, 0, 1), out);
}

const double *mfl_heat1d_profile(const MflHeat1D *h, size_t sample)
{
    if (h == NULL || h->result.T == NULL || sample >= h->result.samples) {
        return NULL;
    }
    return h->result.T + sample * h->result.nodes;
}

MflStatus mfl_heat1d_first_time_reaching(const MflHeat1D *h, size_t node, double temperature,
                                         const char *unit, const char *time_unit, double *time,
                                         int *reached)
{
    double k = 0;
    size_t s = 0;
    MflStatus st;
    H1_READ(h, time);
    if (reached == NULL) {
        return fail(h->S, MFL_ERR_ARGUMENT, "output pointer is NULL");
    }
    if ((st = check_index(h, "node", node, h->result.nodes)) != MFL_OK) {
        return st;
    }
    st = to_si(h->S, "the threshold temperature", temperature, unit, dim3(0, 0, 0, 1), 1, &k, NULL, 0);
    if (st != MFL_OK) {
        return st;
    }
    *reached = mfl_h1_first_sample_reaching(&h->result, node, k, &s);
    if (!*reached) {
        return MFL_OK;
    }
    return from_si(h->S, "time", h->result.times[s], time_unit, dim3(0, 0, 1, 0), time);
}

MflStatus mfl_heat1d_export(const MflHeat1D *h, const char *csv_path, const char *metadata_path)
{
    MflH1Provenance prov;
    char msg[2400];
    MflStatus st;
    H1_BEGIN(h);
    if (csv_path == NULL || metadata_path == NULL) {
        return fail(h->S, MFL_ERR_ARGUMENT, "file name is NULL");
    }
    if ((st = need_results(h)) != MFL_OK) {
        return st;
    }
    prov.mfl_version = MFL_VERSION_STRING;
    prov.files = (const char *const *)h->sources;
    prov.hashes = h->hashes;
    prov.file_count = h->source_count;
    if (!mfl_h1_export(&h->result, csv_path, metadata_path, &prov, NULL, msg, sizeof msg)) {
        return fail(h->S, MFL_ERR_IO, "export failed: %s; no metadata file was written, so the "
                                      "export is incomplete", msg);
    }
    return MFL_OK;
}
