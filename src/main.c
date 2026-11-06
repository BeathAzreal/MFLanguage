/*
 * main.c - `mfl` command-line driver.
 *
 * Only the lexer exists so far. `mfl --tokens file.mfl` prints the token
 * stream; running a script still exits with status 2 because the parser,
 * compiler and VM are not implemented yet.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer/lexer.h"
#include "mfl/mfl.h"

static void usage(FILE *out)
{
    fprintf(out,
            "usage: mfl --version | --help\n"
            "       mfl --tokens <file.mfl>   print the token stream\n"
            "       mfl <file.mfl>            run a script (not implemented yet)\n"
            "\n"
            "MFL %s - pre-alpha. Only the lexer is implemented.\n",
            mfl_version());
}

/* Reads a whole file in binary mode. Returns NULL on failure. */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (f == NULL) {
        return NULL;
    }
    for (;;) {
        size_t n;
        if (len == cap) {
            size_t new_cap = cap == 0 ? 4096 : cap * 2;
            char *p = (char *)realloc(buf, new_cap);
            if (p == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = p;
            cap = new_cap;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            break;
        }
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

/* Prints a lexeme, escaping bytes that are not printable ASCII or UTF-8. */
static void print_lexeme(const char *s, size_t len)
{
    size_t i;
    putchar('\'');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            fputs("\\\\", stdout);
        } else if (c == '\'') {
            fputs("\\'", stdout);
        } else if (c == '\t') {
            fputs("\\t", stdout);
        } else if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c < 0x20 || c == 0x7F) {
            printf("\\x%02X", c);
        } else {
            putchar(c);
        }
    }
    putchar('\'');
}

/* Prints one token per line: "line:column KIND 'lexeme'". Returns the
 * number of ERROR tokens. */
static int dump_tokens(const char *src, size_t len)
{
    MflLexer lx;
    int errors = 0;

    mfl_lexer_init(&lx, src, len);
    for (;;) {
        MflToken t = mfl_lexer_next(&lx);
        printf("%d:%d %s", t.line, t.column, mfl_token_kind_name(t.kind));
        if (t.length > 0) {
            putchar(' ');
            print_lexeme(t.start, t.length);
        }
        if (t.kind == MFL_TOK_ERROR) {
            printf(" error: %s", t.message);
            errors++;
        }
        putchar('\n');
        if (t.kind == MFL_TOK_EOF) {
            break;
        }
    }
    return errors;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("mfl %s\n", mfl_version());
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--tokens") == 0) {
        char *src;
        size_t len = 0;
        int errors;
        if (argc != 3) {
            usage(stderr);
            return 1;
        }
        src = read_file(argv[2], &len);
        if (src == NULL) {
            fprintf(stderr, "mfl: cannot read '%s'\n", argv[2]);
            return 1;
        }
        errors = dump_tokens(src, len);
        free(src);
        return errors > 0 ? 1 : 0;
    }
    fprintf(stderr, "mfl: cannot run '%s': the interpreter is not implemented yet (v%s).\n",
            argv[1], mfl_version());
    return 2;
}
