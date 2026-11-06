/*
 * test_lexer.c - unit tests for src/lexer.
 *
 * Self-contained (no test framework). Exit status 0 means all checks
 * passed; each failure prints file:line and the failing expression.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "lexer/lexer.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define MAX_TOKENS 256

typedef struct {
    MflToken tok[MAX_TOKENS];
    int count; /* includes the final EOF */
} TokenList;

static void lex_n(TokenList *out, const char *src, size_t len)
{
    MflLexer lx;
    mfl_lexer_init(&lx, src, len);
    out->count = 0;
    while (out->count < MAX_TOKENS) {
        MflToken t = mfl_lexer_next(&lx);
        out->tok[out->count++] = t;
        if (t.kind == MFL_TOK_EOF) {
            break;
        }
    }
}

static void lex(TokenList *out, const char *src) { lex_n(out, src, strlen(src)); }

/* Compares token kinds against an MFL_TOK_EOF-terminated list. */
static int kinds_are(const TokenList *tl, const MflTokenKind *expected, const char *src,
                     int line)
{
    int i;
    for (i = 0; i < tl->count; i++) {
        if (tl->tok[i].kind != expected[i]) {
            fprintf(stderr, "%s:%d: source \"%s\": token %d is %s, expected %s\n", __FILE__,
                    line, src, i, mfl_token_kind_name(tl->tok[i].kind),
                    mfl_token_kind_name(expected[i]));
            return 0;
        }
        if (expected[i] == MFL_TOK_EOF) {
            return 1;
        }
    }
    fprintf(stderr, "%s:%d: source \"%s\": token stream too short\n", __FILE__, line, src);
    return 0;
}

#define EXPECT_KINDS(src, ...)                                                \
    do {                                                                      \
        static const MflTokenKind exp_[] = {__VA_ARGS__, MFL_TOK_EOF};        \
        TokenList tl_;                                                        \
        lex(&tl_, src);                                                       \
        g_checks++;                                                           \
        if (!kinds_are(&tl_, exp_, src, __LINE__)) {                          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static int lexeme_is(const MflToken *t, const char *text)
{
    return t->length == strlen(text) && memcmp(t->start, text, t->length) == 0;
}

/* ------------------------------------------------------------------------ */

static void test_heat_input_program(void)
{
    const char *src = "let voltage = 20 V\n"
                      "let current = 150 A\n"
                      "let speed = 5 mm/s\n"
                      "let efficiency = 0.8\n"
                      "\n"
                      "let energy = efficiency * voltage * current / speed\n"
                      "\n"
                      "print(energy.to(J/mm))\n";
    TokenList tl;

    EXPECT_KINDS(src,
                 MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_NUMBER, MFL_TOK_IDENT,
                 MFL_TOK_NEWLINE,
                 MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_NUMBER, MFL_TOK_IDENT,
                 MFL_TOK_NEWLINE,
                 MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_NUMBER, MFL_TOK_IDENT,
                 MFL_TOK_SLASH, MFL_TOK_IDENT, MFL_TOK_NEWLINE,
                 MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_NUMBER, MFL_TOK_NEWLINE,
                 MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_IDENT, MFL_TOK_STAR,
                 MFL_TOK_IDENT, MFL_TOK_STAR, MFL_TOK_IDENT, MFL_TOK_SLASH, MFL_TOK_IDENT,
                 MFL_TOK_NEWLINE,
                 MFL_TOK_IDENT, MFL_TOK_LPAREN, MFL_TOK_IDENT, MFL_TOK_DOT, MFL_TOK_IDENT,
                 MFL_TOK_LPAREN, MFL_TOK_IDENT, MFL_TOK_SLASH, MFL_TOK_IDENT, MFL_TOK_RPAREN,
                 MFL_TOK_RPAREN, MFL_TOK_NEWLINE);

    lex(&tl, src);
    /* "5" and "mm" on line 3; positions are 1-based. */
    CHECK(lexeme_is(&tl.tok[15], "5"));
    CHECK(tl.tok[15].line == 3 && tl.tok[15].column == 13);
    CHECK(lexeme_is(&tl.tok[16], "mm"));
    CHECK(tl.tok[16].line == 3 && tl.tok[16].column == 15);
    CHECK(lexeme_is(&tl.tok[23], "0.8"));
    /* The blank line 5 collapses into the NEWLINE that ends line 4. */
    CHECK(tl.tok[24].kind == MFL_TOK_NEWLINE && tl.tok[24].line == 4);
    CHECK(tl.tok[25].line == 6);
    /* EOF position: after the final newline. */
    CHECK(tl.tok[tl.count - 1].line == 9 && tl.tok[tl.count - 1].column == 1);
}

static void test_numbers(void)
{
    TokenList tl;
    const char *ok[] = {"20", "0.8", "1.5e-3", "6.02e23", "1E+5", "007", "3.14159", "2e10"};
    size_t i;

    for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        lex(&tl, ok[i]);
        CHECK(tl.count == 2);
        CHECK(tl.tok[0].kind == MFL_TOK_NUMBER);
        CHECK(lexeme_is(&tl.tok[0], ok[i]));
    }

    /* A '.' only belongs to the number when a digit follows. */
    EXPECT_KINDS("0..n", MFL_TOK_NUMBER, MFL_TOK_DOT_DOT, MFL_TOK_IDENT);
    EXPECT_KINDS("5.to(J)", MFL_TOK_NUMBER, MFL_TOK_DOT, MFL_TOK_IDENT, MFL_TOK_LPAREN,
                 MFL_TOK_IDENT, MFL_TOK_RPAREN);
    lex(&tl, "1.5.to");
    CHECK(lexeme_is(&tl.tok[0], "1.5"));

    /* An exponent needs digits; otherwise 'e' starts an identifier. */
    lex(&tl, "2e");
    CHECK(tl.tok[0].kind == MFL_TOK_NUMBER && lexeme_is(&tl.tok[0], "2"));
    CHECK(tl.tok[1].kind == MFL_TOK_IDENT && lexeme_is(&tl.tok[1], "e"));
    EXPECT_KINDS("1e+", MFL_TOK_NUMBER, MFL_TOK_IDENT, MFL_TOK_PLUS);

    /* Units may follow a number with or without a space. */
    lex(&tl, "5mm 2.5e3m 10 kJ/mm");
    CHECK(lexeme_is(&tl.tok[0], "5") && lexeme_is(&tl.tok[1], "mm"));
    CHECK(lexeme_is(&tl.tok[2], "2.5e3") && lexeme_is(&tl.tok[3], "m"));
    CHECK(lexeme_is(&tl.tok[4], "10") && lexeme_is(&tl.tok[5], "kJ"));

    /* No sign in the literal: '-' is a separate operator. */
    EXPECT_KINDS("-3", MFL_TOK_MINUS, MFL_TOK_NUMBER);
}

static void test_identifiers_and_keywords(void)
{
    TokenList tl;
    EXPECT_KINDS("let var fn return if else while for in true false nil and or not import",
                 MFL_TOK_LET, MFL_TOK_VAR, MFL_TOK_FN, MFL_TOK_RETURN, MFL_TOK_IF,
                 MFL_TOK_ELSE, MFL_TOK_WHILE, MFL_TOK_FOR, MFL_TOK_IN, MFL_TOK_TRUE,
                 MFL_TOK_FALSE, MFL_TOK_NIL, MFL_TOK_AND, MFL_TOK_OR, MFL_TOK_NOT,
                 MFL_TOK_IMPORT);
    /* Keywords are exact and case-sensitive. */
    EXPECT_KINDS("letter let_ Let LET lets _ T0 heat_input", MFL_TOK_IDENT, MFL_TOK_IDENT,
                 MFL_TOK_IDENT, MFL_TOK_IDENT, MFL_TOK_IDENT, MFL_TOK_IDENT, MFL_TOK_IDENT,
                 MFL_TOK_IDENT);
    lex(&tl, "heat_input2");
    CHECK(lexeme_is(&tl.tok[0], "heat_input2"));
}

static void test_operators(void)
{
    EXPECT_KINDS("+ - * / ^ % = == != < <= > >= . .. -> , : ; ( ) [ ] { }", MFL_TOK_PLUS,
                 MFL_TOK_MINUS, MFL_TOK_STAR, MFL_TOK_SLASH, MFL_TOK_CARET,
                 MFL_TOK_PERCENT, MFL_TOK_EQ, MFL_TOK_EQ_EQ, MFL_TOK_BANG_EQ, MFL_TOK_LT,
                 MFL_TOK_LT_EQ, MFL_TOK_GT, MFL_TOK_GT_EQ, MFL_TOK_DOT, MFL_TOK_DOT_DOT,
                 MFL_TOK_ARROW, MFL_TOK_COMMA, MFL_TOK_COLON, MFL_TOK_SEMICOLON,
                 MFL_TOK_LPAREN, MFL_TOK_RPAREN, MFL_TOK_LBRACKET, MFL_TOK_RBRACKET,
                 MFL_TOK_LBRACE, MFL_TOK_RBRACE);
    /* Longest match, no spaces. */
    EXPECT_KINDS("a<=b==c->d", MFL_TOK_IDENT, MFL_TOK_LT_EQ, MFL_TOK_IDENT, MFL_TOK_EQ_EQ,
                 MFL_TOK_IDENT, MFL_TOK_ARROW, MFL_TOK_IDENT);
    EXPECT_KINDS("x=-1", MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_MINUS, MFL_TOK_NUMBER);
    EXPECT_KINDS("...", MFL_TOK_DOT_DOT, MFL_TOK_DOT);
    EXPECT_KINDS("W/(m*K) kg/m^3", MFL_TOK_IDENT, MFL_TOK_SLASH, MFL_TOK_LPAREN,
                 MFL_TOK_IDENT, MFL_TOK_STAR, MFL_TOK_IDENT, MFL_TOK_RPAREN, MFL_TOK_IDENT,
                 MFL_TOK_SLASH, MFL_TOK_IDENT, MFL_TOK_CARET, MFL_TOK_NUMBER);
}

static void test_strings(void)
{
    TokenList tl;

    lex(&tl, "print(\"Hello, MFL!\")");
    CHECK(tl.tok[2].kind == MFL_TOK_STRING);
    CHECK(lexeme_is(&tl.tok[2], "\"Hello, MFL!\""));

    lex(&tl, "\"a\\nb\\t\\\"c\\\\\"");
    CHECK(tl.count == 2 && tl.tok[0].kind == MFL_TOK_STRING);

    /* '#' inside a string is not a comment. */
    lex(&tl, "\"# not a comment\" x");
    CHECK(tl.tok[0].kind == MFL_TOK_STRING && tl.tok[1].kind == MFL_TOK_IDENT);

    /* Invalid escape: one ERROR covering the whole string, then go on. */
    lex(&tl, "\"bad \\q escape\" x");
    CHECK(tl.tok[0].kind == MFL_TOK_ERROR && tl.tok[0].message != NULL);
    CHECK(lexeme_is(&tl.tok[0], "\"bad \\q escape\""));
    CHECK(tl.tok[1].kind == MFL_TOK_IDENT);

    /* Unterminated at end of line: error, then the next line lexes normally. */
    EXPECT_KINDS("let s = \"open\nlet y = 1", MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ,
                 MFL_TOK_ERROR, MFL_TOK_NEWLINE, MFL_TOK_LET, MFL_TOK_IDENT, MFL_TOK_EQ,
                 MFL_TOK_NUMBER);
    EXPECT_KINDS("\"open", MFL_TOK_ERROR);
    EXPECT_KINDS("\"ends with backslash\\", MFL_TOK_ERROR);
    EXPECT_KINDS("\"backslash newline\\\nx", MFL_TOK_ERROR, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
}

static void test_newlines(void)
{
    /* Blank lines, comment-only lines and leading newlines collapse. */
    EXPECT_KINDS("\n\n# header\na\n\n  # c\n\nb\n", MFL_TOK_IDENT, MFL_TOK_NEWLINE,
                 MFL_TOK_IDENT, MFL_TOK_NEWLINE);
    /* Trailing comment. */
    EXPECT_KINDS("a # comment\nb", MFL_TOK_IDENT, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
    /* No final newline. */
    EXPECT_KINDS("a", MFL_TOK_IDENT);
    /* Inside () and [] newlines are ignored, even nested. */
    EXPECT_KINDS("f(a,\n  b,\n  [1,\n 2]\n)\nx", MFL_TOK_IDENT, MFL_TOK_LPAREN, MFL_TOK_IDENT,
                 MFL_TOK_COMMA, MFL_TOK_IDENT, MFL_TOK_COMMA, MFL_TOK_LBRACKET,
                 MFL_TOK_NUMBER, MFL_TOK_COMMA, MFL_TOK_NUMBER, MFL_TOK_RBRACKET,
                 MFL_TOK_RPAREN, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
    /* A line ending in a binary operator continues (comment allowed). */
    EXPECT_KINDS("x = a *  # times\n    b\ny", MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_IDENT,
                 MFL_TOK_STAR, MFL_TOK_IDENT, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
    EXPECT_KINDS("ok = a and\n b", MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_IDENT, MFL_TOK_AND,
                 MFL_TOK_IDENT);
    EXPECT_KINDS("x =\n 1", MFL_TOK_IDENT, MFL_TOK_EQ, MFL_TOK_NUMBER);
    /* A line starting with an operator does NOT join the previous one. */
    EXPECT_KINDS("a\n- b", MFL_TOK_IDENT, MFL_TOK_NEWLINE, MFL_TOK_MINUS, MFL_TOK_IDENT);
    /* Braces: no NEWLINE right after '{'; statements inside are separated. */
    EXPECT_KINDS("if x {\n  a\n  b\n}\n", MFL_TOK_IF, MFL_TOK_IDENT, MFL_TOK_LBRACE,
                 MFL_TOK_IDENT, MFL_TOK_NEWLINE, MFL_TOK_IDENT, MFL_TOK_NEWLINE,
                 MFL_TOK_RBRACE, MFL_TOK_NEWLINE);
    /* ';' as separator; no extra NEWLINE after it. */
    EXPECT_KINDS("a; b;\nc", MFL_TOK_IDENT, MFL_TOK_SEMICOLON, MFL_TOK_IDENT,
                 MFL_TOK_SEMICOLON, MFL_TOK_IDENT);
    /* Windows line endings behave like LF. */
    EXPECT_KINDS("a\r\n\r\nb\r\n", MFL_TOK_IDENT, MFL_TOK_NEWLINE, MFL_TOK_IDENT,
                 MFL_TOK_NEWLINE);
    /* CRLF: same tokens AND same positions as LF (Windows checkouts). */
    {
        TokenList lf, crlf;
        int i, same = 1;
        lex(&lf, "let a = 5 # c\nlet s = \"open\nx\n");
        lex(&crlf, "let a = 5 # c\r\nlet s = \"open\r\nx\r\n");
        CHECK(lf.count == crlf.count);
        for (i = 0; i < lf.count && i < crlf.count; i++) {
            if (lf.tok[i].kind != crlf.tok[i].kind || lf.tok[i].line != crlf.tok[i].line ||
                lf.tok[i].column != crlf.tok[i].column ||
                lf.tok[i].length != crlf.tok[i].length) {
                same = 0;
            }
        }
        CHECK(same);
        /* The unterminated string does not swallow the '\r'. */
        CHECK(crlf.tok[8].kind == MFL_TOK_ERROR && lexeme_is(&crlf.tok[8], "\"open"));
        CHECK(crlf.tok[4].kind == MFL_TOK_NEWLINE && crlf.tok[4].column == 14);
    }
    EXPECT_KINDS("\"esc\\\r\nx", MFL_TOK_ERROR, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
    /* A lone CR is whitespace, not a line break. */
    EXPECT_KINDS("a\rb", MFL_TOK_IDENT, MFL_TOK_IDENT);
    /* Unbalanced ')' does not make depth negative. */
    EXPECT_KINDS(")\na", MFL_TOK_RPAREN, MFL_TOK_NEWLINE, MFL_TOK_IDENT);
}

static void test_positions(void)
{
    TokenList tl;

    lex(&tl, "a\n  bb\tc");
    CHECK(tl.tok[0].line == 1 && tl.tok[0].column == 1);
    CHECK(tl.tok[1].kind == MFL_TOK_NEWLINE && tl.tok[1].line == 1 && tl.tok[1].column == 2);
    CHECK(tl.tok[2].line == 2 && tl.tok[2].column == 3);
    CHECK(tl.tok[3].line == 2 && tl.tok[3].column == 6); /* tab counts as one */

    /* Columns count code points: "µm" is 4 columns wide including quotes. */
    lex(&tl, "\"\xC2\xB5m\" x");
    CHECK(tl.tok[0].kind == MFL_TOK_STRING && tl.tok[0].length == 5);
    CHECK(tl.tok[1].column == 6);

    /* UTF-8 BOM is skipped and does not shift columns. */
    lex(&tl, "\xEF\xBB\xBFlet x");
    CHECK(tl.tok[0].kind == MFL_TOK_LET && tl.tok[0].column == 1);
    CHECK(tl.tok[1].column == 5);
}

static void test_errors(void)
{
    TokenList tl;

    /* Each bad character is one ERROR; lexing continues. */
    EXPECT_KINDS("a @ b $", MFL_TOK_IDENT, MFL_TOK_ERROR, MFL_TOK_IDENT, MFL_TOK_ERROR);
    lex(&tl, "!x");
    CHECK(tl.tok[0].kind == MFL_TOK_ERROR && strstr(tl.tok[0].message, "not") != NULL);
    CHECK(tl.tok[1].kind == MFL_TOK_IDENT);

    /* Non-ASCII outside strings: one ERROR for the whole code point. */
    lex(&tl, "5 \xC2\xB5m");
    CHECK(tl.tok[1].kind == MFL_TOK_ERROR && tl.tok[1].length == 2);
    CHECK(tl.tok[2].kind == MFL_TOK_IDENT && lexeme_is(&tl.tok[2], "m"));
    lex(&tl, "\xE2\x84\xA6"); /* U+2126 OHM SIGN, 3 bytes */
    CHECK(tl.tok[0].kind == MFL_TOK_ERROR && tl.tok[0].length == 3 && tl.count == 2);
    /* Non-ASCII in comments is fine. */
    EXPECT_KINDS("x # temperatura em \xC2\xB0" "C\ny", MFL_TOK_IDENT, MFL_TOK_NEWLINE,
                 MFL_TOK_IDENT);

    /* Embedded NUL is an error, not end of input. */
    lex_n(&tl, "a\0b", 3);
    CHECK(tl.count == 4);
    CHECK(tl.tok[1].kind == MFL_TOK_ERROR && tl.tok[2].kind == MFL_TOK_IDENT);
}

static void test_eof(void)
{
    MflLexer lx;
    MflToken t;

    mfl_lexer_init(&lx, "", 0);
    t = mfl_lexer_next(&lx);
    CHECK(t.kind == MFL_TOK_EOF && t.line == 1 && t.column == 1);
    t = mfl_lexer_next(&lx);
    CHECK(t.kind == MFL_TOK_EOF); /* EOF is sticky */

    EXPECT_KINDS("   \n\n # only a comment", MFL_TOK_EOF);

    /* The source need not be NUL-terminated: only `length` bytes are read. */
    mfl_lexer_init(&lx, "let xyz", 5);
    t = mfl_lexer_next(&lx);
    CHECK(t.kind == MFL_TOK_LET);
    t = mfl_lexer_next(&lx);
    CHECK(t.kind == MFL_TOK_IDENT && lexeme_is(&t, "x"));
}

static void test_kind_names(void)
{
    CHECK(strcmp(mfl_token_kind_name(MFL_TOK_NUMBER), "NUMBER") == 0);
    CHECK(strcmp(mfl_token_kind_name(MFL_TOK_RBRACE), "RBRACE") == 0);
    CHECK(strcmp(mfl_token_kind_name(MFL_TOK_COUNT_), "?") == 0);
}

/* Random bytes must always terminate, make progress and stay in bounds. */
static void test_random_input_terminates(void)
{
    static char buf[512];
    unsigned long seed = 12345UL;
    int round;

    for (round = 0; round < 2000; round++) {
        MflLexer lx;
        size_t len = (size_t)(seed % sizeof buf);
        size_t i;
        int steps = 0;
        int ok = 1;
        const char *prev_end = buf;

        for (i = 0; i < len; i++) {
            seed = seed * 1103515245UL + 12345UL;
            /* Bias toward characters the lexer cares about. */
            buf[i] = (char)((seed >> 16) % 4 == 0 ? "\"\\\n#.e0123+-()[]{}!=<>\r"[(seed >> 8) % 24]
                                                  : (int)((seed >> 16) & 0xFF));
        }
        mfl_lexer_init(&lx, buf, len);
        for (;;) {
            MflToken t = mfl_lexer_next(&lx);
            if (t.kind == MFL_TOK_EOF) {
                break;
            }
            if (t.kind != MFL_TOK_NEWLINE) {
                /* Tokens are non-empty, in bounds and in order. */
                if (t.length == 0 || t.start < prev_end || t.start + t.length > buf + len) {
                    ok = 0;
                    break;
                }
                prev_end = t.start + t.length;
            }
            if (++steps > (int)len + 2) {
                ok = 0;
                break;
            }
        }
        CHECK(ok);
        seed = seed * 1103515245UL + 12345UL;
    }
}

int main(void)
{
    test_heat_input_program();
    test_numbers();
    test_identifiers_and_keywords();
    test_operators();
    test_strings();
    test_newlines();
    test_positions();
    test_errors();
    test_eof();
    test_kind_names();
    test_random_input_terminates();

    printf("test_lexer: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
