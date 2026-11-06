/*
 * lexer.h - MFL tokenizer (internal API).
 *
 * The lexer turns UTF-8 source text into a stream of tokens. It does not
 * allocate memory: tokens point back into the source buffer, which must
 * outlive them. Numeric and string literals are NOT converted here; the
 * lexer only validates their shape and reports their exact source span.
 *
 * Newline handling (see docs/language-spec.md, section 2):
 *   - NEWLINE tokens separate statements; runs of blank lines and
 *     comment-only lines collapse into a single NEWLINE.
 *   - No NEWLINE is produced at the start of the input, after another
 *     NEWLINE, after ';' or '{', inside '(' ... ')' or '[' ... ']', or
 *     after a token that cannot end an expression (binary operators,
 *     '=', ',', '.', '..', '->', ':', 'and', 'or', 'not').
 *
 * Errors do not stop the lexer: an MFL_TOK_ERROR token is returned with
 * a message, and scanning resumes after the offending text, so several
 * errors can be reported in one pass.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_LEXER_H
#define MFL_LEXER_H

#include <stddef.h>

/* X-macro: token kind, printable name. */
#define MFL_TOKEN_KINDS(X)                                                  \
    /* special */                                                           \
    X(EOF, "EOF") X(ERROR, "ERROR") X(NEWLINE, "NEWLINE")                   \
    /* literals and names */                                                \
    X(NUMBER, "NUMBER") X(STRING, "STRING") X(IDENT, "IDENT")               \
    /* keywords */                                                          \
    X(LET, "LET") X(VAR, "VAR") X(FN, "FN") X(RETURN, "RETURN")             \
    X(IF, "IF") X(ELSE, "ELSE") X(WHILE, "WHILE") X(FOR, "FOR")             \
    X(IN, "IN") X(TRUE, "TRUE") X(FALSE, "FALSE") X(NIL, "NIL")             \
    X(AND, "AND") X(OR, "OR") X(NOT, "NOT") X(IMPORT, "IMPORT")             \
    /* operators and punctuation */                                         \
    X(PLUS, "PLUS") X(MINUS, "MINUS") X(STAR, "STAR") X(SLASH, "SLASH")     \
    X(CARET, "CARET") X(PERCENT, "PERCENT")                                 \
    X(EQ, "EQ") X(EQ_EQ, "EQ_EQ") X(BANG_EQ, "BANG_EQ")                     \
    X(LT, "LT") X(LT_EQ, "LT_EQ") X(GT, "GT") X(GT_EQ, "GT_EQ")             \
    X(DOT, "DOT") X(DOT_DOT, "DOT_DOT") X(ARROW, "ARROW")                   \
    X(COMMA, "COMMA") X(COLON, "COLON") X(SEMICOLON, "SEMICOLON")           \
    X(LPAREN, "LPAREN") X(RPAREN, "RPAREN")                                 \
    X(LBRACKET, "LBRACKET") X(RBRACKET, "RBRACKET")                         \
    X(LBRACE, "LBRACE") X(RBRACE, "RBRACE")

typedef enum {
#define MFL_TOKEN_ENUM_(name, str) MFL_TOK_##name,
    MFL_TOKEN_KINDS(MFL_TOKEN_ENUM_)
#undef MFL_TOKEN_ENUM_
    MFL_TOK_COUNT_
} MflTokenKind;

typedef struct {
    MflTokenKind kind;
    const char  *start;   /* first byte of the token in the source */
    size_t       length;  /* length in bytes (0 for EOF and NEWLINE) */
    int          line;    /* 1-based */
    int          column;  /* 1-based, counted in Unicode code points */
    const char  *message; /* static error text for MFL_TOK_ERROR, else NULL */
} MflToken;

typedef struct {
    const char  *cur;
    const char  *end;
    int          line;
    int          column;
    int          group_depth; /* open '(' and '[' */
    MflTokenKind last;        /* last token returned (MFL_TOK_NEWLINE at start) */
} MflLexer;

/* Prepares a lexer over src[0 .. length). A leading UTF-8 BOM is skipped.
 * The buffer may contain NUL bytes and need not be NUL-terminated. */
void mfl_lexer_init(MflLexer *lx, const char *src, size_t length);

/* Returns the next token. After the end of input it keeps returning
 * MFL_TOK_EOF. */
MflToken mfl_lexer_next(MflLexer *lx);

/* Printable name of a token kind, e.g. "NUMBER" or "EQ_EQ". */
const char *mfl_token_kind_name(MflTokenKind kind);

#endif /* MFL_LEXER_H */
