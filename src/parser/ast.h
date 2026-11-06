/*
 * ast.h - MFL abstract syntax tree (internal API).
 *
 * All nodes live in an MflArena owned by the parse result. Names and
 * literal texts point into the SOURCE buffer, which must outlive the AST.
 * Literals are kept as source text: numbers are converted to double in a
 * later, locale-independent stage.
 *
 * Unit expressions in quantity literals (`5 mm/s`) reuse the ordinary
 * expression nodes: IDENT for a unit symbol, BINARY '*' '/' '^' for
 * products, quotients and integer powers. Deciding which identifiers are
 * units is the job of the semantic checker, not of the parser.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_AST_H
#define MFL_AST_H

#include <stddef.h>

#include "lexer/lexer.h"

typedef enum {
    MFL_EXPR_ERROR,    /* placeholder produced after a syntax error */
    MFL_EXPR_NUMBER,   /* as.text: digits as written, e.g. "1.5e-3" */
    MFL_EXPR_STRING,   /* as.text: including quotes, escapes not decoded */
    MFL_EXPR_BOOL,     /* as.boolean */
    MFL_EXPR_NIL,
    MFL_EXPR_IDENT,    /* as.text: the name */
    MFL_EXPR_UNARY,    /* as.unary: '-' or 'not' */
    MFL_EXPR_BINARY,   /* as.binary */
    MFL_EXPR_CALL,     /* as.call: callee(args...) */
    MFL_EXPR_MEMBER,   /* as.member: object.name */
    MFL_EXPR_INDEX,    /* as.index: object[index] */
    MFL_EXPR_QUANTITY  /* as.quantity: number followed by a unit expression */
} MflExprKind;

typedef struct MflExpr MflExpr;

typedef struct {
    const char *start;
    size_t      length;
} MflText;

struct MflExpr {
    MflExprKind kind;
    int         line;   /* position of the token that defines the node */
    int         column; /* (operator token for unary/binary) */
    MflExpr    *next;   /* next argument in a call's argument list */
    union {
        MflText text;
        int     boolean;
        struct {
            MflTokenKind op;
            MflExpr     *operand;
        } unary;
        struct {
            MflTokenKind op;
            MflExpr     *left;
            MflExpr     *right;
        } binary;
        struct {
            MflExpr *callee;
            MflExpr *args; /* linked through ->next */
            int      argc;
        } call;
        struct {
            MflExpr *object;
            MflText  name;
        } member;
        struct {
            MflExpr *object;
            MflExpr *index;
        } index;
        struct {
            MflExpr *value; /* MFL_EXPR_NUMBER */
            MflExpr *unit;
        } quantity;
    } as;
};

typedef enum {
    MFL_STMT_LET,    /* let name = value     (immutable binding) */
    MFL_STMT_VAR,    /* var name = value     (mutable binding)   */
    MFL_STMT_ASSIGN, /* target = value       (target: IDENT)     */
    MFL_STMT_EXPR    /* value                                    */
} MflStmtKind;

typedef struct MflStmt MflStmt;

struct MflStmt {
    MflStmtKind kind;
    int         line;
    int         column;
    MflStmt    *next;
    MflText     name;   /* LET, VAR */
    MflExpr    *target; /* ASSIGN */
    MflExpr    *value;
};

typedef struct {
    MflStmt *first; /* linked through ->next */
    int      count;
} MflProgram;

/* Output sink for the printer. */
typedef void (*MflWriteFn)(void *ctx, const char *data, size_t length);

/* Prints the program as S-expressions, one statement per line:
 *   "12:1 (let speed (quantity 5 (/ mm s)))"                           */
void mfl_ast_print_program(const MflProgram *program, MflWriteFn write, void *ctx);
void mfl_ast_print_expr(const MflExpr *expr, MflWriteFn write, void *ctx);

#endif /* MFL_AST_H */
