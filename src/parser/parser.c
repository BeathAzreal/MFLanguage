/*
 * parser.c - recursive-descent parser for MFL v0.1. See parser.h for the
 * grammar.
 *
 * Error handling follows the "panic mode" scheme: the first error in a
 * statement is recorded, further errors are suppressed until the parser
 * resynchronises at the next statement separator.
 *
 * Out of memory: node constructors return a per-parser placeholder node,
 * the parse continues harmlessly to the end and the result is flagged;
 * the AST is then discarded (program.first = NULL).
 *
 * SPDX-License-Identifier: MIT
 */
#include "parser/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    MflLexer        lx;
    MflToken        cur;
    MflToken        prev;
    MflParseResult *res;
    MflDiagnostic  *diag_tail;
    MflStmt        *stmt_tail;
    int             panic; /* inside an erroneous statement */
    int             stop;  /* too many errors or out of memory */
    int             depth;
    int             block_depth; /* open '{' blocks of statements */
    int             fn_depth;    /* inside a function body */
    int             seen_code;   /* a top-level statement other than 'import' was parsed */
    MflExpr         oom_expr;
} Parser;

/* ------------------------------------------------------------------------ */
/* Tokens                                                                    */
/* ------------------------------------------------------------------------ */

static void advance(Parser *p)
{
    p->prev = p->cur;
    p->cur = mfl_lexer_next(&p->lx);
}

/* Kind of the token n positions after the current one (n >= 1). */
static MflTokenKind peek_kind(const Parser *p, int n)
{
    MflLexer copy = p->lx; /* the lexer is a plain value: cheap lookahead */
    MflTokenKind k = p->cur.kind;
    while (n-- > 0) {
        k = mfl_lexer_next(&copy).kind;
    }
    return k;
}

static int check(const Parser *p, MflTokenKind k) { return p->cur.kind == k; }

static int match(Parser *p, MflTokenKind k)
{
    if (!check(p, k)) {
        return 0;
    }
    advance(p);
    return 1;
}

static int is_separator(MflTokenKind k)
{
    return k == MFL_TOK_NEWLINE || k == MFL_TOK_SEMICOLON || k == MFL_TOK_EOF;
}

/* Human-readable description of a token for messages. */
static void describe(const MflToken *t, char *buf, size_t size)
{
    switch (t->kind) {
    case MFL_TOK_NEWLINE:
        snprintf(buf, size, "end of line");
        break;
    case MFL_TOK_EOF:
        snprintf(buf, size, "end of file");
        break;
    default: {
        int n = t->length > 24 ? 24 : (int)t->length;
        snprintf(buf, size, "'%.*s%s'", n, t->start, t->length > 24 ? "..." : "");
        break;
    }
    }
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

static void add_diagnostic(Parser *p, int line, int column, const char *text)
{
    MflDiagnostic *d;
    char *msg;
    size_t len = strlen(text);

    d = (MflDiagnostic *)mfl_arena_alloc(&p->res->arena, sizeof *d);
    msg = (char *)mfl_arena_alloc(&p->res->arena, len + 1);
    if (d == NULL || msg == NULL) {
        p->res->out_of_memory = 1;
        p->stop = 1;
        return;
    }
    memcpy(msg, text, len + 1);
    d->line = line;
    d->column = column;
    d->message = msg;
    if (p->diag_tail == NULL) {
        p->res->diagnostics = d;
    } else {
        p->diag_tail->next = d;
    }
    p->diag_tail = d;
    p->res->diagnostic_count++;
}

/* Records an error at token t (unless already in panic mode). A lexical
 * ERROR token reports the lexer's own message instead of fmt. */
static void error_at(Parser *p, const MflToken *t, const char *fmt, ...)
{
    char text[256];
    va_list ap;

    if (p->panic || p->stop) {
        return;
    }
    p->panic = 1;
    if (p->res->diagnostic_count >= MFL_PARSE_MAX_DIAGNOSTICS - 1) {
        add_diagnostic(p, t->line, t->column, "too many errors; stopping");
        p->stop = 1;
        return;
    }
    if (t->kind == MFL_TOK_ERROR) {
        add_diagnostic(p, t->line, t->column, t->message);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    add_diagnostic(p, t->line, t->column, text);
}

/* "expected <what>, found <current token>" */
static void error_expected(Parser *p, const char *what)
{
    char found[64];
    describe(&p->cur, found, sizeof found);
    error_at(p, &p->cur, "expected %s, found %s", what, found);
}

static int expect(Parser *p, MflTokenKind k, const char *what)
{
    if (match(p, k)) {
        return 1;
    }
    error_expected(p, what);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Node construction                                                         */
/* ------------------------------------------------------------------------ */

static MflExpr *new_expr(Parser *p, MflExprKind kind, const MflToken *at)
{
    MflExpr *e = (MflExpr *)mfl_arena_alloc(&p->res->arena, sizeof *e);
    if (e == NULL) {
        p->res->out_of_memory = 1;
        p->stop = 1;
        return &p->oom_expr;
    }
    e->kind = kind;
    e->line = at->line;
    e->column = at->column;
    return e;
}

static MflText text_of(const MflToken *t)
{
    MflText x;
    x.start = t->start;
    x.length = t->length;
    return x;
}

static MflExpr *error_expr(Parser *p) { return new_expr(p, MFL_EXPR_ERROR, &p->cur); }

static MflExpr *leaf(Parser *p, MflExprKind kind, const MflToken *t)
{
    MflExpr *e = new_expr(p, kind, t);
    if (e != &p->oom_expr) {
        e->as.text = text_of(t);
    }
    return e;
}

static MflExpr *unary(Parser *p, const MflToken *op, MflExpr *operand)
{
    MflExpr *e = new_expr(p, MFL_EXPR_UNARY, op);
    if (e != &p->oom_expr) {
        e->as.unary.op = op->kind;
        e->as.unary.operand = operand;
    }
    return e;
}

static MflExpr *binary(Parser *p, const MflToken *op, MflExpr *left, MflExpr *right)
{
    MflExpr *e = new_expr(p, MFL_EXPR_BINARY, op);
    if (e != &p->oom_expr) {
        e->as.binary.op = op->kind;
        e->as.binary.left = left;
        e->as.binary.right = right;
    }
    return e;
}

/* Re-positions a node created from a token copy at an expression start. */
static void place_at(MflExpr *e, const MflExpr *start)
{
    e->line = start->line;
    e->column = start->column;
}

/* ------------------------------------------------------------------------ */
/* Nesting guard                                                             */
/* ------------------------------------------------------------------------ */

static int enter(Parser *p)
{
    if (p->depth >= MFL_PARSE_MAX_DEPTH) {
        error_at(p, &p->cur, "expression nested too deeply (limit %d)", MFL_PARSE_MAX_DEPTH);
        return 0;
    }
    p->depth++;
    return 1;
}

static void leave(Parser *p) { p->depth--; }

/* ------------------------------------------------------------------------ */
/* Units                                                                     */
/* ------------------------------------------------------------------------ */

static MflExpr *parse_unit(Parser *p);

static int is_integer_text(const MflToken *t)
{
    size_t i;
    for (i = 0; i < t->length; i++) {
        if (t->start[i] < '0' || t->start[i] > '9') {
            return 0;
        }
    }
    return t->length > 0;
}

static MflExpr *parse_unit_atom(Parser *p)
{
    if (check(p, MFL_TOK_IDENT)) {
        advance(p);
        return leaf(p, MFL_EXPR_IDENT, &p->prev);
    }
    if (check(p, MFL_TOK_LPAREN)) {
        MflExpr *u;
        advance(p);
        if (!enter(p)) {
            return error_expr(p);
        }
        u = parse_unit(p);
        leave(p);
        if (!match(p, MFL_TOK_RPAREN)) {
            char found[64];
            describe(&p->cur, found, sizeof found);
            error_at(p, &p->cur,
                     "expected ')' in unit, found %s (to divide or multiply a quantity "
                     "by an expression, write e.g. (5 mm) / (...))",
                     found);
        }
        return u;
    }
    error_expected(p, "unit name");
    return error_expr(p);
}

static MflExpr *parse_unit_term(Parser *p)
{
    MflExpr *atom = parse_unit_atom(p);
    MflToken caret;
    MflToken sign;
    MflExpr *exponent;
    int negative = 0;

    if (!check(p, MFL_TOK_CARET)) {
        return atom;
    }
    caret = p->cur;
    sign = caret;
    advance(p);
    if (check(p, MFL_TOK_MINUS) || check(p, MFL_TOK_PLUS)) {
        sign = p->cur;
        negative = check(p, MFL_TOK_MINUS);
        advance(p);
    }
    if (!check(p, MFL_TOK_NUMBER) || !is_integer_text(&p->cur)) {
        error_expected(p, "integer unit exponent");
        return error_expr(p);
    }
    advance(p);
    exponent = leaf(p, MFL_EXPR_NUMBER, &p->prev);
    if (negative) {
        exponent = unary(p, &sign, exponent);
    }
    return binary(p, &caret, atom, exponent);
}

/* Does the '(' that follows the current token start a unit group? Only if
 * everything up to its matching ')' can be a unit expression: names, '*',
 * '/', nested parentheses and integer exponents ('^' with an optional sign).
 * So `W/(m*K)` continues the unit, while `(6 W/K) * (T - T_amb)` and
 * `5 m / (2 s)` end it and the parentheses are an ordinary expression. */
static int unit_group_follows(const Parser *p)
{
    MflLexer lx = p->lx; /* positioned after the current token */
    MflToken t = mfl_lexer_next(&lx); /* the '(' */
    int depth = 0;
    int count = 0;
    MflTokenKind prev = MFL_TOK_LPAREN;

    if (t.kind != MFL_TOK_LPAREN) {
        return 0;
    }
    depth = 1;
    while (depth > 0 && count++ < 256) {
        t = mfl_lexer_next(&lx);
        switch (t.kind) {
        case MFL_TOK_IDENT:
            if (prev == MFL_TOK_IDENT || prev == MFL_TOK_RPAREN || prev == MFL_TOK_NUMBER) {
                return 0; /* `(m s)`: two names in a row is not a unit */
            }
            break;
        case MFL_TOK_LPAREN:
            if (prev == MFL_TOK_IDENT || prev == MFL_TOK_RPAREN || prev == MFL_TOK_NUMBER) {
                return 0;
            }
            depth++;
            break;
        case MFL_TOK_RPAREN:
            if (prev != MFL_TOK_IDENT && prev != MFL_TOK_RPAREN && prev != MFL_TOK_NUMBER) {
                return 0;
            }
            depth--;
            break;
        case MFL_TOK_STAR:
        case MFL_TOK_SLASH:
        case MFL_TOK_CARET:
            if (prev != MFL_TOK_IDENT && prev != MFL_TOK_RPAREN && prev != MFL_TOK_NUMBER) {
                return 0;
            }
            break;
        case MFL_TOK_PLUS:
        case MFL_TOK_MINUS:
            if (prev != MFL_TOK_CARET) {
                return 0; /* a sign only right after '^' */
            }
            break;
        case MFL_TOK_NUMBER:
            if (prev != MFL_TOK_CARET && prev != MFL_TOK_PLUS && prev != MFL_TOK_MINUS) {
                return 0; /* a number only as an exponent */
            }
            break;
        default:
            return 0;
        }
        prev = t.kind;
    }
    return depth == 0;
}

static MflExpr *parse_unit(Parser *p)
{
    MflExpr *left = parse_unit_term(p);
    while ((check(p, MFL_TOK_STAR) || check(p, MFL_TOK_SLASH)) && !p->panic) {
        MflTokenKind after = peek_kind(p, 1);
        MflToken op;
        if (after == MFL_TOK_LPAREN) {
            if (!unit_group_follows(p)) {
                break;
            }
        } else if (after != MFL_TOK_IDENT) {
            break; /* `5 mm * 2`: the operator belongs to the expression */
        }
        op = p->cur;
        advance(p);
        left = binary(p, &op, left, parse_unit_term(p));
    }
    return left;
}

/* ------------------------------------------------------------------------ */
/* Expressions                                                               */
/* ------------------------------------------------------------------------ */

static MflExpr *parse_expr(Parser *p);
static MflExpr *parse_unary(Parser *p);

static MflExpr *parse_primary(Parser *p)
{
    MflToken t = p->cur;

    switch (t.kind) {
    case MFL_TOK_NUMBER: {
        MflExpr *num;
        advance(p);
        num = leaf(p, MFL_EXPR_NUMBER, &t);
        if (check(p, MFL_TOK_IDENT)) {
            MflExpr *q = new_expr(p, MFL_EXPR_QUANTITY, &t);
            MflExpr *unit = parse_unit(p);
            if (q != &p->oom_expr) {
                q->as.quantity.value = num;
                q->as.quantity.unit = unit;
            }
            return q;
        }
        return num;
    }
    case MFL_TOK_STRING:
        advance(p);
        return leaf(p, MFL_EXPR_STRING, &t);
    case MFL_TOK_IDENT:
        advance(p);
        return leaf(p, MFL_EXPR_IDENT, &t);
    case MFL_TOK_TRUE:
    case MFL_TOK_FALSE: {
        MflExpr *e;
        advance(p);
        e = new_expr(p, MFL_EXPR_BOOL, &t);
        if (e != &p->oom_expr) {
            e->as.boolean = t.kind == MFL_TOK_TRUE;
        }
        return e;
    }
    case MFL_TOK_NIL:
        advance(p);
        return new_expr(p, MFL_EXPR_NIL, &t);
    case MFL_TOK_LBRACKET: { /* array literal (v0.2) */
        MflExpr *arr = new_expr(p, MFL_EXPR_ARRAY, &t);
        MflExpr *first = NULL;
        MflExpr *last = NULL;
        int count = 0;
        advance(p);
        if (!enter(p)) {
            return error_expr(p);
        }
        if (!check(p, MFL_TOK_RBRACKET)) {
            do {
                MflExpr *el = parse_expr(p);
                if (last == NULL) {
                    first = el;
                } else {
                    last->next = el;
                }
                last = el;
                count++;
            } while (!p->panic && match(p, MFL_TOK_COMMA));
        }
        leave(p);
        if (!p->panic) {
            expect(p, MFL_TOK_RBRACKET, "']' to close the array");
        }
        if (arr != &p->oom_expr) {
            arr->as.array.elems = first;
            arr->as.array.count = count;
        }
        return arr;
    }
    case MFL_TOK_LPAREN: {
        MflExpr *e;
        advance(p);
        if (!enter(p)) {
            return error_expr(p);
        }
        e = parse_expr(p);
        leave(p);
        if (!check(p, MFL_TOK_RPAREN)) {
            char found[64];
            describe(&p->cur, found, sizeof found);
            error_at(p, &p->cur, "expected ')' to close '(' at %d:%d, found %s", t.line,
                     t.column, found);
            return e;
        }
        advance(p);
        return e;
    }
    default:
        error_expected(p, "expression");
        return error_expr(p);
    }
}

static MflExpr *parse_postfix(Parser *p)
{
    MflExpr *e = parse_primary(p);

    while (!p->panic) {
        if (check(p, MFL_TOK_LPAREN)) {
            MflToken open = p->cur;
            MflExpr *call = new_expr(p, MFL_EXPR_CALL, &open);
            MflExpr *first = NULL;
            MflExpr *last = NULL;
            int argc = 0;
            advance(p);
            if (!check(p, MFL_TOK_RPAREN)) {
                int named = 0;
                do {
                    MflExpr *arg;
                    MflToken label;
                    int has_label = 0;
                    if (!enter(p)) {
                        return error_expr(p);
                    }
                    /* name: value (v0.4) */
                    if (check(p, MFL_TOK_IDENT) && peek_kind(p, 1) == MFL_TOK_COLON) {
                        label = p->cur;
                        has_label = 1;
                        advance(p);
                        advance(p);
                        named = 1;
                    } else if (named) {
                        error_at(p, &p->cur, "a positional argument cannot follow a named one "
                                             "(write the positional arguments first)");
                        leave(p);
                        return error_expr(p);
                    }
                    arg = parse_expr(p);
                    leave(p);
                    if (has_label && arg != &p->oom_expr) {
                        arg->label = text_of(&label);
                    }
                    if (last == NULL) {
                        first = arg;
                    } else {
                        last->next = arg;
                    }
                    last = arg;
                    argc++;
                } while (!p->panic && match(p, MFL_TOK_COMMA));
            }
            if (!p->panic) {
                expect(p, MFL_TOK_RPAREN, "')' after arguments");
            }
            if (call != &p->oom_expr) {
                call->as.call.callee = e;
                call->as.call.args = first;
                call->as.call.argc = argc;
                place_at(call, e);
            }
            e = call;
        } else if (check(p, MFL_TOK_DOT)) {
            MflToken dot = p->cur;
            MflExpr *m;
            advance(p);
            if (!expect(p, MFL_TOK_IDENT, "property name after '.'")) {
                return e;
            }
            m = new_expr(p, MFL_EXPR_MEMBER, &dot);
            if (m != &p->oom_expr) {
                m->as.member.object = e;
                m->as.member.name = text_of(&p->prev);
                place_at(m, e);
            }
            e = m;
        } else if (check(p, MFL_TOK_LBRACKET)) {
            MflToken open = p->cur;
            MflExpr *ix = new_expr(p, MFL_EXPR_INDEX, &open);
            MflExpr *index;
            advance(p);
            if (!enter(p)) {
                return error_expr(p);
            }
            index = parse_expr(p);
            leave(p);
            if (!p->panic) {
                expect(p, MFL_TOK_RBRACKET, "']' after index");
            }
            if (ix != &p->oom_expr) {
                ix->as.index.object = e;
                ix->as.index.index = index;
                place_at(ix, e);
            }
            e = ix;
        } else {
            break;
        }
    }
    return e;
}

static MflExpr *parse_power(Parser *p)
{
    MflExpr *base = parse_postfix(p);
    MflToken op;
    MflExpr *exponent;

    if (!check(p, MFL_TOK_CARET) || p->panic) {
        return base;
    }
    op = p->cur;
    advance(p);
    if (!enter(p)) {
        return error_expr(p);
    }
    exponent = parse_unary(p); /* right-assoc; allows 2^-1 */
    leave(p);
    return binary(p, &op, base, exponent);
}

static MflExpr *parse_unary(Parser *p)
{
    if (check(p, MFL_TOK_MINUS) || check(p, MFL_TOK_NOT)) {
        MflToken op = p->cur;
        MflExpr *operand;
        advance(p);
        if (!enter(p)) {
            return error_expr(p);
        }
        operand = parse_unary(p);
        leave(p);
        return unary(p, &op, operand);
    }
    return parse_power(p);
}

typedef MflExpr *(*LevelFn)(Parser *);

/* left-assoc binary level: next (op next)* with op in ops[0..n) */
static MflExpr *parse_level(Parser *p, LevelFn next, const MflTokenKind *ops, int n)
{
    MflExpr *left = next(p);
    for (;;) {
        int i;
        int found = 0;
        if (p->panic) {
            return left;
        }
        for (i = 0; i < n; i++) {
            if (check(p, ops[i])) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return left;
        }
        {
            MflToken op = p->cur;
            advance(p);
            left = binary(p, &op, left, next(p));
        }
    }
}

static MflExpr *parse_factor(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_STAR, MFL_TOK_SLASH, MFL_TOK_PERCENT};
    return parse_level(p, parse_unary, ops, 3);
}

static MflExpr *parse_term(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_PLUS, MFL_TOK_MINUS};
    return parse_level(p, parse_factor, ops, 2);
}

static MflExpr *parse_comparison(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_LT, MFL_TOK_LT_EQ, MFL_TOK_GT, MFL_TOK_GT_EQ};
    return parse_level(p, parse_term, ops, 4);
}

static MflExpr *parse_equality(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_EQ_EQ, MFL_TOK_BANG_EQ};
    return parse_level(p, parse_comparison, ops, 2);
}

static MflExpr *parse_and(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_AND};
    return parse_level(p, parse_equality, ops, 1);
}

static MflExpr *parse_or(Parser *p)
{
    static const MflTokenKind ops[] = {MFL_TOK_OR};
    return parse_level(p, parse_and, ops, 1);
}

static MflExpr *parse_expr(Parser *p) { return parse_or(p); }

/* ------------------------------------------------------------------------ */
/* Statements                                                                */
/* ------------------------------------------------------------------------ */

static MflStmt *new_stmt(Parser *p, MflStmtKind kind, const MflToken *at)
{
    MflStmt *s = (MflStmt *)mfl_arena_alloc(&p->res->arena, sizeof *s);
    if (s == NULL) {
        p->res->out_of_memory = 1;
        p->stop = 1;
        return NULL;
    }
    s->kind = kind;
    s->line = at->line;
    s->column = at->column;
    return s;
}

static MflStmt *parse_statement(Parser *p);

static int at_statement_end(const Parser *p)
{
    return is_separator(p->cur.kind) || (p->block_depth > 0 && check(p, MFL_TOK_RBRACE));
}

/* Type annotation: Number | Bool | String | Quantity[unit] | Array[type] */
static MflTypeExpr *parse_type(Parser *p)
{
    MflToken t = p->cur;
    MflTypeExpr *te;

    if (!check(p, MFL_TOK_IDENT)) {
        error_expected(p, "a type (Number, Quantity[unit], Bool, String or Array[type])");
        return NULL;
    }
    te = (MflTypeExpr *)mfl_arena_alloc(&p->res->arena, sizeof *te);
    if (te == NULL) {
        p->res->out_of_memory = 1;
        p->stop = 1;
        return NULL;
    }
    te->line = t.line;
    te->column = t.column;
    advance(p);
#define IS(word) (t.length == strlen(word) && memcmp(t.start, word, t.length) == 0)
    if (IS("Number")) {
        te->kind = MFL_TE_NUMBER;
    } else if (IS("Bool")) {
        te->kind = MFL_TE_BOOL;
    } else if (IS("String")) {
        te->kind = MFL_TE_STRING;
    } else if (IS("Quantity") || IS("Array")) {
        int quantity = IS("Quantity");
        if (!expect(p, MFL_TOK_LBRACKET, quantity ? "'[' after Quantity (e.g. Quantity[m/s])"
                                                  : "'[' after Array (e.g. Array[Number])")) {
            return NULL;
        }
        if (!enter(p)) {
            return NULL;
        }
        if (quantity) {
            te->kind = MFL_TE_QUANTITY;
            if (!check(p, MFL_TOK_IDENT) && !check(p, MFL_TOK_LPAREN)) {
                error_expected(p, "a unit inside Quantity[...]");
                leave(p);
                return NULL;
            }
            te->unit = parse_unit(p);
        } else {
            te->kind = MFL_TE_ARRAY;
            te->elem = parse_type(p);
        }
        leave(p);
        if (p->panic || !expect(p, MFL_TOK_RBRACKET, "']' to close the type")) {
            return NULL;
        }
    } else {
        char found[64];
        describe(&t, found, sizeof found);
        error_at(p, &t, "unknown type %s (expected Number, Quantity[unit], Bool, String or Array[type])",
                 found);
        return NULL;
    }
#undef IS
    return te;
}

/* '{' statements '}' -> first statement of the block (NULL if empty). */
static MflStmt *parse_block(Parser *p, const char *what)
{
    MflToken open = p->cur;
    MflStmt *first = NULL;
    MflStmt *last = NULL;

    if (!check(p, MFL_TOK_LBRACE)) {
        error_expected(p, what);
        return NULL;
    }
    advance(p);
    if (!enter(p)) {
        return NULL;
    }
    p->block_depth++;
    for (;;) {
        MflStmt *s;
        while (match(p, MFL_TOK_NEWLINE) || match(p, MFL_TOK_SEMICOLON)) {
        }
        if (check(p, MFL_TOK_RBRACE) || check(p, MFL_TOK_EOF) || p->stop) {
            break;
        }
        s = parse_statement(p);
        if (p->panic) {
            /* recover inside the block: skip to the end of the statement */
            int braces = 0;
            while (!check(p, MFL_TOK_EOF)) {
                if (braces == 0 && (check(p, MFL_TOK_NEWLINE) || check(p, MFL_TOK_SEMICOLON) ||
                                    check(p, MFL_TOK_RBRACE))) {
                    break;
                }
                if (check(p, MFL_TOK_LBRACE)) {
                    braces++;
                } else if (check(p, MFL_TOK_RBRACE)) {
                    braces--;
                }
                advance(p);
            }
            p->panic = 0;
            continue;
        }
        if (s != NULL) {
            if (last == NULL) {
                first = s;
            } else {
                last->next = s;
            }
            last = s;
        }
    }
    p->block_depth--;
    leave(p);
    if (!match(p, MFL_TOK_RBRACE)) {
        char found[64];
        describe(&p->cur, found, sizeof found);
        error_at(p, &p->cur, "expected '}' to close the block opened at %d:%d, found %s",
                 open.line, open.column, found);
    }
    return first;
}

static MflStmt *parse_if(Parser *p, const MflToken *start)
{
    MflStmt *s = new_stmt(p, MFL_STMT_IF, start);
    MflExpr *cond;
    MflStmt *then_body;

    cond = parse_expr(p);
    if (p->panic) {
        return NULL;
    }
    then_body = parse_block(p, "'{' after the condition of 'if'");
    if (p->panic) {
        return NULL;
    }
    if (s != NULL) {
        s->value = cond;
        s->body = then_body;
    }
    /* `}` NEWLINE `else` continues the same statement */
    if (check(p, MFL_TOK_NEWLINE) && peek_kind(p, 1) == MFL_TOK_ELSE) {
        advance(p);
    }
    if (match(p, MFL_TOK_ELSE)) {
        MflToken at = p->prev;
        MflStmt *else_body;
        if (match(p, MFL_TOK_IF)) {
            if (!enter(p)) {
                return NULL;
            }
            else_body = parse_if(p, &p->prev);
            leave(p);
        } else {
            MflStmt *blk = new_stmt(p, MFL_STMT_BLOCK, &at);
            MflStmt *b = parse_block(p, "'{' or 'if' after 'else'");
            if (blk != NULL) {
                blk->body = b;
            }
            else_body = blk;
        }
        if (s != NULL) {
            s->else_body = else_body;
        }
    }
    return s;
}

static MflParam *parse_params(Parser *p, int *count)
{
    MflParam *first = NULL;
    MflParam *last = NULL;
    *count = 0;
    if (check(p, MFL_TOK_RPAREN)) {
        return NULL;
    }
    do {
        MflParam *prm;
        if (!expect(p, MFL_TOK_IDENT, "parameter name")) {
            return first;
        }
        prm = (MflParam *)mfl_arena_alloc(&p->res->arena, sizeof *prm);
        if (prm == NULL) {
            p->res->out_of_memory = 1;
            p->stop = 1;
            return first;
        }
        prm->name = text_of(&p->prev);
        prm->line = p->prev.line;
        prm->column = p->prev.column;
        if (match(p, MFL_TOK_COLON)) {
            prm->type = parse_type(p);
            if (p->panic) {
                return first;
            }
        }
        if (last == NULL) {
            first = prm;
        } else {
            last->next = prm;
        }
        last = prm;
        (*count)++;
    } while (match(p, MFL_TOK_COMMA));
    return first;
}

static MflStmt *parse_fn(Parser *p, const MflToken *start)
{
    MflStmt *s;
    MflToken name;
    MflParam *params;
    int count = 0;
    MflTypeExpr *ret = NULL;
    MflStmt *body;

    if (p->block_depth > 0 || p->fn_depth > 0) {
        error_at(p, start, "functions can only be declared at the top level (no nested functions in v0.2)");
        return NULL;
    }
    if (!expect(p, MFL_TOK_IDENT, "function name after 'fn'")) {
        return NULL;
    }
    name = p->prev;
    if (!expect(p, MFL_TOK_LPAREN, "'(' after the function name")) {
        return NULL;
    }
    params = parse_params(p, &count);
    if (p->panic || !expect(p, MFL_TOK_RPAREN, "')' after the parameters")) {
        return NULL;
    }
    /* The return type may start the next line:
     *     fn f(a: Quantity[m],
     *          b: Quantity[s])
     *         -> Quantity[m/s] { ... }
     * ('->' cannot begin a statement, so this never changes a valid program.) */
    if (check(p, MFL_TOK_NEWLINE) && peek_kind(p, 1) == MFL_TOK_ARROW) {
        advance(p);
    }
    if (match(p, MFL_TOK_ARROW)) {
        ret = parse_type(p);
        if (p->panic) {
            return NULL;
        }
    }
    p->fn_depth++;
    body = parse_block(p, "'{' to start the function body");
    p->fn_depth--;
    if (p->panic) {
        return NULL;
    }
    s = new_stmt(p, MFL_STMT_FN, start);
    if (s != NULL) {
        s->name = text_of(&name);
        s->params = params;
        s->param_count = count;
        s->ret_type = ret;
        s->body = body;
    }
    return s;
}

static MflStmt *parse_statement(Parser *p)
{
    MflToken start = p->cur;
    MflStmt *s = NULL;

    switch (start.kind) {
    case MFL_TOK_LET:
    case MFL_TOK_VAR: {
        MflToken name;
        MflExpr *value;
        MflTypeExpr *annotation = NULL;
        const char *kw = start.kind == MFL_TOK_LET ? "let" : "var";
        char what[64];
        advance(p);
        snprintf(what, sizeof what, "variable name after '%s'", kw);
        if (!expect(p, MFL_TOK_IDENT, what)) {
            return NULL;
        }
        name = p->prev;
        if (match(p, MFL_TOK_COLON)) {
            annotation = parse_type(p);
            if (p->panic) {
                return NULL;
            }
        }
        snprintf(what, sizeof what, "'=' after '%.*s'", name.length > 24 ? 24 : (int)name.length,
                 name.start);
        if (!expect(p, MFL_TOK_EQ, what)) {
            return NULL;
        }
        value = parse_expr(p);
        s = new_stmt(p, start.kind == MFL_TOK_LET ? MFL_STMT_LET : MFL_STMT_VAR, &start);
        if (s != NULL) {
            s->name = text_of(&name);
            s->annotation = annotation;
            s->value = value;
        }
        break;
    }
    case MFL_TOK_FN:
        advance(p);
        s = parse_fn(p, &start);
        break;
    case MFL_TOK_RETURN: {
        MflExpr *value = NULL;
        advance(p);
        if (p->fn_depth == 0) {
            error_at(p, &start, "'return' outside a function");
            return NULL;
        }
        if (!at_statement_end(p)) {
            value = parse_expr(p);
        }
        s = new_stmt(p, MFL_STMT_RETURN, &start);
        if (s != NULL) {
            s->value = value;
        }
        break;
    }
    case MFL_TOK_IF:
        advance(p);
        if (!enter(p)) {
            return NULL;
        }
        s = parse_if(p, &start);
        leave(p);
        break;
    case MFL_TOK_WHILE: {
        MflExpr *cond;
        MflStmt *body;
        advance(p);
        cond = parse_expr(p);
        if (p->panic) {
            return NULL;
        }
        if (!enter(p)) {
            return NULL;
        }
        body = parse_block(p, "'{' after the condition of 'while'");
        leave(p);
        s = new_stmt(p, MFL_STMT_WHILE, &start);
        if (s != NULL) {
            s->value = cond;
            s->body = body;
        }
        break;
    }
    case MFL_TOK_FOR: {
        MflToken var;
        MflExpr *first;
        MflExpr *end = NULL;
        MflStmt *body;
        advance(p);
        if (!expect(p, MFL_TOK_IDENT, "loop variable name after 'for'")) {
            return NULL;
        }
        var = p->prev;
        if (!expect(p, MFL_TOK_IN, "'in' after the loop variable")) {
            return NULL;
        }
        first = parse_expr(p);
        if (!p->panic && match(p, MFL_TOK_DOT_DOT)) {
            end = parse_expr(p);
        }
        if (p->panic) {
            return NULL;
        }
        if (!enter(p)) {
            return NULL;
        }
        body = parse_block(p, "'{' after the 'for' header");
        leave(p);
        s = new_stmt(p, MFL_STMT_FOR, &start);
        if (s != NULL) {
            s->name = text_of(&var);
            s->value = first;
            s->range_end = end;
            s->body = body;
        }
        break;
    }
    case MFL_TOK_LBRACE: {
        MflStmt *body;
        if (!enter(p)) {
            return NULL;
        }
        body = parse_block(p, "'{'");
        leave(p);
        s = new_stmt(p, MFL_STMT_BLOCK, &start);
        if (s != NULL) {
            s->body = body;
        }
        break;
    }
    case MFL_TOK_ELSE:
        error_at(p, &start, "'else' without a matching 'if'");
        return NULL;
    case MFL_TOK_IMPORT: {
        int is_path;
        advance(p);
        if (p->block_depth > 0 || p->fn_depth > 0) {
            error_at(p, &start, "'import' is only allowed at the top level of a file");
            return NULL;
        }
        if (p->seen_code) {
            error_at(p, &start,
                     "'import' must come before the other statements of the file (only "
                     "comments and other imports may precede it)");
            return NULL;
        }
        if (!check(p, MFL_TOK_IDENT) && !check(p, MFL_TOK_STRING)) {
            error_expected(p, "a module name (import numerical) or a file path in quotes "
                              "(import \"materials.mfl\") after 'import'");
            return NULL;
        }
        is_path = check(p, MFL_TOK_STRING);
        advance(p);
        s = new_stmt(p, MFL_STMT_IMPORT, &start);
        if (s != NULL) {
            s->name = text_of(&p->prev);
            s->import_path = is_path;
        }
        break;
    }
    default: {
        MflExpr *e = parse_expr(p);
        if (!p->panic && check(p, MFL_TOK_EQ)) {
            MflToken eq = p->cur;
            MflExpr *value;
            advance(p);
            value = parse_expr(p);
            if (e->kind != MFL_EXPR_IDENT && e->kind != MFL_EXPR_INDEX && e != &p->oom_expr) {
                error_at(p, &eq,
                         "invalid assignment target (only a variable or an array element can be "
                         "assigned)");
                return NULL;
            }
            s = new_stmt(p, MFL_STMT_ASSIGN, &start);
            if (s != NULL) {
                s->target = e;
                s->value = value;
            }
        } else {
            s = new_stmt(p, MFL_STMT_EXPR, &start);
            if (s != NULL) {
                s->value = e;
            }
        }
        break;
    }
    }

    if (!p->panic && !at_statement_end(p)) {
        error_expected(p, "end of statement (newline or ';')");
    }
    return s;
}

/* Skips to the next statement separator outside braces. */
static void synchronize(Parser *p)
{
    int braces = 0;
    while (!check(p, MFL_TOK_EOF)) {
        if (braces == 0 && (check(p, MFL_TOK_NEWLINE) || check(p, MFL_TOK_SEMICOLON))) {
            break;
        }
        if (check(p, MFL_TOK_LBRACE)) {
            braces++;
        } else if (check(p, MFL_TOK_RBRACE) && braces > 0) {
            braces--;
        }
        advance(p);
    }
    p->panic = 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int mfl_parse(const char *src, size_t length, size_t memory_limit, MflParseResult *out)
{
    Parser p;

    memset(out, 0, sizeof *out);
    mfl_arena_init(&out->arena, memory_limit);
    memset(&p, 0, sizeof p);
    p.res = out;
    p.oom_expr.kind = MFL_EXPR_ERROR;
    mfl_lexer_init(&p.lx, src, length);
    advance(&p);

    while (!check(&p, MFL_TOK_EOF) && !p.stop) {
        MflStmt *s;
        if (match(&p, MFL_TOK_NEWLINE) || match(&p, MFL_TOK_SEMICOLON)) {
            continue;
        }
        if (!check(&p, MFL_TOK_IMPORT)) {
            p.seen_code = 1;
        }
        s = parse_statement(&p);
        if (p.panic) {
            synchronize(&p);
            continue;
        }
        if (s != NULL) {
            if (p.stmt_tail == NULL) {
                out->program.first = s;
            } else {
                p.stmt_tail->next = s;
            }
            p.stmt_tail = s;
            out->program.count++;
        }
    }

    if (out->out_of_memory) {
        out->program.first = NULL;
        out->program.count = 0;
    }
    return !out->out_of_memory && out->diagnostic_count == 0;
}

int mfl_parse_unit(const char *src, size_t length, MflParseResult *out, MflExpr **unit)
{
    Parser p;
    MflExpr *u;

    memset(out, 0, sizeof *out);
    mfl_arena_init(&out->arena, 0);
    memset(&p, 0, sizeof p);
    p.res = out;
    p.oom_expr.kind = MFL_EXPR_ERROR;
    mfl_lexer_init(&p.lx, src, length);
    advance(&p);
    u = parse_unit(&p);
    match(&p, MFL_TOK_NEWLINE);
    if (!p.panic && !check(&p, MFL_TOK_EOF)) {
        error_expected(&p, "end of unit");
    }
    *unit = u;
    return !out->out_of_memory && out->diagnostic_count == 0 && u != &p.oom_expr;
}

void mfl_parse_free(MflParseResult *result)
{
    mfl_arena_free(&result->arena);
    result->program.first = NULL;
    result->program.count = 0;
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
}
