/*
 * checker.h - semantic checker for MFL v0.1 (internal API).
 *
 * Runs on a successfully parsed program and:
 *   - resolves names: undefined variables, redeclarations, assignment to
 *     'let' bindings, misuse of the built-in 'print';
 *   - resolves unit symbols in quantity literals and in .to()/.value_in()
 *     arguments against the unit registry (src/runtime/units.c);
 *   - infers a static type for every expression and checks dimensions:
 *     + - % and comparisons need equal dimensions, * and / combine them,
 *     ^ on a quantity needs an integer literal exponent, and/or/not need
 *     Bool, .to(u) needs the dimension of u.
 *
 * v0.1 has no functions or control flow, so every type is known before
 * the program runs; a variable keeps the type (and dimension) of its
 * initial value.
 *
 * Results are written into the AST (MflExpr.type, MflExpr.unit_scale);
 * diagnostics and the symbol table live in the result's own arena.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_CHECKER_H
#define MFL_CHECKER_H

#include <stddef.h>

#include "parser/ast.h"
#include "runtime/arena.h"
#include "runtime/diagnostic.h"

#define MFL_CHECK_MAX_DIAGNOSTICS 50

typedef struct MflSymbol MflSymbol;

struct MflSymbol {
    MflText    name;
    int        line;   /* declaration position */
    int        column;
    int        is_var; /* declared with 'var' (mutable) */
    MflType    type;
    MflSymbol *next;   /* declaration order */
};

typedef struct {
    MflArena       arena;
    MflDiagnostic *diagnostics; /* in source order */
    int            diagnostic_count;
    int            out_of_memory;
    MflSymbol     *symbols;
} MflCheckResult;

/* Checks a program produced by a successful mfl_parse(). Returns 1 when
 * there are no errors. Always call mfl_check_free() afterwards. */
int mfl_check(MflProgram *program, size_t memory_limit, MflCheckResult *out);

void mfl_check_free(MflCheckResult *result);

/* "Number", "Quantity[m/s]", "Bool", "String", "Nil", ... */
void mfl_type_format(MflType type, char *buf, size_t size);

/* One line per declared name: "12:1 let speed: Quantity[m/s]". */
void mfl_check_print_symbols(const MflCheckResult *result, MflWriteFn write, void *ctx);

#endif /* MFL_CHECKER_H */
