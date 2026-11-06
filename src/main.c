/*
 * main.c - `mfl` command-line driver.
 *
 *   mfl file.mfl              check, compile and run
 *   mfl --tokens file.mfl     token stream
 *   mfl --ast file.mfl        syntax tree (syntax only)
 *   mfl --types file.mfl      inferred type of every declared name
 *   mfl --bytecode file.mfl   compiled bytecode listing
 *
 * Exit status: 0 success, 1 errors in the program or bad usage,
 * 3 internal failure (out of memory, invalid bytecode).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/checker.h"
#include "compiler/compiler.h"
#include "lexer/lexer.h"
#include "mfl/mfl.h"
#include "parser/parser.h"
#include "vm/vm.h"

static void usage(FILE *out)
{
    fprintf(out,
            "usage: mfl <file.mfl>              run a program\n"
            "       mfl --tokens <file.mfl>     print the token stream\n"
            "       mfl --ast <file.mfl>        print the syntax tree\n"
            "       mfl --types <file.mfl>      check and print inferred types\n"
            "       mfl --bytecode <file.mfl>   compile and print the bytecode\n"
            "       mfl --version | --help\n"
            "\n"
            "MFL %s - pre-alpha (v0.1 subset: let, var, arithmetic with units, print).\n",
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

static void write_stdout(void *ctx, const char *data, size_t length)
{
    (void)ctx;
    fwrite(data, 1, length, stdout);
}

enum { MODE_RUN, MODE_AST, MODE_TYPES, MODE_BYTECODE };

static void print_diagnostics(const char *path, const MflDiagnostic *d)
{
    for (; d != NULL; d = d->next) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, d->line, d->column, d->message);
    }
}

/* Runs the pipeline up to the stage the mode needs. Diagnostics go to
 * stderr as "file:line:col: error: msg". Returns the exit status. */
static int process_file(const char *path, int mode)
{
    MflParseResult res;
    size_t len = 0;
    char *src = read_file(path, &len);
    int ok;
    int status = 0;

    if (src == NULL) {
        fprintf(stderr, "mfl: cannot read '%s'\n", path);
        return 1;
    }
    ok = mfl_parse(src, len, 0, &res);
    print_diagnostics(path, res.diagnostics);
    if (res.out_of_memory) {
        fprintf(stderr, "mfl: out of memory while parsing '%s'\n", path);
    }
    if (ok && mode == MODE_AST) {
        mfl_ast_print_program(&res.program, write_stdout, NULL);
    }
    if (ok && mode != MODE_AST) {
        MflCheckResult chk;
        ok = mfl_check(&res.program, 0, &chk);
        print_diagnostics(path, chk.diagnostics);
        if (chk.out_of_memory) {
            fprintf(stderr, "mfl: out of memory while checking '%s'\n", path);
        }
        if (ok && mode == MODE_TYPES) {
            mfl_check_print_symbols(&chk, write_stdout, NULL);
        }
        if (ok && (mode == MODE_RUN || mode == MODE_BYTECODE)) {
            MflCompileResult comp;
            ok = mfl_compile(&res.program, &chk, 0, &comp);
            print_diagnostics(path, comp.diagnostics);
            if (comp.out_of_memory) {
                fprintf(stderr, "mfl: out of memory while compiling '%s'\n", path);
                status = 3;
            }
            if (ok && mode == MODE_BYTECODE) {
                mfl_chunk_disassemble(&comp.chunk, write_stdout, NULL);
            }
            if (ok && mode == MODE_RUN) {
                MflVmStatus vs = mfl_vm_run(&comp.chunk, write_stdout, NULL, 0);
                fflush(stdout);
                if (vs != MFL_VM_OK) {
                    fprintf(stderr, "mfl: %s while running '%s'\n",
                            vs == MFL_VM_OUT_OF_MEMORY ? "out of memory" : "invalid bytecode",
                            path);
                    ok = 0;
                    status = 3;
                }
            }
            mfl_compile_free(&comp);
        }
        mfl_check_free(&chk);
    }
    mfl_parse_free(&res);
    free(src);
    if (status != 0) {
        return status;
    }
    return ok ? 0 : 1;
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
    if (strcmp(argv[1], "--ast") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 1;
        }
        return process_file(argv[2], MODE_AST);
    }
    if (strcmp(argv[1], "--types") == 0 || strcmp(argv[1], "--bytecode") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 1;
        }
        return process_file(argv[2], argv[1][2] == 't' ? MODE_TYPES : MODE_BYTECODE);
    }
    if (argv[1][0] == '-') {
        fprintf(stderr, "mfl: unknown option '%s'\n", argv[1]);
        usage(stderr);
        return 1;
    }
    if (argc != 2) {
        usage(stderr);
        return 1;
    }
    return process_file(argv[1], MODE_RUN);
}
