/*
 * checker.c - name resolution, unit resolution, static types and
 * dimensional analysis. See checker.h and docs/language-spec.md.
 *
 * Passes:
 *   1. collect every top-level function signature (functions are hoisted);
 *   2. check top-level statements in order (globals become visible at
 *      their declaration);
 *   3. check function bodies (they see every global and function).
 *
 * An expression whose check failed gets MFL_TYPE_ERROR; operators that
 * see an ERROR operand stay silent, so one mistake yields one message.
 * MFL_TYPE_ANY marks values whose type is only known at run time.
 *
 * SPDX-License-Identifier: MIT
 */
#include "compiler/checker.h"
#include "compiler/compiler.h" /* mfl_unit_text */
#include "modules/natives.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPE_BUF 160

typedef struct {
    MflText name;
    MflType type;
    int     slot;
    int     is_var;
    int     depth;
    int     line;
    int     column;
    const char *what; /* "parameter", "loop variable", "variable" */
} Local;

typedef struct {
    int     file;
    MflText name;
    int     line;
    int     column;
} ModuleImport;

typedef struct {
    MflCheckResult *res;
    MflDiagnostic  *diag_tail;
    MflSymbol      *sym_tail;
    int             stop;

    /* files (v0.3) */
    const MflProgram *program;
    int               file;    /* file of the code being checked */
    ModuleImport     *modules; /* `import name` statements seen so far */
    int               module_count;
    int               module_cap;

    /* current function (NULL: top-level code) */
    MflFnSig       *fn;
    Local          *locals;
    int             local_count;
    int             local_cap;
    int             depth;      /* block nesting inside the current frame */
    int             next_slot;
    int             max_slot;
} Checker;

/* ------------------------------------------------------------------------ */
/* Types                                                                     */
/* ------------------------------------------------------------------------ */

static MflType make_type(MflTypeKind kind)
{
    MflType t;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.dim = mfl_dim_none();
    return t;
}

static MflType number_type(MflDim dim, int affine)
{
    MflType t = make_type(MFL_TYPE_NUMBER);
    t.dim = dim;
    t.affine = (unsigned char)(affine != 0);
    return t;
}

static MflType plain_number(void) { return number_type(mfl_dim_none(), 0); }

static MflType array_type(Checker *c, MflType elem)
{
    MflType t = make_type(MFL_TYPE_ARRAY);
    MflType *e = (MflType *)mfl_arena_alloc(&c->res->arena, sizeof *e);
    if (e == NULL) {
        c->res->out_of_memory = 1;
        c->stop = 1;
        return make_type(MFL_TYPE_ERROR);
    }
    *e = elem;
    t.elem = e;
    return t;
}

static int is_number(MflType t) { return t.kind == MFL_TYPE_NUMBER; }
static int is_error(MflType t) { return t.kind == MFL_TYPE_ERROR; }
static int is_any(MflType t) { return t.kind == MFL_TYPE_ANY; }

void mfl_type_format(MflType t, char *buf, size_t size)
{
    char dim[96];
    char inner[TYPE_BUF];
    switch (t.kind) {
    case MFL_TYPE_NUMBER:
        if (t.affine) {
            snprintf(buf, size, "Quantity[degC]");
        } else if (mfl_dim_is_none(t.dim)) {
            snprintf(buf, size, "Number");
        } else {
            mfl_dim_format(t.dim, dim, sizeof dim);
            snprintf(buf, size, "Quantity[%s]", dim);
        }
        break;
    case MFL_TYPE_UNIT:
        mfl_dim_format(t.dim, dim, sizeof dim);
        snprintf(buf, size, "unit of dimension %s", dim);
        break;
    case MFL_TYPE_ARRAY:
        if (t.elem != NULL) {
            mfl_type_format(*t.elem, inner, sizeof inner);
        } else {
            snprintf(inner, sizeof inner, "Any");
        }
        snprintf(buf, size, "Array[%s]", inner);
        break;
    case MFL_TYPE_FUNCTION:
        if (t.fn != NULL) {
            snprintf(buf, size, "Function %.*s", (int)(t.fn->name.length > 40 ? 40 : t.fn->name.length),
                     t.fn->name.start);
        } else {
            snprintf(buf, size, "Function");
        }
        break;
    case MFL_TYPE_ANY: snprintf(buf, size, "Any"); break;
    case MFL_TYPE_BOOL: snprintf(buf, size, "Bool"); break;
    case MFL_TYPE_STRING: snprintf(buf, size, "String"); break;
    case MFL_TYPE_NIL: snprintf(buf, size, "Nil"); break;
    case MFL_TYPE_ERROR: snprintf(buf, size, "<error>"); break;
    case MFL_TYPE_UNCHECKED: snprintf(buf, size, "<unchecked>"); break;
    }
}

MflCompat mfl_type_compat(MflType declared, MflType actual)
{
    if (declared.kind == MFL_TYPE_ANY || declared.kind == MFL_TYPE_ERROR ||
        actual.kind == MFL_TYPE_ERROR) {
        return MFL_COMPAT_YES;
    }
    if (actual.kind == MFL_TYPE_ANY) {
        return MFL_COMPAT_RUNTIME;
    }
    if (declared.kind != actual.kind) {
        return MFL_COMPAT_NO;
    }
    switch (declared.kind) {
    case MFL_TYPE_NUMBER:
        return mfl_dim_equal(declared.dim, actual.dim) && declared.affine == actual.affine
                   ? MFL_COMPAT_YES
                   : MFL_COMPAT_NO;
    case MFL_TYPE_ARRAY:
        if (declared.elem == NULL || actual.elem == NULL) {
            return MFL_COMPAT_YES;
        }
        if (actual.elem->kind == MFL_TYPE_ANY && declared.elem->kind != MFL_TYPE_ANY) {
            return MFL_COMPAT_RUNTIME; /* e.g. [] or [f(x)] */
        }
        return mfl_type_compat(*declared.elem, *actual.elem);
    default:
        return MFL_COMPAT_YES;
    }
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

static void add_diag(Checker *c, int line, int column, const char *text)
{
    size_t len = strlen(text);
    MflDiagnostic *d = (MflDiagnostic *)mfl_arena_alloc(&c->res->arena, sizeof *d);
    char *msg = (char *)mfl_arena_alloc(&c->res->arena, len + 1);
    if (d == NULL || msg == NULL) {
        c->res->out_of_memory = 1;
        c->stop = 1;
        return;
    }
    memcpy(msg, text, len + 1);
    d->file = c->file;
    d->line = line;
    d->column = column;
    d->message = msg;
    if (c->diag_tail == NULL) {
        c->res->diagnostics = d;
    } else {
        c->diag_tail->next = d;
    }
    c->diag_tail = d;
    c->res->diagnostic_count++;
}

static void error_pos(Checker *c, int line, int column, const char *fmt, ...)
{
    char text[480];
    va_list ap;
    if (c->stop) {
        return;
    }
    if (c->res->diagnostic_count >= MFL_CHECK_MAX_DIAGNOSTICS - 1) {
        add_diag(c, line, column, "too many errors; stopping");
        c->stop = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    add_diag(c, line, column, text);
}

#define error_at(c, at, ...) error_pos((c), (at)->line, (at)->column, __VA_ARGS__)
#define TEXT_ARG(t) ((t).length > 40 ? 40 : (int)(t).length), (t).start

static void fmt_type(MflType t, char *buf) { mfl_type_format(t, buf, TYPE_BUF); }

/* " (declared as Quantity[J/mm])" when the annotation, as written, reads
 * differently from the dimension-based type name; "" otherwise. */
static void declared_as(const MflTypeExpr *te, MflType t, char *buf, size_t size)
{
    char unit[200];
    char shown[TYPE_BUF];
    char written[260];
    buf[0] = '\0';
    if (te == NULL || te->kind != MFL_TE_QUANTITY || !mfl_unit_text(te->unit, unit, sizeof unit)) {
        return;
    }
    snprintf(written, sizeof written, "Quantity[%s]", unit);
    fmt_type(t, shown);
    if (strcmp(written, shown) != 0) {
        snprintf(buf, size, " (declared as %s)", written);
    }
}

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

static int text_is(MflText t, const char *s)
{
    return t.length == strlen(s) && memcmp(t.start, s, t.length) == 0;
}

static int text_eq(MflText a, MflText b)
{
    return a.length == b.length && memcmp(a.start, b.start, a.length) == 0;
}

static int builtin_of(MflText name)
{
    if (text_is(name, "print")) {
        return MFL_BUILTIN_PRINT;
    }
    if (text_is(name, "len")) {
        return MFL_BUILTIN_LEN;
    }
    if (text_is(name, "push")) {
        return MFL_BUILTIN_PUSH;
    }
    return -1;
}

/* May code in file `from` use a name declared in file `to`? */
static int visible(const Checker *c, int to)
{
    const MflProgram *p = c->program;
    if (to < 0 || p == NULL || p->visible == NULL || p->file_count <= 1) {
        return 1;
    }
    if (c->file < 0 || c->file >= p->file_count || to >= p->file_count) {
        return 1;
    }
    return p->visible[c->file * p->file_count + to];
}

static const char *file_name(const Checker *c, int file)
{
    const MflProgram *p = c->program;
    if (p == NULL || p->file_names == NULL || file < 0 || file >= p->file_count) {
        return "?";
    }
    return p->file_names[file];
}

/* "12:3" for a position in the current file, "steel.mfl:12:3" otherwise. */
static const char *where(const Checker *c, int file, int line, int column, char *buf, size_t size)
{
    if (file < 0 || file == c->file || c->program == NULL || c->program->file_count <= 1) {
        snprintf(buf, size, "%d:%d", line, column);
    } else {
        snprintf(buf, size, "%s:%d:%d", file_name(c, file), line, column);
    }
    return buf;
}

/* `import name` of a module in the current file, or NULL. */
static const ModuleImport *imported_module(const Checker *c, MflText name)
{
    int i;
    for (i = 0; i < c->module_count; i++) {
        if (c->modules[i].file == c->file && c->modules[i].name.length == name.length &&
            memcmp(c->modules[i].name.start, name.start, name.length) == 0) {
            return &c->modules[i];
        }
    }
    return NULL;
}

static MflSymbol *find_global(Checker *c, MflText name)
{
    MflSymbol *s;
    for (s = c->res->symbols; s != NULL; s = s->next) {
        if (text_eq(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

static int global_slot(Checker *c, const MflSymbol *sym)
{
    const MflSymbol *s;
    int i = 0;
    for (s = c->res->symbols; s != NULL; s = s->next, i++) {
        if (s == sym) {
            return i;
        }
    }
    return -1;
}

static MflFnSig *find_function(Checker *c, MflText name)
{
    int i;
    for (i = 0; i < c->res->function_count; i++) {
        if (text_eq(c->res->functions[i]->name, name)) {
            return c->res->functions[i];
        }
    }
    return NULL;
}

static Local *find_local(Checker *c, MflText name)
{
    int i;
    for (i = c->local_count - 1; i >= 0; i--) {
        if (text_eq(c->locals[i].name, name)) {
            return &c->locals[i];
        }
    }
    return NULL;
}

static void declare_global(Checker *c, const MflStmt *st, MflType type)
{
    MflSymbol *s = (MflSymbol *)mfl_arena_alloc(&c->res->arena, sizeof *s);
    if (s == NULL) {
        c->res->out_of_memory = 1;
        c->stop = 1;
        return;
    }
    s->name = st->name;
    s->line = st->line;
    s->column = st->column;
    s->is_var = st->kind == MFL_STMT_VAR;
    s->file = st->line == 0 ? -1 : c->file;
    s->type = type;
    if (c->sym_tail == NULL) {
        c->res->symbols = s;
    } else {
        c->sym_tail->next = s;
    }
    c->sym_tail = s;
}

/* Returns the new slot, or -1 on failure. */
static int declare_local(Checker *c, MflText name, MflType type, int is_var, int line, int column,
                         const char *what)
{
    Local *l;
    if (c->local_count == c->local_cap) {
        int cap = c->local_cap ? c->local_cap * 2 : 16;
        Local *p = (Local *)realloc(c->locals, (size_t)cap * sizeof *p);
        if (p == NULL) {
            c->res->out_of_memory = 1;
            c->stop = 1;
            return -1;
        }
        c->locals = p;
        c->local_cap = cap;
    }
    if (c->next_slot >= 65535) {
        error_pos(c, line, column, "too many local variables in one function (limit 65535)");
        return -1;
    }
    l = &c->locals[c->local_count++];
    l->name = name;
    l->type = type;
    l->slot = c->next_slot++;
    l->is_var = is_var;
    l->depth = c->depth;
    l->line = line;
    l->column = column;
    l->what = what;
    if (c->next_slot > c->max_slot) {
        c->max_slot = c->next_slot;
    }
    return l->slot;
}

/* Hidden local (loop counters): no name. */
static int hidden_slot(Checker *c)
{
    MflText none;
    none.start = "";
    none.length = 0;
    return declare_local(c, none, make_type(MFL_TYPE_ANY), 1, 0, 0, "hidden");
}

static void enter_scope(Checker *c) { c->depth++; }

static void leave_scope(Checker *c)
{
    while (c->local_count > 0 && c->locals[c->local_count - 1].depth >= c->depth) {
        c->local_count--;
    }
    c->depth--;
    /* Slots are not reused after a block ends: simple, and frames stay small. */
}

/* Checks that `name` may be declared in the current scope. */
static int may_declare(Checker *c, MflText name, int line, int column, int global)
{
    Local *l;
    MflSymbol *g;
    MflFnSig *f;
    const ModuleImport *m;
    char at[300];
    if (builtin_of(name) >= 0) {
        error_pos(c, line, column, "'%.*s' is a built-in function and cannot be redefined",
                  TEXT_ARG(name));
        return 0;
    }
    m = imported_module(c, name);
    if (m != NULL) {
        error_pos(c, line, column, "'%.*s' is the name of the module imported at %d:%d",
                  TEXT_ARG(name), m->line, m->column);
        return 0;
    }
    if (!global) {
        l = find_local(c, name);
        if (l != NULL && l->what[0] != 'h') {
            error_pos(c, line, column, "'%.*s' is already declared in this %s at %d:%d (as a %s)",
                      TEXT_ARG(name), c->fn != NULL ? "function" : "block", l->line, l->column,
                      l->what);
            return 0;
        }
        return 1; /* locals may shadow globals and functions */
    }
    g = find_global(c, name);
    if (g != NULL && g->line == 0) {
        error_pos(c, line, column, "'%.*s' is already defined as an input by the host program",
                  TEXT_ARG(name));
        return 0;
    }
    if (g != NULL) {
        error_pos(c, line, column, "'%.*s' is already declared at %s", TEXT_ARG(name),
                  where(c, g->file, g->line, g->column, at, sizeof at));
        return 0;
    }
    f = find_function(c, name);
    if (f != NULL) {
        error_pos(c, line, column, "'%.*s' is already declared as a function at %s",
                  TEXT_ARG(name), where(c, f->file, f->line, f->column, at, sizeof at));
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Units                                                                     */
/* ------------------------------------------------------------------------ */

/* Integer literal (optionally negated) -> value; 0 if not one or too big. */
static int integer_literal(const MflExpr *e, long *out)
{
    int negative = 0;
    long v = 0;
    size_t i;
    if (e->kind == MFL_EXPR_UNARY && e->as.unary.op == MFL_TOK_MINUS) {
        negative = 1;
        e = e->as.unary.operand;
    }
    if (e->kind != MFL_EXPR_NUMBER) {
        return 0;
    }
    for (i = 0; i < e->as.text.length; i++) {
        char ch = e->as.text.start[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        v = v * 10 + (ch - '0');
        if (v > 1000000L) {
            return 0;
        }
    }
    *out = negative ? -v : v;
    return 1;
}

static const char *describe_expr(const MflExpr *e)
{
    switch (e->kind) {
    case MFL_EXPR_NUMBER: return "a number";
    case MFL_EXPR_QUANTITY: return "a quantity";
    case MFL_EXPR_STRING: return "a string";
    case MFL_EXPR_BOOL: return "a Bool";
    case MFL_EXPR_NIL: return "nil";
    case MFL_EXPR_CALL: return "a call";
    case MFL_EXPR_MEMBER: return "a property access";
    case MFL_EXPR_INDEX: return "an index expression";
    case MFL_EXPR_ARRAY: return "an array";
    default: return "an expression";
    }
}

static MflType unit_fail(MflExpr *e)
{
    e->type = make_type(MFL_TYPE_ERROR);
    return e->type;
}

static MflType unit_ok(MflExpr *e, MflDim dim, MflScale scale, int affine)
{
    e->type = make_type(MFL_TYPE_UNIT);
    e->type.dim = dim;
    e->type.affine = (unsigned char)(affine != 0);
    e->unit_scale = scale;
    return e->type;
}

static int is_local_or_global_name(Checker *c, MflText name)
{
    return find_local(c, name) != NULL || find_global(c, name) != NULL;
}

/* Resolves a unit expression tree; records type UNIT and the SI scale. */
static MflType check_unit(Checker *c, MflExpr *e)
{
    MflScale scale = mfl_scale_one();
    MflDim dim = mfl_dim_none();

    switch (e->kind) {
    case MFL_EXPR_IDENT: {
        int affine = 0;
        MflUnitStatus st =
            mfl_unit_lookup(e->as.text.start, e->as.text.length, &scale, &dim, &affine);
        if (st == MFL_UNIT_OK) {
            return unit_ok(e, dim, scale, affine);
        }
        if (st == MFL_UNIT_UNSUPPORTED) {
            error_at(c, e, "unit '%.*s' is not supported (Fahrenheit is not part of MFL); use K or degC",
                     TEXT_ARG(e->as.text));
        } else if (is_local_or_global_name(c, e->as.text)) {
            error_at(c, e,
                     "unknown unit '%.*s' ('%.*s' is a variable; to combine a quantity "
                     "with it, put the quantity in parentheses, e.g. (5 mm) / %.*s)",
                     TEXT_ARG(e->as.text), TEXT_ARG(e->as.text), TEXT_ARG(e->as.text));
        } else {
            error_at(c, e, "unknown unit '%.*s'", TEXT_ARG(e->as.text));
        }
        return unit_fail(e);
    }
    case MFL_EXPR_BINARY: {
        MflType l;
        MflType r;
        MflTokenKind op = e->as.binary.op;
        long n;
        if (op == MFL_TOK_CARET) {
            l = check_unit(c, e->as.binary.left);
            if (is_error(l)) {
                return unit_fail(e);
            }
            if (l.affine) {
                error_at(c, e, "'degC' can only be used on its own; use K or delta_degC in "
                               "compound units such as W/(m*K)");
                return unit_fail(e);
            }
            if (!integer_literal(e->as.binary.right, &n)) {
                error_at(c, e->as.binary.right, "unit exponent must be a small integer");
                return unit_fail(e);
            }
            if (!mfl_dim_pow(l.dim, n, &dim)) {
                error_at(c, e, "unit exponent out of range");
                return unit_fail(e);
            }
            return unit_ok(e, dim, mfl_scale_pow(e->as.binary.left->unit_scale, n), 0);
        }
        if (op == MFL_TOK_STAR || op == MFL_TOK_SLASH) {
            l = check_unit(c, e->as.binary.left);
            r = check_unit(c, e->as.binary.right);
            if (is_error(l) || is_error(r)) {
                return unit_fail(e);
            }
            if (l.affine || r.affine) {
                error_at(c, e, "'degC' can only be used on its own; use K or delta_degC in "
                               "compound units such as W/(m*K)");
                return unit_fail(e);
            }
            if (!(op == MFL_TOK_STAR ? mfl_dim_mul(l.dim, r.dim, &dim)
                                     : mfl_dim_div(l.dim, r.dim, &dim))) {
                error_at(c, e, "unit exponent out of range");
                return unit_fail(e);
            }
            scale = op == MFL_TOK_STAR
                        ? mfl_scale_mul(e->as.binary.left->unit_scale, e->as.binary.right->unit_scale)
                        : mfl_scale_div(e->as.binary.left->unit_scale, e->as.binary.right->unit_scale);
            return unit_ok(e, dim, scale, 0);
        }
        break;
    }
    default:
        break;
    }
    error_at(c, e, "expected a unit (such as J/mm), found %s", describe_expr(e));
    return unit_fail(e);
}

/* Type annotation -> type. ERROR (already reported) on failure. */
static MflType resolve_type(Checker *c, const MflTypeExpr *te)
{
    MflType u;
    switch (te->kind) {
    case MFL_TE_NUMBER: return plain_number();
    case MFL_TE_BOOL: return make_type(MFL_TYPE_BOOL);
    case MFL_TE_STRING: return make_type(MFL_TYPE_STRING);
    case MFL_TE_QUANTITY:
        u = check_unit(c, te->unit);
        if (is_error(u)) {
            return u;
        }
        return number_type(u.dim, u.affine);
    case MFL_TE_ARRAY: {
        MflType el = resolve_type(c, te->elem);
        return is_error(el) ? el : array_type(c, el);
    }
    }
    return make_type(MFL_TYPE_ERROR);
}

/* ------------------------------------------------------------------------ */
/* Expressions                                                               */
/* ------------------------------------------------------------------------ */

static MflType check_expr(Checker *c, MflExpr *e);

static MflType set_type(MflExpr *e, MflType t)
{
    e->type = t;
    return t;
}

static MflType fail(MflExpr *e) { return set_type(e, make_type(MFL_TYPE_ERROR)); }

static const char *op_text(MflTokenKind op)
{
    switch (op) {
    case MFL_TOK_PLUS: return "+";
    case MFL_TOK_MINUS: return "-";
    case MFL_TOK_STAR: return "*";
    case MFL_TOK_SLASH: return "/";
    case MFL_TOK_CARET: return "^";
    case MFL_TOK_PERCENT: return "%";
    case MFL_TOK_EQ_EQ: return "==";
    case MFL_TOK_BANG_EQ: return "!=";
    case MFL_TOK_LT: return "<";
    case MFL_TOK_LT_EQ: return "<=";
    case MFL_TOK_GT: return ">";
    case MFL_TOK_GT_EQ: return ">=";
    case MFL_TOK_AND: return "and";
    case MFL_TOK_OR: return "or";
    case MFL_TOK_NOT: return "not";
    default: return "?";
    }
}

static MflType binary_error(Checker *c, MflExpr *e, const char *what, MflType l, MflType r)
{
    char ls[TYPE_BUF];
    char rs[TYPE_BUF];
    fmt_type(l, ls);
    fmt_type(r, rs);
    error_at(c, e, "%s %s and %s", what, ls, rs);
    return fail(e);
}

static MflType affine_error(Checker *c, MflExpr *e, const char *what)
{
    error_at(c, e,
             "cannot %s an absolute temperature in degC; convert it with .to(K) first, or use a "
             "difference (K or delta_degC)",
             what);
    return fail(e);
}

static MflType check_binary(Checker *c, MflExpr *e)
{
    MflTokenKind op = e->as.binary.op;
    MflType l = check_expr(c, e->as.binary.left);
    MflType r = check_expr(c, e->as.binary.right);
    MflDim dim = mfl_dim_none();
    char ls[TYPE_BUF];

    if (is_error(l) || is_error(r)) {
        return fail(e);
    }

    if (op == MFL_TOK_AND || op == MFL_TOK_OR) {
        if ((l.kind != MFL_TYPE_BOOL && !is_any(l)) || (r.kind != MFL_TYPE_BOOL && !is_any(r))) {
            char what[64];
            snprintf(what, sizeof what, "'%s' needs Bool operands, found", op_text(op));
            return binary_error(c, e, what, l, r);
        }
        return set_type(e, make_type(MFL_TYPE_BOOL));
    }

    if (op == MFL_TOK_EQ_EQ || op == MFL_TOK_BANG_EQ) {
        if (l.kind == MFL_TYPE_ARRAY || r.kind == MFL_TYPE_ARRAY) {
            error_at(c, e, "arrays cannot be compared with '%s' (compare their elements)", op_text(op));
            return fail(e);
        }
        if (is_any(l) || is_any(r) || l.kind == MFL_TYPE_NIL || r.kind == MFL_TYPE_NIL) {
            return set_type(e, make_type(MFL_TYPE_BOOL));
        }
        if (l.kind != r.kind) {
            return binary_error(c, e, "cannot compare", l, r);
        }
        if (is_number(l) && !mfl_dim_equal(l.dim, r.dim)) {
            char lt[TYPE_BUF];
            char rt[TYPE_BUF];
            fmt_type(l, lt);
            fmt_type(r, rt);
            error_at(c, e, "cannot compare %s and %s: dimensions differ", lt, rt);
            return fail(e);
        }
        return set_type(e, make_type(MFL_TYPE_BOOL));
    }

    /* Arithmetic and ordering: numbers (or Any). */
    if ((!is_number(l) && !is_any(l)) || (!is_number(r) && !is_any(r))) {
        char what[64];
        snprintf(what, sizeof what, "operator '%s' needs numbers, found", op_text(op));
        return binary_error(c, e, what, l, r);
    }

    if (op == MFL_TOK_CARET) {
        long n;
        if (l.affine) {
            return affine_error(c, e, "raise to a power");
        }
        if (is_number(r) && !mfl_dim_is_none(r.dim)) {
            fmt_type(r, ls);
            error_at(c, e->as.binary.right, "exponent must be dimensionless, found %s", ls);
            return fail(e);
        }
        if (is_any(l)) {
            return set_type(e, make_type(MFL_TYPE_ANY));
        }
        if (mfl_dim_is_none(l.dim)) {
            return set_type(e, plain_number());
        }
        if (!integer_literal(e->as.binary.right, &n)) {
            fmt_type(l, ls);
            error_at(c, e->as.binary.right,
                     "the exponent of a %s must be an integer literal, so that its "
                     "dimension is known before running",
                     ls);
            return fail(e);
        }
        if (!mfl_dim_pow(l.dim, n, &dim)) {
            error_at(c, e, "dimension exponent out of range");
            return fail(e);
        }
        return set_type(e, number_type(dim, 0));
    }

    switch (op) {
    case MFL_TOK_PLUS:
    case MFL_TOK_MINUS:
    case MFL_TOK_PERCENT:
    case MFL_TOK_LT:
    case MFL_TOK_LT_EQ:
    case MFL_TOK_GT:
    case MFL_TOK_GT_EQ: {
        int compare = op != MFL_TOK_PLUS && op != MFL_TOK_MINUS && op != MFL_TOK_PERCENT;
        if (is_any(l) || is_any(r)) {
            MflType known = is_any(l) ? r : l;
            if (compare) {
                return set_type(e, make_type(MFL_TYPE_BOOL));
            }
            if (is_any(known) || known.affine) {
                return set_type(e, make_type(MFL_TYPE_ANY)); /* affinity decided at run time */
            }
            return set_type(e, known);
        }
        if (!mfl_dim_equal(l.dim, r.dim)) {
            const char *verb = op == MFL_TOK_PLUS      ? "cannot add"
                               : op == MFL_TOK_MINUS   ? "cannot subtract"
                               : op == MFL_TOK_PERCENT ? "cannot take the remainder of"
                                                       : "cannot compare";
            char lt[TYPE_BUF];
            char rt[TYPE_BUF];
            fmt_type(l, lt);
            fmt_type(r, rt);
            error_at(c, e, "%s %s and %s: dimensions differ", verb, lt, rt);
            return fail(e);
        }
        if (compare) {
            return set_type(e, make_type(MFL_TYPE_BOOL));
        }
        /* Temperatures on the Celsius scale (docs/units.md section 5). */
        if (op == MFL_TOK_PERCENT && (l.affine || r.affine)) {
            return affine_error(c, e, "take the remainder of");
        }
        if (l.affine && r.affine) {
            if (op == MFL_TOK_PLUS) {
                error_at(c, e, "cannot add two absolute temperatures (degC + degC); add a "
                               "difference in K or delta_degC instead");
                return fail(e);
            }
            return set_type(e, number_type(l.dim, 0)); /* degC - degC: a difference */
        }
        if (r.affine) {
            if (op == MFL_TOK_MINUS) {
                error_at(c, e, "cannot subtract an absolute temperature (degC) from a difference");
                return fail(e);
            }
            return set_type(e, r); /* difference + degC */
        }
        return set_type(e, l); /* degC +- difference stays absolute */
    }

    case MFL_TOK_STAR:
    case MFL_TOK_SLASH:
        if (l.affine || r.affine) {
            return affine_error(c, e, op == MFL_TOK_STAR ? "multiply" : "divide");
        }
        if (is_any(l) || is_any(r)) {
            return set_type(e, make_type(MFL_TYPE_ANY));
        }
        if (!(op == MFL_TOK_STAR ? mfl_dim_mul(l.dim, r.dim, &dim) : mfl_dim_div(l.dim, r.dim, &dim))) {
            error_at(c, e, "dimension exponent out of range");
            return fail(e);
        }
        return set_type(e, number_type(dim, 0));

    default:
        break;
    }
    return fail(e);
}

static int is_method_name(MflText name)
{
    return text_is(name, "to") || text_is(name, "value_in");
}

/* x.to(u) / x.value_in(u) */
static MflType check_method(Checker *c, MflExpr *call)
{
    MflExpr *member = call->as.call.callee;
    MflText name = member->as.member.name;
    MflType obj = check_expr(c, member->as.member.object);
    MflType unit;
    char os[TYPE_BUF];

    member->type = make_type(MFL_TYPE_UNCHECKED);
    if (is_error(obj)) {
        return fail(call);
    }
    if (!is_number(obj) && !is_any(obj)) {
        fmt_type(obj, os);
        error_at(c, member, "'.%.*s' needs a number or quantity, found %s", TEXT_ARG(name), os);
        return fail(call);
    }
    if (call->as.call.argc != 1) {
        error_at(c, call, "'.%.*s' takes exactly one unit, e.g. x.%.*s(J/mm); found %d arguments",
                 TEXT_ARG(name), TEXT_ARG(name), call->as.call.argc);
        return fail(call);
    }
    unit = check_unit(c, call->as.call.args);
    if (is_error(unit)) {
        return fail(call);
    }
    if (is_number(obj) && !mfl_dim_equal(obj.dim, unit.dim)) {
        char us[TYPE_BUF];
        fmt_type(obj, os);
        fmt_type(unit, us);
        error_at(c, call->as.call.args, "cannot convert %s to a %s: dimensions differ", os, us);
        return fail(call);
    }
    if (text_is(name, "to")) {
        return set_type(call, number_type(unit.dim, unit.affine));
    }
    return set_type(call, plain_number()); /* value_in */
}

static void check_args(Checker *c, MflExpr *call)
{
    MflExpr *a;
    for (a = call->as.call.args; a != NULL; a = a->next) {
        check_expr(c, a);
    }
}

static MflExpr *nth_arg(MflExpr *call, int n)
{
    MflExpr *a = call->as.call.args;
    while (a != NULL && n-- > 0) {
        a = a->next;
    }
    return a;
}

/* Arguments against a known signature. */
static MflType check_call_sig(Checker *c, MflExpr *e, const MflFnSig *sig)
{
    MflExpr *a;
    int i = 0;
    int ok = 1;
    check_args(c, e);
    if (e->as.call.argc != sig->arity) {
        error_at(c, e, "'%.*s' takes %d argument%s, but %d %s given", TEXT_ARG(sig->name),
                 sig->arity, sig->arity == 1 ? "" : "s", e->as.call.argc,
                 e->as.call.argc == 1 ? "was" : "were");
        return fail(e);
    }
    for (a = e->as.call.args; a != NULL; a = a->next, i++) {
        if (mfl_type_compat(sig->params[i], a->type) == MFL_COMPAT_NO) {
            char want[TYPE_BUF];
            char got[TYPE_BUF];
            char decl[300];
            const MflParam *prm = sig->decl != NULL ? sig->decl->params : NULL;
            int k;
            for (k = 0; k < i && prm != NULL; k++) {
                prm = prm->next;
            }
            fmt_type(sig->params[i], want);
            fmt_type(a->type, got);
            declared_as(prm != NULL ? prm->type : NULL, sig->params[i], decl, sizeof decl);
            error_at(c, a, "argument %d of '%.*s' ('%.*s') must be %s%s, found %s", i + 1,
                     TEXT_ARG(sig->name), TEXT_ARG(sig->param_names[i]), want, decl, got);
            ok = 0;
        }
    }
    if (!ok) {
        return fail(e);
    }
    return set_type(e, sig->ret);
}

static MflType check_builtin_call(Checker *c, MflExpr *e, int builtin)
{
    MflExpr *callee = e->as.call.callee;
    callee->ref_kind = MFL_REF_BUILTIN;
    callee->ref_index = builtin;
    callee->type = make_type(MFL_TYPE_UNCHECKED);
    check_args(c, e);
    if (builtin == MFL_BUILTIN_PRINT) {
        return set_type(e, make_type(MFL_TYPE_NIL));
    }
    if (builtin == MFL_BUILTIN_LEN) {
        MflExpr *a = e->as.call.args;
        if (e->as.call.argc != 1) {
            error_at(c, e, "'len' takes 1 argument (an array), but %d were given", e->as.call.argc);
            return fail(e);
        }
        if (a->type.kind != MFL_TYPE_ARRAY && !is_any(a->type) && !is_error(a->type)) {
            char got[TYPE_BUF];
            fmt_type(a->type, got);
            error_at(c, a, "'len' needs an array, found %s", got);
            return fail(e);
        }
        return set_type(e, plain_number());
    }
    /* push(array, value) */
    {
        MflExpr *arr = e->as.call.args;
        MflExpr *val = nth_arg(e, 1);
        if (e->as.call.argc != 2) {
            error_at(c, e, "'push' takes 2 arguments (an array and a value), but %d %s given",
                     e->as.call.argc, e->as.call.argc == 1 ? "was" : "were");
            return fail(e);
        }
        if (is_error(arr->type) || is_error(val->type)) {
            return fail(e);
        }
        if (arr->type.kind != MFL_TYPE_ARRAY && !is_any(arr->type)) {
            char got[TYPE_BUF];
            fmt_type(arr->type, got);
            error_at(c, arr, "'push' needs an array as its first argument, found %s", got);
            return fail(e);
        }
        if (arr->type.kind == MFL_TYPE_ARRAY && arr->type.elem != NULL &&
            mfl_type_compat(*arr->type.elem, val->type) == MFL_COMPAT_NO) {
            char want[TYPE_BUF];
            char got[TYPE_BUF];
            fmt_type(*arr->type.elem, want);
            fmt_type(val->type, got);
            error_at(c, val, "cannot push %s into an array of %s", got, want);
            return fail(e);
        }
        return set_type(e, make_type(MFL_TYPE_NIL));
    }
}

/* `module.name` where `module` is a module imported in this file (and not
 * shadowed by a local). Returns the native number, -1 if not a module
 * member, -2 after reporting an error. */
static int module_member(Checker *c, MflExpr *member)
{
    MflExpr *obj = member->as.member.object;
    int idx;
    if (obj->kind != MFL_EXPR_IDENT || find_local(c, obj->as.text) != NULL) {
        return -1;
    }
    if (imported_module(c, obj->as.text) == NULL) {
        if (mfl_native_module_exists(obj->as.text) && find_global(c, obj->as.text) == NULL) {
            error_at(c, obj, "module '%.*s' is not imported; add 'import %.*s' at the top of the file",
                     TEXT_ARG(obj->as.text), TEXT_ARG(obj->as.text));
            return -2;
        }
        return -1;
    }
    idx = mfl_native_find(obj->as.text, member->as.member.name);
    if (idx < 0) {
        char names[400];
        mfl_native_module_members(obj->as.text, names, sizeof names);
        error_at(c, member, "module '%.*s' has no member '%.*s' (it has: %s)", TEXT_ARG(obj->as.text),
                 TEXT_ARG(member->as.member.name), names);
        return -2;
    }
    obj->type = make_type(MFL_TYPE_UNCHECKED);
    return idx;
}

static MflType native_arg_error(Checker *c, MflExpr *a, const MflNative *n, int i, const char *want)
{
    char got[TYPE_BUF];
    fmt_type(a->type, got);
    error_at(c, a, "argument %d of '%s.%s' ('%s') must be %s, found %s", i + 1, n->module, n->name,
             n->params[i].name, want, got);
    return fail(a);
}

static void native_dim_want(const MflNativeParam *p, char *buf, size_t size)
{
    char d[TYPE_BUF];
    mfl_dim_format(p->dim, d, sizeof d);
    if (p->unit != NULL && strcmp(p->unit, d) != 0) {
        snprintf(buf, size, "Quantity[%s] (%s)", d, p->unit);
    } else {
        snprintf(buf, size, "Quantity[%s]", d);
    }
}

static int is_temperature(MflType t)
{
    MflDim k = mfl_dim_none();
    k.exp[4] = 1;
    return is_number(t) && mfl_dim_equal(t.dim, k);
}

/* A literal written in delta_degC (a temperature difference). */
static int is_difference_literal(const MflExpr *a)
{
    char u[64];
    return a->kind == MFL_EXPR_QUANTITY && mfl_unit_text(a->as.quantity.unit, u, sizeof u) &&
           strcmp(u, "delta_degC") == 0;
}

/* Checks one argument against a native parameter. 0 = error reported. */
static int check_native_arg(Checker *c, const MflNative *n, int i, MflExpr *a, const MflExpr *first)
{
    const MflNativeParam *p = &n->params[i];
    MflType t = a->type;
    char want[TYPE_BUF * 2];
    int k;

    if (is_error(t) || is_any(t)) {
        return !is_error(t);
    }
    switch (p->kind) {
    case MFL_NP_NUMBER:
    case MFL_NP_COUNT:
        if (!is_number(t) || !mfl_dim_is_none(t.dim) || t.affine) {
            native_arg_error(c, a, n, i, p->kind == MFL_NP_COUNT ? "a plain Number (a count)"
                                                                 : "a plain Number (dimensionless)");
            return 0;
        }
        return 1;
    case MFL_NP_QUANTITY:
        if (!is_number(t)) {
            native_arg_error(c, a, n, i, "a number or quantity");
            return 0;
        }
        if (t.affine) {
            error_at(c, a, "argument %d of '%s.%s' ('%s') cannot be an absolute temperature in degC; "
                           "convert it with .to(K) first", i + 1, n->module, n->name, p->name);
            return 0;
        }
        if (n->flags & MFL_NF_EVEN_DIM) {
            for (k = 0; k < MFL_DIM_COUNT; k++) {
                if (t.dim.exp[k] % 2 != 0) {
                    native_arg_error(c, a, n, i, "a quantity whose dimension has even exponents "
                                                 "(e.g. m^2, m^2/s^2)");
                    return 0;
                }
            }
        }
        return 1;
    case MFL_NP_ANY_NUMBER:
        if (!is_number(t)) {
            native_arg_error(c, a, n, i, "a number or quantity");
            return 0;
        }
        return 1;
    case MFL_NP_SAME_AS_FIRST:
        if (first == NULL || !is_number(first->type)) {
            return 1;
        }
        if (!is_number(t) || !mfl_dim_equal(t.dim, first->type.dim) ||
            t.affine != first->type.affine) {
            fmt_type(first->type, want);
            strncat(want, " (like argument 1)", sizeof want - strlen(want) - 1);
            native_arg_error(c, a, n, i, want);
            return 0;
        }
        return 1;
    case MFL_NP_DIM:
        if (!is_number(t) || !mfl_dim_equal(t.dim, p->dim) || t.affine) {
            native_dim_want(p, want, sizeof want);
            native_arg_error(c, a, n, i, want);
            return 0;
        }
        return 1;
    case MFL_NP_TEMPERATURE:
        if (!is_temperature(t)) {
            native_arg_error(c, a, n, i, "an absolute temperature (K or degC)");
            return 0;
        }
        if (is_difference_literal(a)) {
            error_at(c, a, "argument %d of '%s.%s' ('%s') must be an absolute temperature (K or "
                           "degC), found a temperature difference in delta_degC",
                     i + 1, n->module, n->name, p->name);
            return 0;
        }
        return 1;
    case MFL_NP_NUMBERS:
    case MFL_NP_DIM_ARRAY:
    case MFL_NP_TEMPERATURES:
        if (t.kind != MFL_TYPE_ARRAY) {
            native_arg_error(c, a, n, i, "an array");
            return 0;
        }
        if (t.elem == NULL || is_any(*t.elem) || is_error(*t.elem)) {
            return 1;
        }
        if (p->kind == MFL_NP_NUMBERS) {
            if (!is_number(*t.elem)) {
                native_arg_error(c, a, n, i, "an array of numbers");
                return 0;
            }
            if ((n->flags & MFL_NF_NO_AFFINE) && t.elem->affine) {
                error_at(c, a, "%s.%s cannot add absolute temperatures (degC); convert them with "
                               ".to(K) first", n->module, n->name);
                return 0;
            }
        } else if (p->kind == MFL_NP_DIM_ARRAY) {
            if (!is_number(*t.elem) || !mfl_dim_equal(t.elem->dim, p->dim) || t.elem->affine) {
                char d[TYPE_BUF];
                native_dim_want(p, d, sizeof d);
                snprintf(want, sizeof want, "Array[%s]", d);
                native_arg_error(c, a, n, i, want);
                return 0;
            }
        } else if (!is_temperature(*t.elem)) {
            native_arg_error(c, a, n, i, "an array of absolute temperatures (K or degC)");
            return 0;
        }
        return 1;
    }
    return 1;
}

static MflType check_native_call(Checker *c, MflExpr *e, int idx)
{
    const MflNative *n = mfl_native_get(idx);
    MflExpr *callee = e->as.call.callee;
    MflExpr *a;
    MflExpr *first = e->as.call.args;
    int i = 0;
    int ok = 1;

    callee->ref_kind = MFL_REF_NATIVE;
    callee->ref_index = idx;
    check_args(c, e);
    if (n->result == MFL_NR_CONSTANT) {
        error_at(c, e, "'%s.%s' is a constant, not a function: write %s.%s without '()'", n->module,
                 n->name, n->module, n->name);
        return fail(e);
    }
    if (e->as.call.argc != n->arity) {
        error_at(c, e, "'%s.%s' takes %d argument%s, but %d %s given", n->module, n->name, n->arity,
                 n->arity == 1 ? "" : "s", e->as.call.argc, e->as.call.argc == 1 ? "was" : "were");
        return fail(e);
    }
    for (a = e->as.call.args; a != NULL; a = a->next, i++) {
        if (!check_native_arg(c, n, i, a, first)) {
            ok = 0;
        }
    }
    if (!ok) {
        return fail(e);
    }
    switch (n->result) {
    case MFL_NR_NIL:
        return set_type(e, make_type(MFL_TYPE_NIL));
    case MFL_NR_NUMBER:
        return set_type(e, plain_number());
    case MFL_NR_DIM:
        return set_type(e, number_type(n->result_dim, 0));
    case MFL_NR_ARRAY_OF_FIRST:
        return set_type(e, array_type(c, first->type));
    case MFL_NR_ELEM_OF_FIRST:
        if (first->type.kind == MFL_TYPE_ARRAY && first->type.elem != NULL) {
            return set_type(e, *first->type.elem);
        }
        return set_type(e, make_type(MFL_TYPE_ANY));
    case MFL_NR_SQRT_FIRST:
        if (is_number(first->type)) {
            MflDim d = mfl_dim_none();
            for (i = 0; i < MFL_DIM_COUNT; i++) {
                d.exp[i] = (signed char)(first->type.dim.exp[i] / 2);
            }
            return set_type(e, number_type(d, 0));
        }
        return set_type(e, make_type(MFL_TYPE_ANY));
    case MFL_NR_DIM_ARRAY:
        return set_type(e, array_type(c, number_type(n->result_dim, 0)));
    default:
        return set_type(e, make_type(MFL_TYPE_ANY));
    }
}

static MflType check_call(Checker *c, MflExpr *e)
{
    MflExpr *callee = e->as.call.callee;
    char ts[TYPE_BUF];
    MflType t;

    if (callee->kind == MFL_EXPR_MEMBER) {
        int idx = module_member(c, callee);
        if (idx == -2) {
            check_args(c, e);
            return fail(e);
        }
        if (idx >= 0) {
            return check_native_call(c, e, idx);
        }
    }
    if (callee->kind == MFL_EXPR_MEMBER && is_method_name(callee->as.member.name)) {
        return check_method(c, e);
    }
    if (callee->kind == MFL_EXPR_IDENT && find_local(c, callee->as.text) == NULL &&
        find_global(c, callee->as.text) == NULL && builtin_of(callee->as.text) >= 0) {
        return check_builtin_call(c, e, builtin_of(callee->as.text));
    }
    if (callee->kind == MFL_EXPR_IDENT && find_local(c, callee->as.text) == NULL &&
        find_global(c, callee->as.text) == NULL && find_function(c, callee->as.text) == NULL) {
        check_args(c, e);
        error_at(c, callee, "undefined function '%.*s'", TEXT_ARG(callee->as.text));
        return fail(e);
    }
    t = check_expr(c, callee);
    if (is_error(t)) {
        check_args(c, e);
        return fail(e);
    }
    if (t.kind == MFL_TYPE_FUNCTION && t.fn != NULL) {
        return check_call_sig(c, e, t.fn);
    }
    if (is_any(t)) {
        check_args(c, e); /* arity and parameter types are checked when it runs */
        return set_type(e, make_type(MFL_TYPE_ANY));
    }
    check_args(c, e);
    fmt_type(t, ts);
    if (callee->kind == MFL_EXPR_IDENT) {
        error_at(c, callee, "cannot call '%.*s': it is a %s, not a function",
                 TEXT_ARG(callee->as.text), ts);
    } else {
        error_at(c, callee, "cannot call a value of type %s", ts);
    }
    return fail(e);
}

static MflType not_imported(Checker *c, MflExpr *e, int file)
{
    error_at(c, e, "'%.*s' is declared in %s, which this file does not import", TEXT_ARG(e->as.text),
             file_name(c, file));
    return fail(e);
}

static MflType check_ident(Checker *c, MflExpr *e)
{
    Local *l = find_local(c, e->as.text);
    MflSymbol *g;
    MflFnSig *f;
    if (l != NULL) {
        e->ref_kind = MFL_REF_LOCAL;
        e->ref_index = l->slot;
        return set_type(e, l->type);
    }
    if (imported_module(c, e->as.text) != NULL) {
        error_at(c, e, "'%.*s' is a module: use its functions, e.g. %.*s.name(...)",
                 TEXT_ARG(e->as.text), TEXT_ARG(e->as.text));
        return fail(e);
    }
    g = find_global(c, e->as.text);
    if (g != NULL && !visible(c, g->file)) {
        return not_imported(c, e, g->file);
    }
    if (g != NULL) {
        e->ref_kind = MFL_REF_GLOBAL;
        e->ref_index = global_slot(c, g);
        return set_type(e, g->type);
    }
    f = find_function(c, e->as.text);
    if (f != NULL && !visible(c, f->file)) {
        return not_imported(c, e, f->file);
    }
    if (f != NULL) {
        MflType t = make_type(MFL_TYPE_FUNCTION);
        t.fn = f;
        e->ref_kind = MFL_REF_FUNCTION;
        e->ref_index = f->index;
        return set_type(e, t);
    }
    if (builtin_of(e->as.text) >= 0) {
        error_at(c, e, "'%.*s' is a built-in function; call it, e.g. %.*s(x)", TEXT_ARG(e->as.text),
                 TEXT_ARG(e->as.text));
        return fail(e);
    }
    error_at(c, e, "undefined variable '%.*s'", TEXT_ARG(e->as.text));
    return fail(e);
}

static MflType check_array(Checker *c, MflExpr *e)
{
    MflExpr *el;
    MflType elem = make_type(MFL_TYPE_ANY);
    const MflExpr *first_known = NULL;
    int i = 0;
    int bad = 0;
    for (el = e->as.array.elems; el != NULL; el = el->next, i++) {
        MflType t = check_expr(c, el);
        if (is_error(t)) {
            bad = 1;
            continue;
        }
        if (is_any(t)) {
            continue;
        }
        if (first_known == NULL) {
            first_known = el;
            elem = t;
        } else if (mfl_type_compat(elem, t) == MFL_COMPAT_NO) {
            char want[TYPE_BUF];
            char got[TYPE_BUF];
            fmt_type(elem, want);
            fmt_type(t, got);
            error_at(c, el,
                     "array elements must all have the same type and dimension: element %d is %s, "
                     "but the first element is %s",
                     i + 1, got, want);
            bad = 1;
        }
    }
    if (bad) {
        return fail(e);
    }
    return set_type(e, array_type(c, elem));
}

static MflType check_index(Checker *c, MflExpr *e)
{
    MflType obj = check_expr(c, e->as.index.object);
    MflType ix = check_expr(c, e->as.index.index);
    char ts[TYPE_BUF];
    if (is_error(obj) || is_error(ix)) {
        return fail(e);
    }
    if (obj.kind != MFL_TYPE_ARRAY && !is_any(obj)) {
        fmt_type(obj, ts);
        error_at(c, e, "cannot index a %s (only arrays can be indexed)", ts);
        return fail(e);
    }
    if (!is_any(ix) && !(is_number(ix) && mfl_dim_is_none(ix.dim))) {
        fmt_type(ix, ts);
        error_at(c, e->as.index.index, "array index must be a plain number (0, 1, 2, ...), found %s",
                 ts);
        return fail(e);
    }
    if (obj.kind == MFL_TYPE_ARRAY && obj.elem != NULL) {
        return set_type(e, *obj.elem);
    }
    return set_type(e, make_type(MFL_TYPE_ANY));
}

static MflType check_expr(Checker *c, MflExpr *e)
{
    char ts[TYPE_BUF];

    switch (e->kind) {
    case MFL_EXPR_NUMBER:
        return set_type(e, plain_number());
    case MFL_EXPR_STRING:
        return set_type(e, make_type(MFL_TYPE_STRING));
    case MFL_EXPR_BOOL:
        return set_type(e, make_type(MFL_TYPE_BOOL));
    case MFL_EXPR_NIL:
        return set_type(e, make_type(MFL_TYPE_NIL));
    case MFL_EXPR_IDENT:
        return check_ident(c, e);
    case MFL_EXPR_QUANTITY: {
        MflType u;
        e->as.quantity.value->type = plain_number();
        u = check_unit(c, e->as.quantity.unit);
        if (is_error(u)) {
            return fail(e);
        }
        return set_type(e, number_type(u.dim, u.affine));
    }
    case MFL_EXPR_UNARY: {
        MflType t = check_expr(c, e->as.unary.operand);
        if (is_error(t)) {
            return fail(e);
        }
        if (e->as.unary.op == MFL_TOK_NOT) {
            if (t.kind != MFL_TYPE_BOOL && !is_any(t)) {
                fmt_type(t, ts);
                error_at(c, e, "'not' needs a Bool, found %s", ts);
                return fail(e);
            }
            return set_type(e, make_type(MFL_TYPE_BOOL));
        }
        if (!is_number(t) && !is_any(t)) {
            fmt_type(t, ts);
            error_at(c, e, "operator '-' needs a number, found %s", ts);
            return fail(e);
        }
        if (t.affine) {
            return affine_error(c, e, "negate");
        }
        return set_type(e, t);
    }
    case MFL_EXPR_BINARY:
        return check_binary(c, e);
    case MFL_EXPR_CALL:
        return check_call(c, e);
    case MFL_EXPR_MEMBER: {
        MflType t;
        int idx = module_member(c, e);
        if (idx == -2) {
            return fail(e);
        }
        if (idx >= 0) {
            const MflNative *n = mfl_native_get(idx);
            if (n->result != MFL_NR_CONSTANT) {
                error_at(c, e, "'%s.%s' is a function; call it, e.g. %s.%s(...)", n->module, n->name,
                         n->module, n->name);
                return fail(e);
            }
            e->ref_kind = MFL_REF_CONSTANT;
            e->ref_index = idx;
            return set_type(e, plain_number());
        }
        t = check_expr(c, e->as.member.object);
        if (is_error(t)) {
            return fail(e);
        }
        if (is_method_name(e->as.member.name)) {
            error_at(c, e, "'.%.*s' must be called with a unit, e.g. x.%.*s(J/mm)",
                     TEXT_ARG(e->as.member.name), TEXT_ARG(e->as.member.name));
        } else {
            fmt_type(t, ts);
            error_at(c, e, "unknown property '%.*s' on %s (quantities have: to, value_in)",
                     TEXT_ARG(e->as.member.name), ts);
        }
        return fail(e);
    }
    case MFL_EXPR_INDEX:
        return check_index(c, e);
    case MFL_EXPR_ARRAY:
        return check_array(c, e);
    case MFL_EXPR_ERROR:
        return fail(e);
    }
    return fail(e);
}

/* ------------------------------------------------------------------------ */
/* Statements                                                                */
/* ------------------------------------------------------------------------ */

static void check_stmts(Checker *c, MflStmt *first);

static void report_compat(Checker *c, const MflExpr *at, const char *what, MflType want,
                          MflType got)
{
    char ws[TYPE_BUF];
    char gs[TYPE_BUF];
    fmt_type(want, ws);
    fmt_type(got, gs);
    error_at(c, at, "%s must be %s, found %s", what, ws, gs);
}

static void check_declaration(Checker *c, MflStmt *s)
{
    MflType t = check_expr(c, s->value);
    MflType decl = t;
    int global = c->fn == NULL && c->depth == 0;

    if (s->annotation != NULL) {
        decl = resolve_type(c, s->annotation);
        if (!is_error(decl) && mfl_type_compat(decl, t) == MFL_COMPAT_NO) {
            char what[96];
            snprintf(what, sizeof what, "the value of '%.*s'", TEXT_ARG(s->name));
            report_compat(c, s->value, what, decl, t);
        }
    } else if (t.kind == MFL_TYPE_NIL && s->kind == MFL_STMT_VAR) {
        decl = make_type(MFL_TYPE_ANY); /* `var x = nil`: type fixed by the first assignment at run time */
    }
    s->decl_type = decl;
    if (!may_declare(c, s->name, s->line, s->column, global)) {
        return;
    }
    s->is_global = global;
    if (global) {
        declare_global(c, s, decl);
        s->slot = global_slot(c, c->sym_tail);
    } else {
        s->slot = declare_local(c, s->name, decl, s->kind == MFL_STMT_VAR, s->line, s->column,
                                "variable");
    }
}

static void check_assignment(Checker *c, MflStmt *s)
{
    MflExpr *target = s->target;
    MflType t = check_expr(c, s->value);

    if (target->kind == MFL_EXPR_INDEX) {
        MflType el = check_index(c, target);
        if (!is_error(el) && !is_error(t) && mfl_type_compat(el, t) == MFL_COMPAT_NO) {
            report_compat(c, s->value, "the new element", el, t);
        }
        return;
    }
    {
        MflText name = target->as.text;
        Local *l = find_local(c, name);
        MflSymbol *g = l == NULL ? find_global(c, name) : NULL;
        MflType decl;
        int is_var;
        int dline = 0;
        int dcol = 0;
        char old_t[TYPE_BUF];
        char new_t[TYPE_BUF];

        if (l == NULL && g == NULL) {
            if (find_function(c, name) != NULL) {
                error_at(c, target, "cannot assign to '%.*s': it is a function", TEXT_ARG(name));
            } else if (builtin_of(name) >= 0) {
                error_at(c, target, "'%.*s' is a built-in function and cannot be assigned",
                         TEXT_ARG(name));
            } else {
                error_at(c, target, "undefined variable '%.*s' (declare it first with 'var')",
                         TEXT_ARG(name));
            }
            return;
        }
        if (l != NULL) {
            target->ref_kind = MFL_REF_LOCAL;
            target->ref_index = l->slot;
            decl = l->type;
            is_var = l->is_var;
            dline = l->line;
            dcol = l->column;
            if (!is_var) {
                error_at(c, target, "cannot assign to '%.*s': it is a %s (immutable)",
                         TEXT_ARG(name), l->what);
                return;
            }
        } else {
            char at[300];
            if (!visible(c, g->file)) {
                not_imported(c, target, g->file);
                return;
            }
            target->ref_kind = MFL_REF_GLOBAL;
            target->ref_index = global_slot(c, g);
            decl = g->type;
            is_var = g->is_var;
            dline = g->line;
            dcol = g->column;
            if (g->file >= 0 && g->file != c->file) {
                error_at(c, target, "cannot assign to '%.*s': it belongs to %s (declared at %s); "
                                    "only code in that file may change it",
                         TEXT_ARG(name), file_name(c, g->file),
                         where(c, g->file, g->line, g->column, at, sizeof at));
                return;
            }
            if (g->line == 0) {
                error_at(c, target, "cannot assign to '%.*s': it is an input provided by the host program",
                         TEXT_ARG(name));
                return;
            }
            if (!is_var) {
                error_at(c, target,
                         "cannot assign to '%.*s': it is declared with 'let' at %d:%d "
                         "(use 'var' for a variable that changes)",
                         TEXT_ARG(name), g->line, g->column);
                return;
            }
        }
        target->type = decl;
        if (is_error(t) || is_error(decl) || mfl_type_compat(decl, t) != MFL_COMPAT_NO) {
            return;
        }
        fmt_type(decl, old_t);
        fmt_type(t, new_t);
        error_at(c, target, "cannot assign %s to '%.*s', which is a %s (declared at %d:%d)", new_t,
                 TEXT_ARG(name), old_t, dline, dcol);
    }
}

static void check_condition(Checker *c, MflExpr *cond, const char *what)
{
    MflType t = check_expr(c, cond);
    if (!is_error(t) && !is_any(t) && t.kind != MFL_TYPE_BOOL) {
        char ts[TYPE_BUF];
        fmt_type(t, ts);
        error_at(c, cond, "the condition of '%s' must be a Bool, found %s (write e.g. x > 0)", what,
                 ts);
    }
}

static void check_block(Checker *c, MflStmt *first)
{
    enter_scope(c);
    check_stmts(c, first);
    leave_scope(c);
}

static void check_for(Checker *c, MflStmt *s)
{
    MflType elem = plain_number();
    MflType first = check_expr(c, s->value);
    enter_scope(c); /* hidden slots and the loop variable live in the loop's scope */
    if (s->range_end != NULL) {
        MflType last = check_expr(c, s->range_end);
        MflExpr *bad = NULL;
        MflType bt = make_type(MFL_TYPE_ERROR);
        if (!is_error(first) && !is_any(first) && !(is_number(first) && mfl_dim_is_none(first.dim))) {
            bad = s->value;
            bt = first;
        } else if (!is_error(last) && !is_any(last) && !(is_number(last) && mfl_dim_is_none(last.dim))) {
            bad = s->range_end;
            bt = last;
        }
        if (bad != NULL) {
            char ts[TYPE_BUF];
            fmt_type(bt, ts);
            error_at(c, bad, "range bounds must be plain numbers (e.g. 0..n), found %s", ts);
        }
    } else if (!is_error(first)) {
        if (first.kind == MFL_TYPE_ARRAY) {
            elem = first.elem != NULL ? *first.elem : make_type(MFL_TYPE_ANY);
        } else if (is_any(first)) {
            elem = make_type(MFL_TYPE_ANY);
        } else {
            char ts[TYPE_BUF];
            fmt_type(first, ts);
            error_at(c, s->value, "'for' can iterate over an array or a range a..b, found %s", ts);
        }
    }
    s->slot2 = hidden_slot(c);
    s->slot3 = hidden_slot(c);
    if (builtin_of(s->name) >= 0) {
        error_at(c, s, "'%.*s' is a built-in function and cannot be redefined", TEXT_ARG(s->name));
    } else if (may_declare(c, s->name, s->line, s->column, 0)) {
        s->slot = declare_local(c, s->name, elem, 0, s->line, s->column, "loop variable");
    }
    check_block(c, s->body);
    leave_scope(c);
}

static void check_return(Checker *c, MflStmt *s)
{
    const MflFnSig *fn = c->fn;
    if (s->value == NULL) {
        if (fn != NULL && fn->ret_annotated) {
            char ws[TYPE_BUF];
            fmt_type(fn->ret, ws);
            error_at(c, s, "'%.*s' must return a %s; 'return' needs a value", TEXT_ARG(fn->name), ws);
        }
        return;
    }
    {
        MflType t = check_expr(c, s->value);
        if (fn != NULL && fn->ret_annotated && mfl_type_compat(fn->ret, t) == MFL_COMPAT_NO) {
            char ws[TYPE_BUF];
            char gs[TYPE_BUF];
            char decl[300];
            fmt_type(fn->ret, ws);
            fmt_type(t, gs);
            declared_as(fn->decl->ret_type, fn->ret, decl, sizeof decl);
            error_at(c, s->value, "the return value of '%.*s' must be %s%s, found %s",
                     TEXT_ARG(fn->name), ws, decl, gs);
        }
    }
}

/* import name (module); file imports were resolved by the loader. */
static void check_import(Checker *c, MflStmt *s)
{
    const ModuleImport *prev;
    if (s->import_path) {
        if (c->program == NULL || c->program->file_count == 0) {
            error_at(c, s, "file imports need the program loader (compiler/loader.h)");
        }
        return;
    }
    if (!mfl_native_module_exists(s->name)) {
        error_at(c, s, "unknown module '%.*s' (available modules: %s; to import a file, write "
                       "import \"%.*s.mfl\")",
                 TEXT_ARG(s->name), mfl_native_module_list(), TEXT_ARG(s->name));
        return;
    }
    prev = imported_module(c, s->name);
    if (prev != NULL) {
        error_at(c, s, "module '%.*s' is already imported at %d:%d", TEXT_ARG(s->name), prev->line,
                 prev->column);
        return;
    }
    if (c->module_count == c->module_cap) {
        int cap = c->module_cap ? c->module_cap * 2 : 8;
        ModuleImport *p = (ModuleImport *)realloc(c->modules, (size_t)cap * sizeof *p);
        if (p == NULL) {
            c->res->out_of_memory = 1;
            c->stop = 1;
            return;
        }
        c->modules = p;
        c->module_cap = cap;
    }
    c->modules[c->module_count].file = c->file;
    c->modules[c->module_count].name = s->name;
    c->modules[c->module_count].line = s->line;
    c->modules[c->module_count].column = s->column;
    c->module_count++;
}

static void check_stmt(Checker *c, MflStmt *s)
{
    switch (s->kind) {
    case MFL_STMT_LET:
    case MFL_STMT_VAR:
        check_declaration(c, s);
        break;
    case MFL_STMT_ASSIGN:
        check_assignment(c, s);
        break;
    case MFL_STMT_EXPR:
        check_expr(c, s->value);
        break;
    case MFL_STMT_FN:
        break; /* bodies are checked in pass 3 */
    case MFL_STMT_RETURN:
        check_return(c, s);
        break;
    case MFL_STMT_IF:
        check_condition(c, s->value, "if");
        check_block(c, s->body);
        if (s->else_body != NULL) {
            if (s->else_body->kind == MFL_STMT_IF) {
                check_stmt(c, s->else_body);
            } else {
                check_block(c, s->else_body->body);
            }
        }
        break;
    case MFL_STMT_WHILE:
        check_condition(c, s->value, "while");
        check_block(c, s->body);
        break;
    case MFL_STMT_FOR:
        check_for(c, s);
        break;
    case MFL_STMT_BLOCK:
        check_block(c, s->body);
        break;
    case MFL_STMT_IMPORT:
        check_import(c, s);
        break;
    }
}

static void check_stmts(Checker *c, MflStmt *first)
{
    MflStmt *s;
    for (s = first; s != NULL && !c->stop; s = s->next) {
        check_stmt(c, s);
    }
}

/* Does every path through these statements end in 'return'? */
static int always_returns(const MflStmt *first)
{
    const MflStmt *s;
    for (s = first; s != NULL; s = s->next) {
        if (s->kind == MFL_STMT_RETURN) {
            return 1;
        }
        if (s->kind == MFL_STMT_BLOCK && always_returns(s->body)) {
            return 1;
        }
        if (s->kind == MFL_STMT_IF && s->else_body != NULL && always_returns(s->body)) {
            const MflStmt *e = s->else_body;
            if ((e->kind == MFL_STMT_BLOCK && always_returns(e->body)) ||
                (e->kind == MFL_STMT_IF && always_returns(e))) {
                return 1;
            }
        }
    }
    return 0;
}

/* Pass 1: signatures. */
static void collect_function(Checker *c, MflStmt *s)
{
    MflFnSig *sig;
    MflFnSig **grown;
    MflParam *prm;
    int i;

    if (builtin_of(s->name) >= 0) {
        error_at(c, s, "'%.*s' is a built-in function and cannot be redefined", TEXT_ARG(s->name));
        return;
    }
    if (find_global(c, s->name) != NULL && find_global(c, s->name)->line == 0) {
        error_at(c, s, "'%.*s' is already defined as an input by the host program", TEXT_ARG(s->name));
        return;
    }
    if (find_function(c, s->name) != NULL) {
        char at[300];
        const MflFnSig *f = find_function(c, s->name);
        error_at(c, s, "function '%.*s' is already declared at %s", TEXT_ARG(s->name),
                 where(c, f->file, f->line, f->column, at, sizeof at));
        return;
    }
    if (c->res->function_count >= 65535) {
        error_at(c, s, "too many functions (limit 65535)");
        return;
    }
    sig = (MflFnSig *)mfl_arena_alloc(&c->res->arena, sizeof *sig);
    grown = (MflFnSig **)mfl_arena_alloc(&c->res->arena,
                                         (size_t)(c->res->function_count + 1) * sizeof *grown);
    if (sig == NULL || grown == NULL) {
        c->res->out_of_memory = 1;
        c->stop = 1;
        return;
    }
    sig->name = s->name;
    sig->index = c->res->function_count;
    sig->arity = s->param_count;
    sig->line = s->line;
    sig->column = s->column;
    sig->file = c->file;
    sig->decl = s;
    sig->params = (MflType *)mfl_arena_alloc(&c->res->arena, (size_t)(s->param_count + 1) * sizeof(MflType));
    sig->param_names = (MflText *)mfl_arena_alloc(&c->res->arena,
                                                  (size_t)(s->param_count + 1) * sizeof(MflText));
    if (sig->params == NULL || sig->param_names == NULL) {
        c->res->out_of_memory = 1;
        c->stop = 1;
        return;
    }
    for (prm = s->params, i = 0; prm != NULL; prm = prm->next, i++) {
        MflParam *q;
        sig->param_names[i] = prm->name;
        sig->params[i] = prm->type != NULL ? resolve_type(c, prm->type) : make_type(MFL_TYPE_ANY);
        if (builtin_of(prm->name) >= 0) {
            error_pos(c, prm->line, prm->column, "'%.*s' is a built-in function and cannot be a parameter name",
                      TEXT_ARG(prm->name));
        }
        for (q = s->params; q != prm; q = q->next) {
            if (text_eq(q->name, prm->name)) {
                error_pos(c, prm->line, prm->column, "duplicate parameter '%.*s'", TEXT_ARG(prm->name));
            }
        }
    }
    sig->ret_annotated = s->ret_type != NULL;
    sig->ret = s->ret_type != NULL ? resolve_type(c, s->ret_type) : make_type(MFL_TYPE_ANY);
    s->slot = sig->index;
    if (c->res->function_count > 0) {
        memcpy(grown, c->res->functions, (size_t)c->res->function_count * sizeof *grown);
    }
    grown[c->res->function_count++] = sig;
    c->res->functions = grown;
}

/* Pass 3: one function body. */
static void check_function_body(Checker *c, MflFnSig *sig)
{
    MflStmt *s = sig->decl;
    MflParam *prm;
    int i;

    c->fn = sig;
    c->file = sig->file;
    c->local_count = 0;
    c->depth = 0;
    c->next_slot = 0;
    c->max_slot = 0;
    enter_scope(c);
    for (prm = s->params, i = 0; prm != NULL; prm = prm->next, i++) {
        declare_local(c, prm->name, sig->params[i], 0, prm->line, prm->column, "parameter");
    }
    check_stmts(c, s->body);
    leave_scope(c);
    s->frame_size = c->max_slot;
    if (sig->ret_annotated && !always_returns(s->body)) {
        char ws[TYPE_BUF];
        fmt_type(sig->ret, ws);
        error_at(c, s,
                 "function '%.*s' is declared to return %s, but can reach the end of its body "
                 "without 'return'",
                 TEXT_ARG(sig->name), ws);
    }
    c->fn = NULL;
}

/* Function bodies are checked last; report everything in source order. */
static void sort_diagnostics(MflCheckResult *res)
{
    MflDiagnostic *sorted = NULL;
    MflDiagnostic *d = res->diagnostics;
    while (d != NULL) { /* stable insertion sort: lists are short (<= 50) */
        MflDiagnostic *next = d->next;
        MflDiagnostic **at = &sorted;
        while (*at != NULL &&
               ((*at)->file < d->file ||
                ((*at)->file == d->file &&
                 ((*at)->line < d->line ||
                  ((*at)->line == d->line && (*at)->column <= d->column))))) {
            at = &(*at)->next;
        }
        d->next = *at;
        *at = d;
        d = next;
    }
    res->diagnostics = sorted;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int mfl_check(MflProgram *program, size_t memory_limit, MflCheckResult *out)
{
    return mfl_check_with_inputs(program, NULL, 0, memory_limit, out);
}

int mfl_check_with_inputs(MflProgram *program, const MflCheckInput *inputs, size_t input_count,
                          size_t memory_limit, MflCheckResult *out)
{
    Checker c;
    MflStmt *s;
    size_t i;
    int k;

    memset(out, 0, sizeof *out);
    mfl_arena_init(&out->arena, memory_limit);
    memset(&c, 0, sizeof c);
    c.res = out;
    c.program = program;

    for (i = 0; i < input_count && !c.stop; i++) {
        MflStmt fake;
        memset(&fake, 0, sizeof fake);
        fake.kind = MFL_STMT_LET;
        fake.name = inputs[i].name; /* line 0 marks a host input */
        declare_global(&c, &fake, inputs[i].type);
    }
    /* 1. function signatures */
    for (s = program->first; s != NULL && !c.stop; s = s->next) {
        if (s->kind == MFL_STMT_FN) {
            c.file = s->file;
            collect_function(&c, s);
        }
    }
    /* 2. top-level code (locals of top-level blocks belong to the main frame) */
    for (s = program->first; s != NULL && !c.stop; s = s->next) {
        c.file = s->file;
        check_stmt(&c, s);
    }
    program->main_frame_size = c.max_slot;
    /* 3. function bodies */
    for (k = 0; k < out->function_count && !c.stop; k++) {
        check_function_body(&c, out->functions[k]);
    }
    free(c.locals);
    free(c.modules);
    sort_diagnostics(out);
    return !out->out_of_memory && out->diagnostic_count == 0;
}

int mfl_check_unit(MflExpr *unit, MflScale *scale, MflDim *dim, char *msg, size_t msg_size)
{
    MflCheckResult res;
    Checker c;
    MflType t;
    int ok;

    memset(&res, 0, sizeof res);
    mfl_arena_init(&res.arena, 0);
    memset(&c, 0, sizeof c);
    c.res = &res;
    t = check_unit(&c, unit);
    ok = t.kind == MFL_TYPE_UNIT && res.diagnostic_count == 0;
    if (ok && t.affine) {
        ok = 0;
        snprintf(msg, msg_size, "absolute temperatures (degC) cannot be passed through the API; use K");
    } else if (ok) {
        *scale = unit->unit_scale;
        *dim = t.dim;
    } else if (msg_size > 0) {
        snprintf(msg, msg_size, "%s",
                 res.diagnostics != NULL ? res.diagnostics->message : "invalid unit");
    }
    mfl_arena_free(&res.arena);
    free(c.locals);
    return ok;
}

void mfl_check_free(MflCheckResult *result)
{
    mfl_arena_free(&result->arena);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->symbols = NULL;
    result->functions = NULL;
    result->function_count = 0;
}

void mfl_check_print_symbols(const MflCheckResult *result, MflWriteFn write, void *ctx)
{
    const MflSymbol *s;
    char line[1024];
    char ts[TYPE_BUF];
    int k;

    for (s = result->symbols; s != NULL; s = s->next) {
        int n;
        mfl_type_format(s->type, ts, sizeof ts);
        n = snprintf(line, sizeof line, "%d:%d %s %.*s: %s\n", s->line, s->column,
                     s->is_var ? "var" : "let", TEXT_ARG(s->name), ts);
        if (n > 0) {
            write(ctx, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
    }
    for (k = 0; k < result->function_count; k++) {
        const MflFnSig *f = result->functions[k];
        size_t pos = 0;
        int i;
        int n = snprintf(line, sizeof line, "%d:%d fn %.*s(", f->line, f->column, TEXT_ARG(f->name));
        pos = n > 0 ? (size_t)n : 0;
        for (i = 0; i < f->arity && pos < sizeof line; i++) {
            mfl_type_format(f->params[i], ts, sizeof ts);
            n = snprintf(line + pos, sizeof line - pos, "%s%.*s: %s", i ? ", " : "",
                         TEXT_ARG(f->param_names[i]), ts);
            pos += n > 0 ? (size_t)n : 0;
        }
        if (pos < sizeof line) {
            mfl_type_format(f->ret, ts, sizeof ts);
            n = snprintf(line + pos, sizeof line - pos, ") -> %s\n", ts);
            pos += n > 0 ? (size_t)n : 0;
        }
        if (pos >= sizeof line) {
            pos = sizeof line - 1;
        }
        write(ctx, line, pos);
    }
}
