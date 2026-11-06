/*
 * lexer.c - MFL tokenizer.
 *
 * Character classes are tested with explicit ASCII ranges rather than
 * <ctype.h>, so results never depend on the host program's locale.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lexer/lexer.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Character helpers                                                         */
/* ------------------------------------------------------------------------ */

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(int c) { return is_ident_start(c) || is_digit(c); }

/* Byte at offset n from the cursor, or -1 past the end. */
static int peek(const MflLexer *lx, size_t n)
{
    if ((size_t)(lx->end - lx->cur) <= n) {
        return -1;
    }
    return (unsigned char)lx->cur[n];
}

/* Line break length at the cursor: 1 for "\n", 2 for "\r\n", else 0.
 * A lone "\r" is not a line break (it is whitespace). */
static size_t line_break_at(const MflLexer *lx);

/* Consumes one byte, keeping line and column (in code points) current. */
static void advance(MflLexer *lx)
{
    unsigned char c = (unsigned char)*lx->cur++;
    if (c == '\n') {
        lx->line++;
        lx->column = 1;
    } else if ((c & 0xC0) != 0x80) { /* not a UTF-8 continuation byte */
        lx->column++;
    }
}

static size_t line_break_at(const MflLexer *lx)
{
    if (peek(lx, 0) == '\n') {
        return 1;
    }
    if (peek(lx, 0) == '\r' && peek(lx, 1) == '\n') {
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Token construction                                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *start;
    int line;
    int column;
} Mark;

static Mark mark(const MflLexer *lx)
{
    Mark m;
    m.start = lx->cur;
    m.line = lx->line;
    m.column = lx->column;
    return m;
}

static MflToken finish(MflLexer *lx, MflTokenKind kind, Mark m)
{
    MflToken t;
    t.kind = kind;
    t.start = m.start;
    t.length = (size_t)(lx->cur - m.start);
    t.line = m.line;
    t.column = m.column;
    t.message = NULL;
    lx->last = kind;
    return t;
}

static MflToken error_token(MflLexer *lx, Mark m, const char *message)
{
    MflToken t = finish(lx, MFL_TOK_ERROR, m);
    t.message = message;
    return t;
}

/* ------------------------------------------------------------------------ */
/* Keywords                                                                  */
/* ------------------------------------------------------------------------ */

static const struct {
    const char  *text;
    MflTokenKind kind;
} keywords[] = {
    {"and", MFL_TOK_AND},       {"else", MFL_TOK_ELSE},
    {"false", MFL_TOK_FALSE},   {"fn", MFL_TOK_FN},
    {"for", MFL_TOK_FOR},       {"if", MFL_TOK_IF},
    {"import", MFL_TOK_IMPORT}, {"in", MFL_TOK_IN},
    {"let", MFL_TOK_LET},       {"nil", MFL_TOK_NIL},
    {"not", MFL_TOK_NOT},       {"or", MFL_TOK_OR},
    {"return", MFL_TOK_RETURN}, {"true", MFL_TOK_TRUE},
    {"var", MFL_TOK_VAR},       {"while", MFL_TOK_WHILE},
};

static MflTokenKind keyword_or_ident(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].text) == len && memcmp(keywords[i].text, s, len) == 0) {
            return keywords[i].kind;
        }
    }
    return MFL_TOK_IDENT;
}

/* ------------------------------------------------------------------------ */
/* Newline policy                                                            */
/* ------------------------------------------------------------------------ */

/* Tokens after which a line break does not end the statement. */
static int continues_line(MflTokenKind k)
{
    switch (k) {
    case MFL_TOK_NEWLINE: case MFL_TOK_SEMICOLON: case MFL_TOK_LBRACE:
    case MFL_TOK_PLUS: case MFL_TOK_MINUS: case MFL_TOK_STAR:
    case MFL_TOK_SLASH: case MFL_TOK_CARET: case MFL_TOK_PERCENT:
    case MFL_TOK_EQ: case MFL_TOK_EQ_EQ: case MFL_TOK_BANG_EQ:
    case MFL_TOK_LT: case MFL_TOK_LT_EQ: case MFL_TOK_GT: case MFL_TOK_GT_EQ:
    case MFL_TOK_DOT: case MFL_TOK_DOT_DOT: case MFL_TOK_ARROW:
    case MFL_TOK_COMMA: case MFL_TOK_COLON:
    case MFL_TOK_AND: case MFL_TOK_OR: case MFL_TOK_NOT:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Scanners                                                                  */
/* ------------------------------------------------------------------------ */

/* digits ('.' digits)? ([eE] [+-]? digits)?
 * A '.' not followed by a digit is left alone, so "5.to" and "0..n" work.
 * An 'e' not followed by a (signed) digit is left alone, so "2e" is the
 * number 2 followed by the identifier e. */
static MflToken scan_number(MflLexer *lx, Mark m)
{
    while (is_digit(peek(lx, 0))) {
        advance(lx);
    }
    if (peek(lx, 0) == '.' && is_digit(peek(lx, 1))) {
        advance(lx);
        while (is_digit(peek(lx, 0))) {
            advance(lx);
        }
    }
    if (peek(lx, 0) == 'e' || peek(lx, 0) == 'E') {
        int sign = peek(lx, 1) == '+' || peek(lx, 1) == '-';
        if (is_digit(peek(lx, sign ? 2 : 1))) {
            advance(lx);
            if (sign) {
                advance(lx);
            }
            while (is_digit(peek(lx, 0))) {
                advance(lx);
            }
        }
    }
    return finish(lx, MFL_TOK_NUMBER, m);
}

/* "..." on a single line, escapes \n \t \" \\. */
static MflToken scan_string(MflLexer *lx, Mark m)
{
    const char *bad_escape = NULL;
    advance(lx); /* opening quote */
    for (;;) {
        int c = peek(lx, 0);
        if (c == -1 || line_break_at(lx) > 0) {
            return error_token(lx, m, "unterminated string");
        }
        if (c == '"') {
            advance(lx);
            break;
        }
        if (c == '\\') {
            int e = peek(lx, 1);
            if (e == -1 || e == '\n' || (e == '\r' && peek(lx, 2) == '\n')) {
                advance(lx);
                return error_token(lx, m, "unterminated string");
            }
            if (e != 'n' && e != 't' && e != '"' && e != '\\' && bad_escape == NULL) {
                bad_escape = "invalid escape sequence in string (allowed: \\n \\t \\\" \\\\)";
            }
            advance(lx);
            advance(lx);
            continue;
        }
        advance(lx);
    }
    if (bad_escape != NULL) {
        return error_token(lx, m, bad_escape);
    }
    return finish(lx, MFL_TOK_STRING, m);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void mfl_lexer_init(MflLexer *lx, const char *src, size_t length)
{
    lx->cur = src;
    lx->end = src + length;
    lx->line = 1;
    lx->column = 1;
    lx->group_depth = 0;
    lx->last = MFL_TOK_NEWLINE;
    if (length >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        lx->cur += 3; /* UTF-8 BOM: not part of the program, no column */
    }
}

MflToken mfl_lexer_next(MflLexer *lx)
{
    Mark nl;
    int saw_newline = 0;
    Mark m;
    int c;

    nl.start = NULL;
    nl.line = 0;
    nl.column = 0;

    /* Whitespace, comments and line breaks. */
    for (;;) {
        size_t br = line_break_at(lx);
        c = peek(lx, 0);
        if (br > 0) {
            if (!saw_newline) {
                nl = mark(lx);
                saw_newline = 1;
            }
            while (br-- > 0) {
                advance(lx);
            }
        } else if (c == ' ' || c == '\t' || c == '\r') {
            advance(lx);
        } else if (c == '#') {
            while (peek(lx, 0) != -1 && line_break_at(lx) == 0) {
                advance(lx);
            }
        } else {
            break;
        }
    }

    if (saw_newline && lx->group_depth == 0 && !continues_line(lx->last)) {
        MflToken t;
        t.kind = MFL_TOK_NEWLINE;
        t.start = nl.start;
        t.length = 0;
        t.line = nl.line;
        t.column = nl.column;
        t.message = NULL;
        lx->last = MFL_TOK_NEWLINE;
        return t;
    }

    m = mark(lx);
    if (c == -1) {
        return finish(lx, MFL_TOK_EOF, m);
    }

    if (is_digit(c)) {
        return scan_number(lx, m);
    }
    if (is_ident_start(c)) {
        while (is_ident_char(peek(lx, 0))) {
            advance(lx);
        }
        return finish(lx, keyword_or_ident(m.start, (size_t)(lx->cur - m.start)), m);
    }
    if (c == '"') {
        return scan_string(lx, m);
    }

    advance(lx);
    switch (c) {
    case '+': return finish(lx, MFL_TOK_PLUS, m);
    case '*': return finish(lx, MFL_TOK_STAR, m);
    case '/': return finish(lx, MFL_TOK_SLASH, m);
    case '^': return finish(lx, MFL_TOK_CARET, m);
    case '%': return finish(lx, MFL_TOK_PERCENT, m);
    case ',': return finish(lx, MFL_TOK_COMMA, m);
    case ':': return finish(lx, MFL_TOK_COLON, m);
    case ';': return finish(lx, MFL_TOK_SEMICOLON, m);
    case '{': return finish(lx, MFL_TOK_LBRACE, m);
    case '}': return finish(lx, MFL_TOK_RBRACE, m);
    case '(':
    case '[':
        lx->group_depth++;
        return finish(lx, c == '(' ? MFL_TOK_LPAREN : MFL_TOK_LBRACKET, m);
    case ')':
    case ']':
        if (lx->group_depth > 0) {
            lx->group_depth--;
        }
        return finish(lx, c == ')' ? MFL_TOK_RPAREN : MFL_TOK_RBRACKET, m);
    case '-':
        if (peek(lx, 0) == '>') {
            advance(lx);
            return finish(lx, MFL_TOK_ARROW, m);
        }
        return finish(lx, MFL_TOK_MINUS, m);
    case '.':
        if (peek(lx, 0) == '.') {
            advance(lx);
            return finish(lx, MFL_TOK_DOT_DOT, m);
        }
        return finish(lx, MFL_TOK_DOT, m);
    case '=':
        if (peek(lx, 0) == '=') {
            advance(lx);
            return finish(lx, MFL_TOK_EQ_EQ, m);
        }
        return finish(lx, MFL_TOK_EQ, m);
    case '<':
        if (peek(lx, 0) == '=') {
            advance(lx);
            return finish(lx, MFL_TOK_LT_EQ, m);
        }
        return finish(lx, MFL_TOK_LT, m);
    case '>':
        if (peek(lx, 0) == '=') {
            advance(lx);
            return finish(lx, MFL_TOK_GT_EQ, m);
        }
        return finish(lx, MFL_TOK_GT, m);
    case '!':
        if (peek(lx, 0) == '=') {
            advance(lx);
            return finish(lx, MFL_TOK_BANG_EQ, m);
        }
        return error_token(lx, m, "unexpected '!' (use 'not' for logical negation)");
    default:
        break;
    }

    if (c >= 0x80) {
        /* Swallow the rest of the UTF-8 sequence: one error per character. */
        while (peek(lx, 0) >= 0x80 && peek(lx, 0) <= 0xBF) {
            advance(lx);
        }
        return error_token(lx, m,
                           "non-ASCII character outside a string or comment "
                           "(identifiers are ASCII in v0.1)");
    }
    return error_token(lx, m, "unexpected character");
}

const char *mfl_token_kind_name(MflTokenKind kind)
{
    static const char *const names[] = {
#define MFL_TOKEN_NAME_(name, str) str,
        MFL_TOKEN_KINDS(MFL_TOKEN_NAME_)
#undef MFL_TOKEN_NAME_
    };
    if ((unsigned)kind >= (unsigned)MFL_TOK_COUNT_) {
        return "?";
    }
    return names[kind];
}
