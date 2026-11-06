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
    case MFL_EXPR_ARRAY:
        put(w, ctx, "(array");
        for (arg = e->as.array.elems; arg != NULL; arg = arg->next) {
            put(w, ctx, " ");
            mfl_ast_print_expr(arg, w, ctx);
        }
        put(w, ctx, ")");
        break;
    }
}

static void print_type_expr(const MflTypeExpr *t, MflWriteFn w, void *ctx)
{
    if (t == NULL) {
        put(w, ctx, "<null>");
        return;
    }
    switch (t->kind) {
    case MFL_TE_NUMBER: put(w, ctx, "Number"); break;
    case MFL_TE_BOOL: put(w, ctx, "Bool"); break;
    case MFL_TE_STRING: put(w, ctx, "String"); break;
    case MFL_TE_QUANTITY:
        put(w, ctx, "Quantity[");
        mfl_ast_print_expr(t->unit, w, ctx);
        put(w, ctx, "]");
        break;
    case MFL_TE_ARRAY:
        put(w, ctx, "Array[");
        print_type_expr(t->elem, w, ctx);
        put(w, ctx, "]");
        break;
    }
}

static void print_stmt(const MflStmt *s, MflWriteFn w, void *ctx);

static void print_block(const MflStmt *first, MflWriteFn w, void *ctx)
{
    const MflStmt *s;
    put(w, ctx, "(block");
    for (s = first; s != NULL; s = s->next) {
        put(w, ctx, " ");
        print_stmt(s, w, ctx);
    }
    put(w, ctx, ")");
}

static void print_stmt(const MflStmt *s, MflWriteFn w, void *ctx)
{
    const MflParam *prm;
    switch (s->kind) {
    case MFL_STMT_LET:
    case MFL_STMT_VAR:
        put(w, ctx, s->kind == MFL_STMT_LET ? "(let " : "(var ");
        put_text(w, ctx, s->name);
        if (s->annotation != NULL) {
            put(w, ctx, " : ");
            print_type_expr(s->annotation, w, ctx);
        }
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
    case MFL_STMT_FN:
        put(w, ctx, "(fn ");
        put_text(w, ctx, s->name);
        put(w, ctx, " (params");
        for (prm = s->params; prm != NULL; prm = prm->next) {
            put(w, ctx, " ");
            if (prm->type != NULL) {
                put(w, ctx, "(");
                put_text(w, ctx, prm->name);
                put(w, ctx, " : ");
                print_type_expr(prm->type, w, ctx);
                put(w, ctx, ")");
            } else {
                put_text(w, ctx, prm->name);
            }
        }
        put(w, ctx, ")");
        if (s->ret_type != NULL) {
            put(w, ctx, " -> ");
            print_type_expr(s->ret_type, w, ctx);
        }
        put(w, ctx, " ");
        print_block(s->body, w, ctx);
        break;
    case MFL_STMT_RETURN:
        put(w, ctx, "(return");
        if (s->value != NULL) {
            put(w, ctx, " ");
            mfl_ast_print_expr(s->value, w, ctx);
        }
        break;
    case MFL_STMT_IF:
        put(w, ctx, "(if ");
        mfl_ast_print_expr(s->value, w, ctx);
        put(w, ctx, " ");
        print_block(s->body, w, ctx);
        if (s->else_body != NULL) {
            put(w, ctx, " ");
            print_stmt(s->else_body, w, ctx);
        }
        break;
    case MFL_STMT_WHILE:
        put(w, ctx, "(while ");
        mfl_ast_print_expr(s->value, w, ctx);
        put(w, ctx, " ");
        print_block(s->body, w, ctx);
        break;
    case MFL_STMT_FOR:
        put(w, ctx, "(for ");
        put_text(w, ctx, s->name);
        put(w, ctx, " ");
        if (s->range_end != NULL) {
            put(w, ctx, "(range ");
            mfl_ast_print_expr(s->value, w, ctx);
            put(w, ctx, " ");
            mfl_ast_print_expr(s->range_end, w, ctx);
            put(w, ctx, ")");
        } else {
            mfl_ast_print_expr(s->value, w, ctx);
        }
        put(w, ctx, " ");
        print_block(s->body, w, ctx);
        break;
    case MFL_STMT_BLOCK:
        print_block(s->body, w, ctx);
        return; /* print_block closes its own parenthesis */
    }
    put(w, ctx, ")");
}

void mfl_ast_print_program(const MflProgram *program, MflWriteFn w, void *ctx)
{
    const MflStmt *s;
    char pos[48];

    for (s = program->first; s != NULL; s = s->next) {
        snprintf(pos, sizeof pos, "%d:%d ", s->line, s->column);
        put(w, ctx, pos);
        print_stmt(s, w, ctx);
        put(w, ctx, "\n");
    }
}
