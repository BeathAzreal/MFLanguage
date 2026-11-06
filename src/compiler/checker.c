/*
 * checker.c - name resolution, unit resolution and dimensional analysis.
 * See checker.h.
 *
 * An expression whose check failed gets MFL_TYPE_ERROR; operators that
 * see an ERROR operand stay silent, so one mistake yields one message.
 *
 * SPDX-License-Identifier: MIT
 */
#include "compiler/checker.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    MflCheckResult *res;
    MflDiagnostic  *diag_tail;
    MflSymbol      *sym_tail;
    int             stop;
} Checker;

#define TYPE_BUF 128

/* ------------------------------------------------------------------------ */
/* Types                                                                     */
/* ------------------------------------------------------------------------ */

static MflType make_type(MflTypeKind kind)
{
    MflType t;
    t.kind = kind;
    t.dim = mfl_dim_none();
    return t;
}

static MflType number_type(MflDim dim)
{
    MflType t = make_type(MFL_TYPE_NUMBER);
    t.dim = dim;
    return t;
}

void mfl_type_format(MflType t, char *buf, size_t size)
{
    char dim[96];
    switch (t.kind) {
    case MFL_TYPE_NUMBER:
        if (mfl_dim_is_none(t.dim)) {
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
    case MFL_TYPE_BOOL: snprintf(buf, size, "Bool"); break;
    case MFL_TYPE_STRING: snprintf(buf, size, "String"); break;
    case MFL_TYPE_NIL: snprintf(buf, size, "Nil"); break;
    case MFL_TYPE_ERROR: snprintf(buf, size, "<error>"); break;
    case MFL_TYPE_UNCHECKED: snprintf(buf, size, "<unchecked>"); break;
    }
}

static int is_number(MflType t) { return t.kind == MFL_TYPE_NUMBER; }
static int is_error(MflType t) { return t.kind == MFL_TYPE_ERROR; }

static int same_type(MflType a, MflType b)
{
    return a.kind == b.kind && (a.kind != MFL_TYPE_NUMBER || mfl_dim_equal(a.dim, b.dim));
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

static void error_at(Checker *c, const MflExpr *at, const char *fmt, ...)
{
    char text[400];
    va_list ap;
    if (c->stop) {
        return;
    }
    if (c->res->diagnostic_count >= MFL_CHECK_MAX_DIAGNOSTICS - 1) {
        add_diag(c, at->line, at->column, "too many errors; stopping");
        c->stop = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    add_diag(c, at->line, at->column, text);
}

/* Same, positioned at a statement. */
static void error_at_stmt(Checker *c, const MflStmt *s, const char *fmt, ...)
{
    char text[400];
    va_list ap;
    MflExpr where;
    memset(&where, 0, sizeof where);
    where.line = s->line;
    where.column = s->column;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    error_at(c, &where, "%s", text);
}

#define TEXT_ARG(t) ((t).length > 40 ? 40 : (int)(t).length), (t).start

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

static int is_builtin(MflText name) { return text_is(name, "print"); }

static MflSymbol *lookup(Checker *c, MflText name)
{
    MflSymbol *s;
    for (s = c->res->symbols; s != NULL; s = s->next) {
        if (text_eq(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

static void declare(Checker *c, const MflStmt *st, MflType type)
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
    s->type = type;
    if (c->sym_tail == NULL) {
        c->res->symbols = s;
    } else {
        c->sym_tail->next = s;
    }
    c->sym_tail = s;
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
    case MFL_EXPR_UNARY: return "an expression";
    case MFL_EXPR_BINARY: return "an expression";
    case MFL_EXPR_MEMBER: return "a property access";
    case MFL_EXPR_INDEX: return "an index expression";
    default: return "an expression";
    }
}

static MflType unit_fail(MflExpr *e)
{
    e->type = make_type(MFL_TYPE_ERROR);
    return e->type;
}

static MflType unit_ok(MflExpr *e, MflDim dim, MflScale scale)
{
    e->type = make_type(MFL_TYPE_UNIT);
    e->type.dim = dim;
    e->unit_scale = scale;
    return e->type;
}

/* Resolves a unit expression tree; records type UNIT and the SI scale. */
static MflType check_unit(Checker *c, MflExpr *e)
{
    MflScale scale = mfl_scale_one();
    MflDim dim = mfl_dim_none();

    switch (e->kind) {
    case MFL_EXPR_IDENT: {
        MflUnitStatus st = mfl_unit_lookup(e->as.text.start, e->as.text.length, &scale, &dim);
        if (st == MFL_UNIT_OK) {
            return unit_ok(e, dim, scale);
        }
        if (st == MFL_UNIT_UNSUPPORTED) {
            error_at(c, e,
                     "unit '%.*s' is not supported yet (temperature scales with an offset "
                     "are planned); use K",
                     TEXT_ARG(e->as.text));
        } else if (lookup(c, e->as.text) != NULL) {
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
            if (!integer_literal(e->as.binary.right, &n)) {
                error_at(c, e->as.binary.right, "unit exponent must be a small integer");
                return unit_fail(e);
            }
            if (!mfl_dim_pow(l.dim, n, &dim)) {
                error_at(c, e, "unit exponent out of range");
                return unit_fail(e);
            }
            return unit_ok(e, dim, mfl_scale_pow(e->as.binary.left->unit_scale, n));
        }
        if (op == MFL_TOK_STAR || op == MFL_TOK_SLASH) {
            l = check_unit(c, e->as.binary.left);
            r = check_unit(c, e->as.binary.right);
            if (is_error(l) || is_error(r)) {
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
            return unit_ok(e, dim, scale);
        }
        break;
    }
    default:
        break;
    }
    error_at(c, e, "expected a unit (such as J/mm), found %s", describe_expr(e));
    return unit_fail(e);
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

/* Type mismatch between the two operands of a binary operator. */
static MflType binary_error(Checker *c, MflExpr *e, const char *what, MflType l, MflType r)
{
    char ls[TYPE_BUF];
    char rs[TYPE_BUF];
    mfl_type_format(l, ls, sizeof ls);
    mfl_type_format(r, rs, sizeof rs);
    error_at(c, e, "%s %s and %s", what, ls, rs);
    return fail(e);
}

static MflType check_binary(Checker *c, MflExpr *e)
{
    MflTokenKind op = e->as.binary.op;
    MflType l = check_expr(c, e->as.binary.left);
    MflType r;
    MflDim dim = mfl_dim_none();
    char ls[TYPE_BUF];

    if (op == MFL_TOK_CARET) {
        long n;
        r = check_expr(c, e->as.binary.right);
        if (is_error(l) || is_error(r)) {
            return fail(e);
        }
        if (!is_number(l) || !is_number(r)) {
            return binary_error(c, e, "operator '^' needs numbers, found", l, r);
        }
        if (!mfl_dim_is_none(r.dim)) {
            mfl_type_format(r, ls, sizeof ls);
            error_at(c, e->as.binary.right, "exponent must be dimensionless, found %s", ls);
            return fail(e);
        }
        if (mfl_dim_is_none(l.dim)) {
            return set_type(e, number_type(mfl_dim_none()));
        }
        if (!integer_literal(e->as.binary.right, &n)) {
            mfl_type_format(l, ls, sizeof ls);
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
        return set_type(e, number_type(dim));
    }

    r = check_expr(c, e->as.binary.right);
    if (is_error(l) || is_error(r)) {
        return fail(e);
    }

    switch (op) {
    case MFL_TOK_AND:
    case MFL_TOK_OR:
        if (l.kind != MFL_TYPE_BOOL || r.kind != MFL_TYPE_BOOL) {
            char what[64];
            snprintf(what, sizeof what, "'%s' needs Bool operands, found", op_text(op));
            return binary_error(c, e, what, l, r);
        }
        return set_type(e, make_type(MFL_TYPE_BOOL));

    case MFL_TOK_PLUS:
    case MFL_TOK_MINUS:
    case MFL_TOK_PERCENT:
    case MFL_TOK_LT:
    case MFL_TOK_LT_EQ:
    case MFL_TOK_GT:
    case MFL_TOK_GT_EQ: {
        int compare = op != MFL_TOK_PLUS && op != MFL_TOK_MINUS && op != MFL_TOK_PERCENT;
        if (!is_number(l) || !is_number(r)) {
            char what[64];
            snprintf(what, sizeof what, "operator '%s' needs numbers, found", op_text(op));
            return binary_error(c, e, what, l, r);
        }
        if (!mfl_dim_equal(l.dim, r.dim)) {
            const char *verb = op == MFL_TOK_PLUS    ? "cannot add"
                               : op == MFL_TOK_MINUS ? "cannot subtract"
                               : op == MFL_TOK_PERCENT ? "cannot take the remainder of"
                                                       : "cannot compare";
            char lt[TYPE_BUF];
            char rt[TYPE_BUF];
            mfl_type_format(l, lt, sizeof lt);
            mfl_type_format(r, rt, sizeof rt);
            error_at(c, e, "%s %s and %s: dimensions differ", verb, lt, rt);
            return fail(e);
        }
        return set_type(e, compare ? make_type(MFL_TYPE_BOOL) : l);
    }

    case MFL_TOK_STAR:
    case MFL_TOK_SLASH:
        if (!is_number(l) || !is_number(r)) {
            char what[64];
            snprintf(what, sizeof what, "operator '%s' needs numbers, found", op_text(op));
            return binary_error(c, e, what, l, r);
        }
        if (!(op == MFL_TOK_STAR ? mfl_dim_mul(l.dim, r.dim, &dim)
                                 : mfl_dim_div(l.dim, r.dim, &dim))) {
            error_at(c, e, "dimension exponent out of range");
            return fail(e);
        }
        return set_type(e, number_type(dim));

    case MFL_TOK_EQ_EQ:
    case MFL_TOK_BANG_EQ:
        if (l.kind == MFL_TYPE_NIL || r.kind == MFL_TYPE_NIL) {
            return set_type(e, make_type(MFL_TYPE_BOOL));
        }
        if (l.kind != r.kind) {
            return binary_error(c, e, "cannot compare", l, r);
        }
        if (is_number(l) && !mfl_dim_equal(l.dim, r.dim)) {
            char lt[TYPE_BUF];
            char rt[TYPE_BUF];
            mfl_type_format(l, lt, sizeof lt);
            mfl_type_format(r, rt, sizeof rt);
            error_at(c, e, "cannot compare %s and %s: dimensions differ", lt, rt);
            return fail(e);
        }
        return set_type(e, make_type(MFL_TYPE_BOOL));

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
    if (!is_number(obj)) {
        mfl_type_format(obj, os, sizeof os);
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
    if (!mfl_dim_equal(obj.dim, unit.dim)) {
        char us[TYPE_BUF];
        mfl_type_format(obj, os, sizeof os);
        mfl_type_format(unit, us, sizeof us);
        error_at(c, call->as.call.args, "cannot convert %s to a %s: dimensions differ", os, us);
        return fail(call);
    }
    if (text_is(name, "to")) {
        return set_type(call, number_type(obj.dim));
    }
    return set_type(call, number_type(mfl_dim_none())); /* value_in */
}

static void check_args(Checker *c, MflExpr *call)
{
    MflExpr *a;
    for (a = call->as.call.args; a != NULL; a = a->next) {
        check_expr(c, a);
    }
}

static MflType check_call(Checker *c, MflExpr *e)
{
    MflExpr *callee = e->as.call.callee;
    char ts[TYPE_BUF];

    if (callee->kind == MFL_EXPR_MEMBER && is_method_name(callee->as.member.name)) {
        return check_method(c, e);
    }
    if (callee->kind == MFL_EXPR_IDENT) {
        MflSymbol *s;
        if (is_builtin(callee->as.text)) {
            callee->type = make_type(MFL_TYPE_UNCHECKED);
            check_args(c, e);
            return set_type(e, make_type(MFL_TYPE_NIL));
        }
        s = lookup(c, callee->as.text);
        check_args(c, e);
        if (s == NULL) {
            error_at(c, callee, "undefined function '%.*s' (v0.1 has only the built-in 'print')",
                     TEXT_ARG(callee->as.text));
            return fail(e);
        }
        callee->type = s->type;
        if (is_error(s->type)) {
            return fail(e);
        }
        mfl_type_format(s->type, ts, sizeof ts);
        error_at(c, callee, "cannot call '%.*s': it is a %s, not a function",
                 TEXT_ARG(callee->as.text), ts);
        return fail(e);
    }
    {
        MflType t = check_expr(c, callee);
        check_args(c, e);
        if (is_error(t)) {
            return fail(e);
        }
        mfl_type_format(t, ts, sizeof ts);
        error_at(c, callee, "cannot call a value of type %s", ts);
        return fail(e);
    }
}

static MflType check_expr(Checker *c, MflExpr *e)
{
    char ts[TYPE_BUF];

    switch (e->kind) {
    case MFL_EXPR_NUMBER:
        return set_type(e, number_type(mfl_dim_none()));
    case MFL_EXPR_STRING:
        return set_type(e, make_type(MFL_TYPE_STRING));
    case MFL_EXPR_BOOL:
        return set_type(e, make_type(MFL_TYPE_BOOL));
    case MFL_EXPR_NIL:
        return set_type(e, make_type(MFL_TYPE_NIL));

    case MFL_EXPR_IDENT: {
        MflSymbol *s;
        if (is_builtin(e->as.text)) {
            error_at(c, e, "'%.*s' is a built-in function; call it, e.g. %.*s(x)",
                     TEXT_ARG(e->as.text), TEXT_ARG(e->as.text));
            return fail(e);
        }
        s = lookup(c, e->as.text);
        if (s == NULL) {
            error_at(c, e, "undefined variable '%.*s'", TEXT_ARG(e->as.text));
            return fail(e);
        }
        return set_type(e, s->type);
    }

    case MFL_EXPR_QUANTITY: {
        MflType u;
        e->as.quantity.value->type = number_type(mfl_dim_none());
        u = check_unit(c, e->as.quantity.unit);
        if (is_error(u)) {
            return fail(e);
        }
        return set_type(e, number_type(u.dim));
    }

    case MFL_EXPR_UNARY: {
        MflType t = check_expr(c, e->as.unary.operand);
        if (is_error(t)) {
            return fail(e);
        }
        if (e->as.unary.op == MFL_TOK_NOT) {
            if (t.kind != MFL_TYPE_BOOL) {
                mfl_type_format(t, ts, sizeof ts);
                error_at(c, e, "'not' needs a Bool, found %s", ts);
                return fail(e);
            }
            return set_type(e, t);
        }
        if (!is_number(t)) {
            mfl_type_format(t, ts, sizeof ts);
            error_at(c, e, "operator '-' needs a number, found %s", ts);
            return fail(e);
        }
        return set_type(e, t);
    }

    case MFL_EXPR_BINARY:
        return check_binary(c, e);

    case MFL_EXPR_CALL:
        return check_call(c, e);

    case MFL_EXPR_MEMBER: {
        MflType t = check_expr(c, e->as.member.object);
        if (is_error(t)) {
            return fail(e);
        }
        if (is_method_name(e->as.member.name)) {
            error_at(c, e, "'.%.*s' must be called with a unit, e.g. x.%.*s(J/mm)",
                     TEXT_ARG(e->as.member.name), TEXT_ARG(e->as.member.name));
        } else {
            mfl_type_format(t, ts, sizeof ts);
            error_at(c, e, "unknown property '%.*s' on %s (v0.1 quantities have: to, value_in)",
                     TEXT_ARG(e->as.member.name), ts);
        }
        return fail(e);
    }

    case MFL_EXPR_INDEX:
        error_at(c, e, "indexing is not supported yet (planned for v0.2)");
        return fail(e);

    case MFL_EXPR_ERROR:
        return fail(e);
    }
    return fail(e);
}

/* ------------------------------------------------------------------------ */
/* Statements                                                                */
/* ------------------------------------------------------------------------ */

static void check_declaration(Checker *c, MflStmt *s)
{
    MflType t = check_expr(c, s->value);
    MflSymbol *prev = lookup(c, s->name);

    if (is_builtin(s->name)) {
        error_at_stmt(c, s, "'%.*s' is a built-in function and cannot be redefined",
                      TEXT_ARG(s->name));
        return;
    }
    if (prev != NULL && prev->line == 0) {
        error_at_stmt(c, s, "'%.*s' is already defined as an input by the host program",
                      TEXT_ARG(s->name));
        return;
    }
    if (prev != NULL) {
        error_at_stmt(c, s, "'%.*s' is already declared at %d:%d", TEXT_ARG(s->name), prev->line,
                      prev->column);
        return;
    }
    declare(c, s, t);
}

static void check_assignment(Checker *c, MflStmt *s)
{
    MflText name = s->target->as.text;
    MflType t = check_expr(c, s->value);
    MflSymbol *sym;
    char old_t[TYPE_BUF];
    char new_t[TYPE_BUF];

    if (is_builtin(name)) {
        error_at(c, s->target, "'%.*s' is a built-in function and cannot be assigned",
                 TEXT_ARG(name));
        return;
    }
    sym = lookup(c, name);
    if (sym == NULL) {
        error_at(c, s->target, "undefined variable '%.*s' (declare it first with 'var')",
                 TEXT_ARG(name));
        return;
    }
    s->target->type = sym->type;
    if (sym->line == 0) {
        error_at(c, s->target, "cannot assign to '%.*s': it is an input provided by the host program",
                 TEXT_ARG(name));
        return;
    }
    if (!sym->is_var) {
        error_at(c, s->target,
                 "cannot assign to '%.*s': it is declared with 'let' at %d:%d "
                 "(use 'var' for a variable that changes)",
                 TEXT_ARG(name), sym->line, sym->column);
        return;
    }
    if (is_error(t) || is_error(sym->type) || same_type(sym->type, t)) {
        return;
    }
    mfl_type_format(sym->type, old_t, sizeof old_t);
    mfl_type_format(t, new_t, sizeof new_t);
    error_at(c, s->target, "cannot assign %s to '%.*s', which is a %s (declared at %d:%d)", new_t,
             TEXT_ARG(name), old_t, sym->line, sym->column);
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

    memset(out, 0, sizeof *out);
    mfl_arena_init(&out->arena, memory_limit);
    memset(&c, 0, sizeof c);
    c.res = out;

    for (i = 0; i < input_count && !c.stop; i++) {
        MflStmt fake;
        memset(&fake, 0, sizeof fake);
        fake.kind = MFL_STMT_LET;
        fake.name = inputs[i].name; /* line 0 marks a host input */
        declare(&c, &fake, inputs[i].type);
    }

    for (s = program->first; s != NULL && !c.stop; s = s->next) {
        switch (s->kind) {
        case MFL_STMT_LET:
        case MFL_STMT_VAR:
            check_declaration(&c, s);
            break;
        case MFL_STMT_ASSIGN:
            check_assignment(&c, s);
            break;
        case MFL_STMT_EXPR:
            check_expr(&c, s->value);
            break;
        }
    }
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
    if (ok) {
        *scale = unit->unit_scale;
        *dim = t.dim;
    } else if (msg_size > 0) {
        snprintf(msg, msg_size, "%s",
                 res.diagnostics != NULL ? res.diagnostics->message : "invalid unit");
    }
    mfl_arena_free(&res.arena);
    return ok;
}

void mfl_check_free(MflCheckResult *result)
{
    mfl_arena_free(&result->arena);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->symbols = NULL;
}

void mfl_check_print_symbols(const MflCheckResult *result, MflWriteFn write, void *ctx)
{
    const MflSymbol *s;
    char line[256];
    char ts[TYPE_BUF];

    for (s = result->symbols; s != NULL; s = s->next) {
        int n;
        mfl_type_format(s->type, ts, sizeof ts);
        n = snprintf(line, sizeof line, "%d:%d %s %.*s: %s\n", s->line, s->column,
                     s->is_var ? "var" : "let", TEXT_ARG(s->name), ts);
        if (n > 0) {
            write(ctx, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
    }
}
