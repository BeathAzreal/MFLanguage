/*
 * compiler.c - checked AST -> bytecode. See compiler.h and chunk.h.
 *
 * Layout: the main program first (ending in HALT), then every function.
 * Frames: a call places the callee and its arguments on the stack; the
 * arguments become the first local slots of the new frame, followed by the
 * function's other locals (initialised to nil).
 *
 * Gradual typing: where the checker could not prove a type (MFL_COMPAT_
 * RUNTIME), the compiler emits CHECK_TYPE / CHECK_LIKE_* so the VM verifies
 * it; every operation is also re-checked by the VM itself.
 *
 * SPDX-License-Identifier: MIT
 */
#include "compiler/compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "runtime/number.h"
#include "runtime/units.h"

typedef struct {
    MflCompileResult     *res;
    MflChunk             *chunk;
    const MflCheckResult *checked;
    MflDiagnostic        *diag_tail;
    size_t                depth;     /* current value-stack depth in the frame */
    size_t                max_depth; /* of the current frame */
    const MflFnSig       *fn;        /* function being compiled (NULL: main) */
    int                   failed;
} Compiler;

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

static void error_at(Compiler *c, int line, int column, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    MflDiagnostic *d;
    char *msg;
    size_t len;

    c->failed = 1;
    if (c->res->diagnostic_count >= 50) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    len = strlen(text);
    d = (MflDiagnostic *)mfl_arena_alloc(&c->res->arena, sizeof *d);
    msg = (char *)mfl_arena_alloc(&c->res->arena, len + 1);
    if (d == NULL || msg == NULL) {
        c->res->out_of_memory = 1;
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

static int oom(Compiler *c)
{
    if (c->chunk->out_of_memory) {
        c->res->out_of_memory = 1;
        c->failed = 1;
        return 1;
    }
    return 0;
}

#define TEXT_ARG(t) ((t).length > 40 ? 40 : (int)(t).length), (t).start

/* ------------------------------------------------------------------------ */
/* Emission                                                                  */
/* ------------------------------------------------------------------------ */

static long stack_effect(MflOpcode op, unsigned arg)
{
    switch (op) {
    case MFL_OP_CONST:
    case MFL_OP_NIL:
    case MFL_OP_TRUE:
    case MFL_OP_FALSE:
    case MFL_OP_GET_GLOBAL:
    case MFL_OP_GET_LOCAL:
        return 1;
    case MFL_OP_POP:
    case MFL_OP_SET_GLOBAL:
    case MFL_OP_SET_LOCAL:
    case MFL_OP_ADD:
    case MFL_OP_SUB:
    case MFL_OP_MUL:
    case MFL_OP_DIV:
    case MFL_OP_MOD:
    case MFL_OP_POW:
    case MFL_OP_EQ:
    case MFL_OP_NE:
    case MFL_OP_LT:
    case MFL_OP_LE:
    case MFL_OP_GT:
    case MFL_OP_GE:
    case MFL_OP_INDEX:
    case MFL_OP_PUSH:
    case MFL_OP_RETURN:
        return -1;
    case MFL_OP_SET_INDEX:
        return -3;
    case MFL_OP_PRINT:
    case MFL_OP_ARRAY:
        return 1 - (long)arg;
    case MFL_OP_CALL:
        return -(long)arg;
    default:
        return 0;
    }
}

static void emit_byte(Compiler *c, unsigned char b, int line, int column)
{
    if (!c->failed) {
        mfl_chunk_emit(c->chunk, b, line, column);
        oom(c);
    }
}

/* Emits op and its operands; returns the code offset of the first operand. */
static size_t emit2(Compiler *c, MflOpcode op, unsigned a, unsigned b, int line, int column)
{
    size_t at;
    long effect = stack_effect(op, a);
    int nops = mfl_opcode_operands(op);
    emit_byte(c, (unsigned char)op, line, column);
    at = c->chunk->code_len;
    if (nops >= 1) {
        emit_byte(c, (unsigned char)(a & 0xFF), line, column);
        emit_byte(c, (unsigned char)((a >> 8) & 0xFF), line, column);
    }
    if (nops >= 2) {
        emit_byte(c, (unsigned char)(b & 0xFF), line, column);
        emit_byte(c, (unsigned char)((b >> 8) & 0xFF), line, column);
    }
    if (effect < 0 && (size_t)(-effect) > c->depth) {
        c->depth = 0; /* cannot happen for checked programs */
    } else {
        c->depth = (size_t)((long)c->depth + effect);
    }
    if (c->depth > c->max_depth) {
        c->max_depth = c->depth;
    }
    return at;
}

static size_t emit(Compiler *c, MflOpcode op, unsigned a, int line, int column)
{
    return emit2(c, op, a, 0, line, column);
}

#define EMIT(c, op, a, at) emit((c), (op), (a), (at)->line, (at)->column)

static void patch_jump(Compiler *c, size_t operand_at, int line, int column)
{
    size_t offset;
    if (c->failed) {
        return;
    }
    offset = c->chunk->code_len - (operand_at + 2);
    if (offset > MFL_CHUNK_MAX_INDEX) {
        error_at(c, line, column, "block too large to compile (jump over 65535 bytes)");
        return;
    }
    c->chunk->code[operand_at] = (unsigned char)(offset & 0xFF);
    c->chunk->code[operand_at + 1] = (unsigned char)((offset >> 8) & 0xFF);
}

static void emit_loop(Compiler *c, size_t loop_start, int line, int column)
{
    size_t offset;
    if (c->failed) {
        return;
    }
    offset = c->chunk->code_len + 3 - loop_start;
    if (offset > MFL_CHUNK_MAX_INDEX) {
        error_at(c, line, column, "loop body too large to compile (jump over 65535 bytes)");
        return;
    }
    emit(c, MFL_OP_LOOP, (unsigned)offset, line, column);
}

static unsigned add_constant(Compiler *c, MflValue v, const MflExpr *at)
{
    int k;
    if (c->failed) {
        return 0;
    }
    k = mfl_chunk_add_constant(c->chunk, v);
    if (k < 0) {
        if (!oom(c)) {
            error_at(c, at->line, at->column, "too many constants (limit 65535)");
        }
        return 0;
    }
    return (unsigned)k;
}

static unsigned add_unit(Compiler *c, const char *text, MflScale scale, MflDim dim, int affine,
                         int line, int column)
{
    int u;
    if (c->failed) {
        return 0;
    }
    u = mfl_chunk_add_unit_ex(c->chunk, text, scale, dim, affine, affine ? MFL_DEGC_OFFSET : 0.0);
    if (u < 0) {
        if (!oom(c)) {
            error_at(c, line, column, "too many distinct units (limit 65535)");
        }
        return 0;
    }
    return (unsigned)u;
}

static MflValue string_value(const char *chars, size_t length)
{
    MflValue v;
    memset(&v, 0, sizeof v);
    v.kind = MFL_VAL_STRING;
    v.unit = MFL_NO_UNIT;
    v.as.string.chars = chars;
    v.as.string.length = length;
    return v;
}

/* String constant (for run-time error contexts). */
static unsigned context_constant(Compiler *c, const MflExpr *at, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    char *copy;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    copy = mfl_chunk_strdup(c->chunk, text, strlen(text));
    if (copy == NULL) {
        oom(c);
        return 0;
    }
    return add_constant(c, string_value(copy, strlen(copy)), at);
}

/* Run-time type descriptor for a static type. */
static int type_desc(Compiler *c, MflType t)
{
    MflTypeDesc d;
    int k;
    if (c->failed) {
        return -1;
    }
    memset(&d, 0, sizeof d);
    d.kind = t.kind;
    d.dim = t.dim;
    d.affine = t.affine;
    d.elem = -1;
    if (t.kind == MFL_TYPE_ARRAY && t.elem != NULL && t.elem->kind != MFL_TYPE_ANY) {
        d.elem = type_desc(c, *t.elem);
    }
    k = mfl_chunk_add_type(c->chunk, d);
    if (k < 0) {
        if (!oom(c)) {
            error_at(c, 1, 1, "too many distinct types (limit 65535)");
        }
        return 0;
    }
    return k;
}

static void emit_check(Compiler *c, MflType want, const MflExpr *at, unsigned context)
{
    int desc = type_desc(c, want);
    if (desc >= 0) {
        emit2(c, MFL_OP_CHECK_TYPE, (unsigned)desc, context, at->line, at->column);
    }
}

/* ------------------------------------------------------------------------ */
/* Units                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    char   text[256];
    size_t len;
    int    overflow;
} UnitText;

static void ut_put(UnitText *u, const char *s, size_t n)
{
    if (u->len + n >= sizeof u->text) {
        u->overflow = 1;
        return;
    }
    memcpy(u->text + u->len, s, n);
    u->len += n;
    u->text[u->len] = '\0';
}

static int is_product(const MflExpr *e)
{
    return e->kind == MFL_EXPR_BINARY &&
           (e->as.binary.op == MFL_TOK_STAR || e->as.binary.op == MFL_TOK_SLASH);
}

/* Normalised text of a checked unit expression: "J/mm", "W/(m*K)", "m^-2". */
static void unit_text(const MflExpr *e, UnitText *u)
{
    if (e->kind == MFL_EXPR_IDENT || e->kind == MFL_EXPR_NUMBER) {
        ut_put(u, e->as.text.start, e->as.text.length);
    } else if (e->kind == MFL_EXPR_UNARY) {
        ut_put(u, "-", 1);
        unit_text(e->as.unary.operand, u);
    } else if (e->kind == MFL_EXPR_BINARY && e->as.binary.op == MFL_TOK_CARET) {
        int group = is_product(e->as.binary.left);
        if (group) {
            ut_put(u, "(", 1);
        }
        unit_text(e->as.binary.left, u);
        if (group) {
            ut_put(u, ")", 1);
        }
        ut_put(u, "^", 1);
        unit_text(e->as.binary.right, u);
    } else if (is_product(e)) {
        int group = is_product(e->as.binary.right);
        unit_text(e->as.binary.left, u);
        ut_put(u, e->as.binary.op == MFL_TOK_STAR ? "*" : "/", 1);
        if (group) {
            ut_put(u, "(", 1);
        }
        unit_text(e->as.binary.right, u);
        if (group) {
            ut_put(u, ")", 1);
        }
    }
}

int mfl_unit_text(const MflExpr *unit, char *buf, size_t size)
{
    UnitText u;
    u.len = 0;
    u.overflow = 0;
    u.text[0] = '\0';
    unit_text(unit, &u);
    if (u.overflow || u.len + 1 > size) {
        return 0;
    }
    memcpy(buf, u.text, u.len + 1);
    return 1;
}

static unsigned unit_of(Compiler *c, const MflExpr *unit)
{
    UnitText u;
    u.len = 0;
    u.overflow = 0;
    u.text[0] = '\0';
    unit_text(unit, &u);
    if (u.overflow) {
        error_at(c, unit->line, unit->column, "unit expression too long (limit 255 characters)");
        return 0;
    }
    return add_unit(c, u.text, unit->unit_scale, unit->type.dim, unit->type.affine, unit->line,
                    unit->column);
}

/* ------------------------------------------------------------------------ */
/* Expressions                                                               */
/* ------------------------------------------------------------------------ */

static double number_literal(Compiler *c, const MflExpr *e)
{
    double v = 0.0;
    switch (mfl_number_parse(e->as.text.start, e->as.text.length, &v)) {
    case MFL_NUM_OK:
        return v;
    case MFL_NUM_OVERFLOW:
        error_at(c, e->line, e->column, "number literal too large (beyond about 1.8e308)");
        break;
    case MFL_NUM_UNDERFLOW:
        error_at(c, e->line, e->column, "number literal too small (rounds to zero)");
        break;
    case MFL_NUM_INVALID:
        error_at(c, e->line, e->column, "invalid number literal (at most 400 characters)");
        break;
    }
    return 0.0;
}

/* Decodes \n \t \" \\ (the lexer has validated the escapes). */
static MflValue string_constant(Compiler *c, const MflExpr *e)
{
    const char *s = e->as.text.start + 1;
    size_t n = e->as.text.length - 2;
    char *out = mfl_chunk_strdup(c->chunk, s, n);
    size_t i;
    size_t j = 0;

    if (out == NULL) {
        oom(c);
        return string_value("", 0);
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char e2 = s[++i];
            out[j++] = e2 == 'n' ? '\n' : e2 == 't' ? '\t' : e2;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return string_value(out, j);
}

static MflValue number_value(double x, int unit, MflDim dim, int affine)
{
    MflValue v;
    memset(&v, 0, sizeof v);
    v.kind = MFL_VAL_NUMBER;
    v.unit = unit;
    v.dim = dim;
    v.affine = (unsigned char)(affine != 0);
    v.as.number = x;
    return v;
}

static void compile_expr(Compiler *c, const MflExpr *e);

static void compile_binary(Compiler *c, const MflExpr *e)
{
    MflTokenKind op = e->as.binary.op;
    MflOpcode code;

    if (op == MFL_TOK_AND || op == MFL_TOK_OR) {
        size_t jump;
        compile_expr(c, e->as.binary.left);
        jump = EMIT(c, op == MFL_TOK_AND ? MFL_OP_JUMP_IF_FALSE : MFL_OP_JUMP_IF_TRUE, 0, e);
        EMIT(c, MFL_OP_POP, 0, e);
        compile_expr(c, e->as.binary.right);
        if (e->as.binary.right->type.kind == MFL_TYPE_ANY) {
            /* the left operand is checked by the jump; the right one here */
            MflType b;
            memset(&b, 0, sizeof b);
            b.kind = MFL_TYPE_BOOL;
            emit_check(c, b, e->as.binary.right,
                       context_constant(c, e, "the right operand of '%s'", op == MFL_TOK_AND ? "and" : "or"));
        }
        patch_jump(c, jump, e->line, e->column);
        return;
    }

    compile_expr(c, e->as.binary.left);
    compile_expr(c, e->as.binary.right);
    switch (op) {
    case MFL_TOK_PLUS: code = MFL_OP_ADD; break;
    case MFL_TOK_MINUS: code = MFL_OP_SUB; break;
    case MFL_TOK_STAR: code = MFL_OP_MUL; break;
    case MFL_TOK_SLASH: code = MFL_OP_DIV; break;
    case MFL_TOK_PERCENT: code = MFL_OP_MOD; break;
    case MFL_TOK_CARET: code = MFL_OP_POW; break;
    case MFL_TOK_EQ_EQ: code = MFL_OP_EQ; break;
    case MFL_TOK_BANG_EQ: code = MFL_OP_NE; break;
    case MFL_TOK_LT: code = MFL_OP_LT; break;
    case MFL_TOK_LT_EQ: code = MFL_OP_LE; break;
    case MFL_TOK_GT: code = MFL_OP_GT; break;
    case MFL_TOK_GT_EQ: code = MFL_OP_GE; break;
    default:
        error_at(c, e->line, e->column, "internal error: unexpected operator");
        return;
    }
    EMIT(c, code, 0, e);
}

static void compile_call(Compiler *c, const MflExpr *e)
{
    const MflExpr *callee = e->as.call.callee;
    const MflExpr *a;

    if (callee->kind == MFL_EXPR_MEMBER) { /* x.to(u) / x.value_in(u) */
        MflText name = callee->as.member.name;
        int to = name.length == 2 && memcmp(name.start, "to", 2) == 0;
        compile_expr(c, callee->as.member.object);
        EMIT(c, to ? MFL_OP_CONVERT : MFL_OP_VALUE_IN, unit_of(c, e->as.call.args), e);
        return;
    }
    if (e->as.call.argc > 255) {
        error_at(c, e->line, e->column, "too many arguments (limit 255)");
        return;
    }
    if (callee->kind == MFL_EXPR_IDENT && callee->ref_kind == MFL_REF_BUILTIN) {
        for (a = e->as.call.args; a != NULL; a = a->next) {
            compile_expr(c, a);
        }
        switch ((MflBuiltin)callee->ref_index) {
        case MFL_BUILTIN_PRINT:
            EMIT(c, MFL_OP_PRINT, (unsigned)e->as.call.argc, e);
            break;
        case MFL_BUILTIN_LEN:
            EMIT(c, MFL_OP_LEN, 0, e);
            break;
        case MFL_BUILTIN_PUSH: {
            const MflExpr *arr = e->as.call.args;
            const MflExpr *val = arr->next;
            if (arr->type.kind == MFL_TYPE_ARRAY && arr->type.elem != NULL &&
                mfl_type_compat(*arr->type.elem, val->type) == MFL_COMPAT_RUNTIME) {
                emit_check(c, *arr->type.elem, val, context_constant(c, val, "the value pushed"));
            }
            EMIT(c, MFL_OP_PUSH, 0, e);
            break;
        }
        }
        return;
    }
    compile_expr(c, callee);
    for (a = e->as.call.args; a != NULL; a = a->next) {
        compile_expr(c, a);
    }
    EMIT(c, MFL_OP_CALL, (unsigned)e->as.call.argc, e);
}

static void compile_expr(Compiler *c, const MflExpr *e)
{
    MflValue v;
    const MflExpr *el;

    if (c->failed) {
        return;
    }
    switch (e->kind) {
    case MFL_EXPR_NUMBER:
        v = number_value(number_literal(c, e), MFL_NO_UNIT, mfl_dim_none(), 0);
        EMIT(c, MFL_OP_CONST, add_constant(c, v, e), e);
        break;
    case MFL_EXPR_QUANTITY:
        v = number_value(number_literal(c, e->as.quantity.value), (int)unit_of(c, e->as.quantity.unit),
                         e->type.dim, e->type.affine);
        EMIT(c, MFL_OP_CONST, add_constant(c, v, e), e);
        break;
    case MFL_EXPR_STRING:
        v = string_constant(c, e);
        EMIT(c, MFL_OP_CONST, add_constant(c, v, e), e);
        break;
    case MFL_EXPR_BOOL:
        EMIT(c, e->as.boolean ? MFL_OP_TRUE : MFL_OP_FALSE, 0, e);
        break;
    case MFL_EXPR_NIL:
        EMIT(c, MFL_OP_NIL, 0, e);
        break;
    case MFL_EXPR_IDENT:
        switch (e->ref_kind) {
        case MFL_REF_GLOBAL:
            EMIT(c, MFL_OP_GET_GLOBAL, (unsigned)e->ref_index, e);
            break;
        case MFL_REF_LOCAL:
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)e->ref_index, e);
            break;
        case MFL_REF_FUNCTION:
            memset(&v, 0, sizeof v);
            v.kind = MFL_VAL_FUNCTION;
            v.unit = MFL_NO_UNIT;
            v.as.function = e->ref_index;
            EMIT(c, MFL_OP_CONST, add_constant(c, v, e), e);
            break;
        default:
            error_at(c, e->line, e->column, "internal error: unresolved name");
            break;
        }
        break;
    case MFL_EXPR_UNARY:
        compile_expr(c, e->as.unary.operand);
        EMIT(c, e->as.unary.op == MFL_TOK_NOT ? MFL_OP_NOT : MFL_OP_NEG, 0, e);
        break;
    case MFL_EXPR_BINARY:
        compile_binary(c, e);
        break;
    case MFL_EXPR_CALL:
        compile_call(c, e);
        break;
    case MFL_EXPR_ARRAY:
        if (e->as.array.count > 65535) {
            error_at(c, e->line, e->column, "array literal too long (limit 65535 elements)");
            return;
        }
        for (el = e->as.array.elems; el != NULL; el = el->next) {
            compile_expr(c, el);
        }
        EMIT(c, MFL_OP_ARRAY, (unsigned)e->as.array.count, e);
        break;
    case MFL_EXPR_INDEX:
        compile_expr(c, e->as.index.object);
        compile_expr(c, e->as.index.index);
        EMIT(c, MFL_OP_INDEX, 0, e);
        break;
    default:
        error_at(c, e->line, e->column, "internal error: expression cannot be compiled");
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Statements                                                                */
/* ------------------------------------------------------------------------ */

static void compile_block(Compiler *c, const MflStmt *first);

static void store(Compiler *c, int is_global, int slot, const MflStmt *at)
{
    EMIT(c, is_global ? MFL_OP_SET_GLOBAL : MFL_OP_SET_LOCAL, (unsigned)slot, at);
}

static MflType bool_type(void)
{
    MflType t;
    memset(&t, 0, sizeof t);
    t.kind = MFL_TYPE_BOOL;
    return t;
}

static MflType plain_number(void)
{
    MflType t;
    memset(&t, 0, sizeof t);
    t.kind = MFL_TYPE_NUMBER;
    t.dim = mfl_dim_none();
    return t;
}

static void compile_condition(Compiler *c, const MflExpr *cond, const char *what)
{
    compile_expr(c, cond);
    if (cond->type.kind == MFL_TYPE_ANY) {
        emit_check(c, bool_type(), cond, context_constant(c, cond, "the condition of '%s'", what));
    }
}

static void compile_if(Compiler *c, const MflStmt *s)
{
    size_t to_else;
    size_t to_end;
    compile_condition(c, s->value, "if");
    to_else = EMIT(c, MFL_OP_JUMP_IF_FALSE, 0, s);
    EMIT(c, MFL_OP_POP, 0, s);
    compile_block(c, s->body);
    to_end = EMIT(c, MFL_OP_JUMP, 0, s);
    patch_jump(c, to_else, s->line, s->column);
    c->depth++; /* on the jump path the condition is still on the stack */
    EMIT(c, MFL_OP_POP, 0, s);
    if (s->else_body != NULL) {
        if (s->else_body->kind == MFL_STMT_IF) {
            compile_if(c, s->else_body);
        } else {
            compile_block(c, s->else_body->body);
        }
    }
    patch_jump(c, to_end, s->line, s->column);
}

static void compile_stmt(Compiler *c, const MflStmt *s)
{
    MflCompat k;
    if (c->failed) {
        return;
    }
    switch (s->kind) {
    case MFL_STMT_LET:
    case MFL_STMT_VAR:
        compile_expr(c, s->value);
        if (s->annotation != NULL && mfl_type_compat(s->decl_type, s->value->type) == MFL_COMPAT_RUNTIME) {
            emit_check(c, s->decl_type, s->value, context_constant(c, s->value, "the value of '%.*s'", TEXT_ARG(s->name)));
        }
        store(c, s->is_global, s->slot, s);
        break;
    case MFL_STMT_ASSIGN: {
        const MflExpr *t = s->target;
        if (t->kind == MFL_EXPR_INDEX) {
            const MflType *el = t->as.index.object->type.kind == MFL_TYPE_ARRAY ? t->as.index.object->type.elem : NULL;
            compile_expr(c, t->as.index.object);
            compile_expr(c, t->as.index.index);
            compile_expr(c, s->value);
            if (el != NULL && mfl_type_compat(*el, s->value->type) == MFL_COMPAT_RUNTIME) {
                emit_check(c, *el, s->value, context_constant(c, s->value, "the new element"));
            }
            EMIT(c, MFL_OP_SET_INDEX, 0, t);
            break;
        }
        compile_expr(c, s->value);
        k = mfl_type_compat(t->type, s->value->type);
        if (t->type.kind == MFL_TYPE_ANY) {
            unsigned ctx = context_constant(c, t, "'%.*s'", TEXT_ARG(t->as.text));
            emit2(c, t->ref_kind == MFL_REF_GLOBAL ? MFL_OP_CHECK_LIKE_GLOBAL : MFL_OP_CHECK_LIKE_LOCAL,
                  (unsigned)t->ref_index, ctx, t->line, t->column);
        } else if (k == MFL_COMPAT_RUNTIME) {
            emit_check(c, t->type, s->value, context_constant(c, t, "the new value of '%.*s'", TEXT_ARG(t->as.text)));
        }
        EMIT(c, t->ref_kind == MFL_REF_GLOBAL ? MFL_OP_SET_GLOBAL : MFL_OP_SET_LOCAL,
             (unsigned)t->ref_index, t);
        break;
    }
    case MFL_STMT_EXPR:
        compile_expr(c, s->value);
        EMIT(c, MFL_OP_POP, 0, s);
        break;
    case MFL_STMT_FN:
        break; /* compiled separately */
    case MFL_STMT_RETURN:
        if (s->value != NULL) {
            compile_expr(c, s->value);
            if (c->fn != NULL && c->fn->ret_annotated &&
                mfl_type_compat(c->fn->ret, s->value->type) == MFL_COMPAT_RUNTIME) {
                emit_check(c, c->fn->ret, s->value,
                           context_constant(c, s->value, "the return value of '%.*s'", TEXT_ARG(c->fn->name)));
            }
        } else {
            EMIT(c, MFL_OP_NIL, 0, s);
        }
        EMIT(c, MFL_OP_RETURN, 0, s);
        break;
    case MFL_STMT_IF:
        compile_if(c, s);
        break;
    case MFL_STMT_WHILE: {
        size_t start = c->chunk->code_len;
        size_t exit;
        compile_condition(c, s->value, "while");
        exit = EMIT(c, MFL_OP_JUMP_IF_FALSE, 0, s);
        EMIT(c, MFL_OP_POP, 0, s);
        compile_block(c, s->body);
        emit_loop(c, start, s->line, s->column);
        patch_jump(c, exit, s->line, s->column);
        c->depth++; /* the false condition is still on the stack here */
        EMIT(c, MFL_OP_POP, 0, s);
        break;
    }
    case MFL_STMT_FOR: {
        size_t start;
        size_t exit;
        MflValue one = number_value(1.0, MFL_NO_UNIT, mfl_dim_none(), 0);
        if (s->range_end != NULL) {
            /* for i in a..b:  i = a; end = b; while i < end { body; i = i + 1 } */
            compile_expr(c, s->value);
            if (s->value->type.kind == MFL_TYPE_ANY) {
                emit_check(c, plain_number(), s->value, context_constant(c, s->value, "the start of the range"));
            }
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot, s);
            compile_expr(c, s->range_end);
            if (s->range_end->type.kind == MFL_TYPE_ANY) {
                emit_check(c, plain_number(), s->range_end, context_constant(c, s->range_end, "the end of the range"));
            }
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot2, s);
            start = c->chunk->code_len;
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot, s);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot2, s);
            EMIT(c, MFL_OP_LT, 0, s);
            exit = EMIT(c, MFL_OP_JUMP_IF_FALSE, 0, s);
            EMIT(c, MFL_OP_POP, 0, s);
            compile_block(c, s->body);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot, s);
            EMIT(c, MFL_OP_CONST, add_constant(c, one, s->value), s);
            EMIT(c, MFL_OP_ADD, 0, s);
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot, s);
        } else {
            /* for x in array:  arr = e; idx = 0; while idx < len(arr) { x = arr[idx]; body; idx += 1 } */
            compile_expr(c, s->value);
            if (s->value->type.kind == MFL_TYPE_ANY) {
                MflType at;
                memset(&at, 0, sizeof at);
                at.kind = MFL_TYPE_ARRAY;
                emit_check(c, at, s->value, context_constant(c, s->value, "the value after 'in'"));
            }
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot2, s);
            EMIT(c, MFL_OP_CONST, add_constant(c, number_value(0.0, MFL_NO_UNIT, mfl_dim_none(), 0), s->value), s);
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot3, s);
            start = c->chunk->code_len;
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot3, s);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot2, s);
            EMIT(c, MFL_OP_LEN, 0, s);
            EMIT(c, MFL_OP_LT, 0, s);
            exit = EMIT(c, MFL_OP_JUMP_IF_FALSE, 0, s);
            EMIT(c, MFL_OP_POP, 0, s);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot2, s);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot3, s);
            EMIT(c, MFL_OP_INDEX, 0, s);
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot, s);
            compile_block(c, s->body);
            EMIT(c, MFL_OP_GET_LOCAL, (unsigned)s->slot3, s);
            EMIT(c, MFL_OP_CONST, add_constant(c, one, s->value), s);
            EMIT(c, MFL_OP_ADD, 0, s);
            EMIT(c, MFL_OP_SET_LOCAL, (unsigned)s->slot3, s);
        }
        emit_loop(c, start, s->line, s->column);
        patch_jump(c, exit, s->line, s->column);
        c->depth++; /* the false comparison is still on the stack here */
        EMIT(c, MFL_OP_POP, 0, s);
        break;
    }
    case MFL_STMT_BLOCK:
        compile_block(c, s->body);
        break;
    }
}

static void compile_block(Compiler *c, const MflStmt *first)
{
    const MflStmt *s;
    for (s = first; s != NULL && !c->failed; s = s->next) {
        compile_stmt(c, s);
    }
}

static void compile_function(Compiler *c, const MflFnSig *sig, MflFunction *out)
{
    const MflStmt *decl = sig->decl;
    const MflParam *prm;
    int i;

    c->fn = sig;
    c->depth = (size_t)decl->frame_size;
    c->max_depth = c->depth;
    out->entry = c->chunk->code_len;
    /* Prologue: annotated parameters are checked when the function runs
     * (the call may come through a value whose type was not known). */
    for (prm = decl->params, i = 0; prm != NULL; prm = prm->next, i++) {
        if (prm->type != NULL && sig->params[i].kind != MFL_TYPE_ERROR) {
            MflExpr at;
            memset(&at, 0, sizeof at);
            at.line = prm->line;
            at.column = prm->column;
            emit(c, MFL_OP_GET_LOCAL, (unsigned)i, prm->line, prm->column);
            emit_check(c, sig->params[i], &at,
                       context_constant(c, &at, "argument %d of '%.*s' ('%.*s')", i + 1,
                                        TEXT_ARG(sig->name), TEXT_ARG(prm->name)));
            emit(c, MFL_OP_POP, 0, prm->line, prm->column);
        }
    }
    compile_block(c, decl->body);
    /* Falling off the end returns nil (unannotated functions only: the
     * checker rejects annotated ones that can reach this point). */
    emit(c, MFL_OP_NIL, 0, decl->line, decl->column);
    emit(c, MFL_OP_RETURN, 0, decl->line, decl->column);
    out->max_stack = c->max_depth + 1;
    c->fn = NULL;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int mfl_compile(const MflProgram *program, const MflCheckResult *checked, size_t memory_limit,
                MflCompileResult *out)
{
    Compiler c;
    const MflStmt *s;
    const MflSymbol *sym;
    size_t n = 0;
    int k;

    memset(out, 0, sizeof *out);
    mfl_chunk_init(&out->chunk, memory_limit);
    mfl_arena_init(&out->arena, 0);
    memset(&c, 0, sizeof c);
    c.res = out;
    c.chunk = &out->chunk;
    c.checked = checked;

    for (sym = checked->symbols; sym != NULL; sym = sym->next) {
        n++;
    }
    if (n > MFL_CHUNK_MAX_INDEX) {
        error_at(&c, 1, 1, "too many variables (limit 65535)");
        return 0;
    }
    out->chunk.global_count = n;
    out->chunk.global_names = (const char **)mfl_chunk_alloc(&out->chunk, (n + 1) * sizeof(char *));
    out->chunk.functions = (MflFunction *)mfl_chunk_alloc(
        &out->chunk, (size_t)(checked->function_count + 1) * sizeof(MflFunction));
    if (oom(&c)) {
        return 0;
    }
    n = 0;
    for (sym = checked->symbols; sym != NULL; sym = sym->next, n++) {
        out->chunk.global_names[n] = mfl_chunk_strdup(&out->chunk, sym->name.start, sym->name.length);
    }
    out->chunk.function_count = (size_t)checked->function_count;
    for (k = 0; k < checked->function_count; k++) {
        const MflFnSig *f = checked->functions[k];
        out->chunk.functions[k].name = mfl_chunk_strdup(&out->chunk, f->name.start, f->name.length);
        out->chunk.functions[k].arity = f->arity;
        out->chunk.functions[k].frame_size = f->decl->frame_size;
        out->chunk.functions[k].line = f->line;
        out->chunk.functions[k].column = f->column;
    }
    if (oom(&c)) {
        return 0;
    }

    /* Main program. */
    out->chunk.main_frame_size = program->main_frame_size;
    c.depth = (size_t)program->main_frame_size;
    c.max_depth = c.depth;
    for (s = program->first; s != NULL && !c.failed; s = s->next) {
        compile_stmt(&c, s);
    }
    emit(&c, MFL_OP_HALT, 0, 0, 0);
    out->chunk.main_max_stack = c.max_depth + 1;
    out->chunk.max_stack = out->chunk.main_max_stack;

    /* Functions. */
    for (k = 0; k < checked->function_count && !c.failed; k++) {
        compile_function(&c, checked->functions[k], &out->chunk.functions[k]);
    }
    oom(&c);
    return !c.failed && !out->out_of_memory;
}

void mfl_compile_free(MflCompileResult *result)
{
    mfl_chunk_free(&result->chunk);
    mfl_arena_free(&result->arena);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
}
