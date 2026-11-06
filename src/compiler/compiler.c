/*
 * compiler.c - checked AST -> bytecode. See compiler.h and chunk.h.
 *
 * Globals: every declared name gets a slot, in declaration order (the
 * checker's symbol table).
 *
 * Units: a quantity literal becomes a constant whose magnitude is the
 * literal as written and whose unit is the literal's unit (by its
 * normalised text, e.g. "W/(m*K)"). After any arithmetic result that has
 * a dimension, DEFAULT_UNIT gives it the coherent SI display unit of
 * that dimension unless the VM kept the operands' common unit.
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
    size_t                depth; /* current value-stack depth */
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

/* ------------------------------------------------------------------------ */
/* Emission                                                                  */
/* ------------------------------------------------------------------------ */

static int stack_effect(MflOpcode op, unsigned arg)
{
    switch (op) {
    case MFL_OP_CONST:
    case MFL_OP_NIL:
    case MFL_OP_TRUE:
    case MFL_OP_FALSE:
    case MFL_OP_GET_GLOBAL:
        return 1;
    case MFL_OP_POP:
    case MFL_OP_SET_GLOBAL:
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
        return -1;
    case MFL_OP_PRINT:
        return 1 - (int)arg;
    default:
        return 0;
    }
}

static void emit_byte(Compiler *c, unsigned char b, int line)
{
    if (!c->failed) {
        mfl_chunk_emit(c->chunk, b, line);
        oom(c);
    }
}

/* Emits op (and its u16 operand); returns the operand's code offset. */
static size_t emit(Compiler *c, MflOpcode op, unsigned arg, int line)
{
    size_t at;
    int effect = stack_effect(op, arg);
    emit_byte(c, (unsigned char)op, line);
    at = c->chunk->code_len;
    if (mfl_opcode_operands(op) == 1) {
        emit_byte(c, (unsigned char)(arg & 0xFF), line);
        emit_byte(c, (unsigned char)((arg >> 8) & 0xFF), line);
    }
    if (effect < 0 && (size_t)(-effect) > c->depth) {
        c->depth = 0; /* cannot happen for checked programs */
    } else {
        c->depth = (size_t)((long)c->depth + effect);
    }
    if (c->depth > c->chunk->max_stack) {
        c->chunk->max_stack = c->depth;
    }
    return at;
}

static void patch_jump(Compiler *c, size_t operand_at, int line, int column)
{
    size_t offset;
    if (c->failed) {
        return;
    }
    offset = c->chunk->code_len - (operand_at + 2);
    if (offset > MFL_CHUNK_MAX_INDEX) {
        error_at(c, line, column, "expression too large to compile (jump over 65535 bytes)");
        return;
    }
    c->chunk->code[operand_at] = (unsigned char)(offset & 0xFF);
    c->chunk->code[operand_at + 1] = (unsigned char)((offset >> 8) & 0xFF);
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

static unsigned add_unit(Compiler *c, const char *text, MflScale scale, const MflExpr *at)
{
    int u;
    if (c->failed) {
        return 0;
    }
    u = mfl_chunk_add_unit(c->chunk, text, scale);
    if (u < 0) {
        if (!oom(c)) {
            error_at(c, at->line, at->column, "too many distinct units (limit 65535)");
        }
        return 0;
    }
    return (unsigned)u;
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
    if (e->kind == MFL_EXPR_IDENT) {
        ut_put(u, e->as.text.start, e->as.text.length);
    } else if (e->kind == MFL_EXPR_NUMBER) {
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

/* Unit-table index for a checked unit expression. */
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
    return add_unit(c, u.text, unit->unit_scale, unit);
}

/* Coherent SI display unit for a dimension. */
static unsigned si_unit_of(Compiler *c, MflDim dim, const MflExpr *at)
{
    char name[128];
    mfl_dim_display_name(dim, name, sizeof name);
    return add_unit(c, name, mfl_scale_one(), at);
}

/* ------------------------------------------------------------------------ */
/* Expressions                                                               */
/* ------------------------------------------------------------------------ */

static int global_slot(const Compiler *c, MflText name)
{
    const MflSymbol *s;
    int i = 0;
    for (s = c->checked->symbols; s != NULL; s = s->next, i++) {
        if (s->name.length == name.length && memcmp(s->name.start, name.start, name.length) == 0) {
            return i;
        }
    }
    return -1;
}

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
    MflValue v;
    const char *s = e->as.text.start + 1;
    size_t n = e->as.text.length - 2;
    char *out = mfl_chunk_strdup(c->chunk, s, n);
    size_t i;
    size_t j = 0;

    v.kind = MFL_VAL_STRING;
    v.unit = MFL_NO_UNIT;
    v.as.string.chars = "";
    v.as.string.length = 0;
    if (out == NULL) {
        oom(c);
        return v;
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
    v.as.string.chars = out;
    v.as.string.length = j;
    return v;
}

static void compile_expr(Compiler *c, const MflExpr *e);

static int has_dimension(const MflExpr *e)
{
    return e->type.kind == MFL_TYPE_NUMBER && !mfl_dim_is_none(e->type.dim);
}

static void compile_binary(Compiler *c, const MflExpr *e)
{
    MflTokenKind op = e->as.binary.op;
    MflOpcode code;

    if (op == MFL_TOK_AND || op == MFL_TOK_OR) {
        size_t jump;
        compile_expr(c, e->as.binary.left);
        jump = emit(c, op == MFL_TOK_AND ? MFL_OP_JUMP_IF_FALSE : MFL_OP_JUMP_IF_TRUE, 0,
                    e->line);
        emit(c, MFL_OP_POP, 0, e->line);
        compile_expr(c, e->as.binary.right);
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
    emit(c, code, 0, e->line);
    if (has_dimension(e)) {
        emit(c, MFL_OP_DEFAULT_UNIT, si_unit_of(c, e->type.dim, e), e->line);
    }
}

static void compile_call(Compiler *c, const MflExpr *e)
{
    const MflExpr *callee = e->as.call.callee;
    const MflExpr *a;

    if (callee->kind == MFL_EXPR_MEMBER) { /* x.to(u) / x.value_in(u) */
        MflText name = callee->as.member.name;
        int to = name.length == 2 && memcmp(name.start, "to", 2) == 0;
        compile_expr(c, callee->as.member.object);
        emit(c, to ? MFL_OP_CONVERT : MFL_OP_VALUE_IN, unit_of(c, e->as.call.args), e->line);
        return;
    }
    /* print(...): the only callable in v0.1 */
    if (e->as.call.argc > 255) {
        error_at(c, e->line, e->column, "too many arguments to print (limit 255)");
        return;
    }
    for (a = e->as.call.args; a != NULL; a = a->next) {
        compile_expr(c, a);
    }
    emit(c, MFL_OP_PRINT, (unsigned)e->as.call.argc, e->line);
}

static void compile_expr(Compiler *c, const MflExpr *e)
{
    MflValue v;

    if (c->failed) {
        return;
    }
    switch (e->kind) {
    case MFL_EXPR_NUMBER:
        v.kind = MFL_VAL_NUMBER;
        v.unit = MFL_NO_UNIT;
        v.as.number = number_literal(c, e);
        emit(c, MFL_OP_CONST, add_constant(c, v, e), e->line);
        break;
    case MFL_EXPR_QUANTITY:
        v.kind = MFL_VAL_NUMBER;
        v.as.number = number_literal(c, e->as.quantity.value);
        v.unit = (int)unit_of(c, e->as.quantity.unit);
        emit(c, MFL_OP_CONST, add_constant(c, v, e), e->line);
        break;
    case MFL_EXPR_STRING:
        v = string_constant(c, e);
        emit(c, MFL_OP_CONST, add_constant(c, v, e), e->line);
        break;
    case MFL_EXPR_BOOL:
        emit(c, e->as.boolean ? MFL_OP_TRUE : MFL_OP_FALSE, 0, e->line);
        break;
    case MFL_EXPR_NIL:
        emit(c, MFL_OP_NIL, 0, e->line);
        break;
    case MFL_EXPR_IDENT: {
        int slot = global_slot(c, e->as.text);
        if (slot < 0) {
            error_at(c, e->line, e->column, "internal error: unresolved name");
            return;
        }
        emit(c, MFL_OP_GET_GLOBAL, (unsigned)slot, e->line);
        break;
    }
    case MFL_EXPR_UNARY:
        compile_expr(c, e->as.unary.operand);
        emit(c, e->as.unary.op == MFL_TOK_NOT ? MFL_OP_NOT : MFL_OP_NEG, 0, e->line);
        break;
    case MFL_EXPR_BINARY:
        compile_binary(c, e);
        break;
    case MFL_EXPR_CALL:
        compile_call(c, e);
        break;
    default:
        error_at(c, e->line, e->column, "internal error: expression cannot be compiled");
        break;
    }
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

    memset(out, 0, sizeof *out);
    mfl_chunk_init(&out->chunk, memory_limit);
    mfl_arena_init(&out->arena, 0);
    memset(&c, 0, sizeof c);
    c.res = out;
    c.chunk = &out->chunk;
    c.checked = checked;

    for (sym = checked->symbols; sym != NULL; sym = sym->next) {
        out->chunk.global_count++;
    }
    if (out->chunk.global_count > MFL_CHUNK_MAX_INDEX) {
        error_at(&c, 1, 1, "too many variables (limit 65535)");
    }

    for (s = program->first; s != NULL && !c.failed; s = s->next) {
        int slot;
        switch (s->kind) {
        case MFL_STMT_LET:
        case MFL_STMT_VAR:
        case MFL_STMT_ASSIGN:
            compile_expr(&c, s->value);
            slot = global_slot(&c, s->kind == MFL_STMT_ASSIGN ? s->target->as.text : s->name);
            if (slot < 0) {
                error_at(&c, s->line, s->column, "internal error: unresolved name");
                break;
            }
            emit(&c, MFL_OP_SET_GLOBAL, (unsigned)slot, s->line);
            break;
        case MFL_STMT_EXPR:
            compile_expr(&c, s->value);
            emit(&c, MFL_OP_POP, 0, s->line);
            break;
        }
    }
    emit(&c, MFL_OP_HALT, 0, 0);
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
