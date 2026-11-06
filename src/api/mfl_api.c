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
#include "lexer/lexer.h"
#include "parser/parser.h"
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

MflStatus mfl_script_run(MflState *S, MflScript *sc)
{
    MflValue *init = NULL;
    MflValue *snap = NULL;
    Result *results = NULL;
    const MflChunk *chunk;
    const MflSymbol *sym;
    MflVmStatus vs;
    MflVmError verr;
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
        vs = mfl_vm_execute(chunk, init, sc->input_count, snap, S->cfg.output, S->cfg.user,
                            &limits, &verr);
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

    /* Keep every top-level variable, numbers in SI. */
    for (i = 0, sym = sc->check.symbols; sym != NULL && i < n; sym = sym->next, i++) {
        Result *r = &results[i];
        r->name = (char *)malloc(sym->name.length + 1);
        if (r->name == NULL) {
            size_t k;
            for (k = 0; k < i; k++) {
                free(results[k].name);
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
