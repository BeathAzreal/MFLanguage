/* ast.c - AST printer. SPDX-License-Identifier: MIT */
#include "parser/ast.h"

#include <stdio.h>
#include <string.h>

static void put(MflWriteFn w, void *ctx, const char *s) { w(ctx, s, strlen(s)); }

static void put_text(MflWriteFn w, void *ctx, MflText t) { w(ctx, t.start, t.length); }

static const char *op_symbol(MflTokenKind op)
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

void mfl_ast_print_expr(const MflExpr *e, MflWriteFn w, void *ctx)
{
    const MflExpr *arg;

    if (e == NULL) {
        put(w, ctx, "<null>");
        return;
    }
    switch (e->kind) {
    case MFL_EXPR_ERROR:
        put(w, ctx, "<error>");
        break;
    case MFL_EXPR_NUMBER:
    case MFL_EXPR_STRING:
    case MFL_EXPR_IDENT:
        put_text(w, ctx, e->as.text);
        break;
    case MFL_EXPR_BOOL:
        put(w, ctx, e->as.boolean ? "true" : "false");
        break;
    case MFL_EXPR_NIL:
        put(w, ctx, "nil");
        break;
    case MFL_EXPR_UNARY:
        put(w, ctx, "(");
        put(w, ctx, op_symbol(e->as.unary.op));
        put(w, ctx, " ");
        mfl_ast_print_expr(e->as.unary.operand, w, ctx);
        put(w, ctx, ")");
        break;
    case MFL_EXPR_BINARY:
        put(w, ctx, "(");
        put(w, ctx, op_symbol(e->as.binary.op));
        put(w, ctx, " ");
        mfl_ast_print_expr(e->as.binary.left, w, ctx);
        put(w, ctx, " ");
        mfl_ast_print_expr(e->as.binary.right, w, ctx);
        put(w, ctx, ")");
        break;
    case MFL_EXPR_CALL:
        put(w, ctx, "(call ");
        mfl_ast_print_expr(e->as.call.callee, w, ctx);
        for (arg = e->as.call.args; arg != NULL; arg = arg->next) {
            put(w, ctx, " ");
            mfl_ast_print_expr(arg, w, ctx);
        }
        put(w, ctx, ")");
        break;
    case MFL_EXPR_MEMBER:
        put(w, ctx, "(. ");
        mfl_ast_print_expr(e->as.member.object, w, ctx);
        put(w, ctx, " ");
        put_text(w, ctx, e->as.member.name);
        put(w, ctx, ")");
        break;
    case MFL_EXPR_INDEX:
        put(w, ctx, "(index ");
        mfl_ast_print_expr(e->as.index.object, w, ctx);
        put(w, ctx, " ");
        mfl_ast_print_expr(e->as.index.index, w, ctx);
        put(w, ctx, ")");
        break;
    case MFL_EXPR_QUANTITY:
        put(w, ctx, "(quantity ");
        mfl_ast_print_expr(e->as.quantity.value, w, ctx);
        put(w, ctx, " ");
        mfl_ast_print_expr(e->as.quantity.unit, w, ctx);
        put(w, ctx, ")");
        break;
    }
}

void mfl_ast_print_program(const MflProgram *program, MflWriteFn w, void *ctx)
{
    const MflStmt *s;
    char pos[48];

    for (s = program->first; s != NULL; s = s->next) {
        snprintf(pos, sizeof pos, "%d:%d ", s->line, s->column);
        put(w, ctx, pos);
        switch (s->kind) {
        case MFL_STMT_LET:
        case MFL_STMT_VAR:
            put(w, ctx, s->kind == MFL_STMT_LET ? "(let " : "(var ");
            put_text(w, ctx, s->name);
            put(w, ctx, " ");
            mfl_ast_print_expr(s->value, w, ctx);
            break;
        case MFL_STMT_ASSIGN:
            put(w, ctx, "(assign ");
            mfl_ast_print_expr(s->target, w, ctx);
            put(w, ctx, " ");
            mfl_ast_print_expr(s->value, w, ctx);
            break;
        case MFL_STMT_EXPR:
            put(w, ctx, "(expr ");
            mfl_ast_print_expr(s->value, w, ctx);
            break;
        }
        put(w, ctx, ")\n");
    }
}
