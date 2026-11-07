/*
 * parser.h - MFL parser (internal API).
 *
 * Grammar implemented (v0.1 subset of docs/language-spec.md):
 *
 *   program    := separator* (statement (separator+ statement)*)? separator* EOF
 *   separator  := NEWLINE | ';'
 *   statement  := ('let' | 'var') IDENT '=' expr
 *               | expr ('=' expr)?            -- assignment if '=' follows
 *   expr       := or
 *   or         := and ('or' and)*
 *   and        := equality ('and' equality)*
 *   equality   := comparison (('==' | '!=') comparison)*
 *   comparison := term (('<' | '<=' | '>' | '>=') term)*
 *   term       := factor (('+' | '-') factor)*
 *   factor     := unary (('*' | '/' | '%') unary)*
 *   unary      := ('-' | 'not') unary | power
 *   power      := postfix ('^' unary)?        -- right-associative
 *   postfix    := primary ('(' args? ')' | '.' IDENT | '[' expr ']')*
 *   primary    := NUMBER unit? | STRING | 'true' | 'false' | 'nil'
 *               | IDENT | '(' expr ')'
 *   unit       := unit_term (('*' | '/') unit_term)*
 *   unit_term  := unit_atom ('^' ('+' | '-')? INTEGER)?
 *   unit_atom  := IDENT | '(' unit ')'
 *
 * A unit starts only with an IDENT right after a NUMBER. Inside a unit,
 * '*' and '/' continue the unit when followed by IDENT, or by '(' whose
 * next token is IDENT or '(' (so `5 mm/s` and `45 W/(m*K)` are single
 * quantities); otherwise they end it (`5 mm * 2` is (5 mm) * 2, and
 * `5 m / (2 s)` divides by the parenthesised quantity). Write
 * `(5 mm) / t` to divide a quantity by a variable.
 *
 * Keywords of later versions (fn, if, while, for, return, import) produce
 * a "not supported yet" error; the parser skips the whole construct,
 * including a braced block, and continues.
 *
 * Errors: each syntax error is recorded as a diagnostic, the parser skips
 * to the end of the statement and continues, so several errors are
 * reported in one pass (at most MFL_PARSE_MAX_DIAGNOSTICS).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_PARSER_H
#define MFL_PARSER_H

#include <stddef.h>

#include "parser/ast.h"
#include "runtime/arena.h"
#include "runtime/diagnostic.h"

#define MFL_PARSE_MAX_DIAGNOSTICS 50
#define MFL_PARSE_MAX_DEPTH 200


typedef struct {
    MflArena       arena;
    MflProgram     program;
    MflDiagnostic *diagnostics; /* in source order */
    int            diagnostic_count;
    int            out_of_memory; /* AST and diagnostics are unusable if set */
} MflParseResult;

/* Parses src[0 .. length). memory_limit caps arena usage in bytes
 * (0 = unlimited). Returns 1 on success (no diagnostics, no OOM), 0 if
 * there were syntax errors or memory ran out. Always call
 * mfl_parse_free() afterwards. */
int mfl_parse(const char *src, size_t length, size_t memory_limit, MflParseResult *out);

void mfl_parse_free(MflParseResult *result);

/* Parses a standalone unit expression such as "J/mm" or "W/(m*K)" (the
 * grammar of a quantity literal's unit). On success returns 1 and sets
 * *unit to the expression, which lives in out->arena. */
int mfl_parse_unit(const char *src, size_t length, MflParseResult *out, MflExpr **unit);

#endif /* MFL_PARSER_H */
