/*
 * test_vm.c - unit tests for src/compiler/compiler, chunk and src/vm.
 *
 * Programs are run through the full pipeline (parse, check, compile, VM)
 * and their printed output is compared with the expected text.
 *
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/checker.h"
#include "compiler/compiler.h"
#include "parser/parser.h"
#include "vm/vm.h"

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

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_write(void *ctx, const char *s, size_t n)
{
    Buf *b = (Buf *)ctx;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512;
        char *p;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        p = (char *)realloc(b->data, cap);
        if (p == NULL) {
            exit(2);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_clear(Buf *b)
{
    b->len = 0;
    buf_write(b, "", 0);
}

static Buf g_out;
static Buf g_err;

/* Full pipeline. Returns the VM status, or -1 if an earlier stage failed
 * (its messages go to g_err as "line:col: message\n"). */
static int run_limits(const char *src, size_t compile_limit, size_t vm_limit)
{
    MflParseResult pr;
    MflCheckResult cr;
    MflCompileResult comp;
    const MflDiagnostic *d;
    char line[400];
    int status = -1;

    buf_clear(&g_out);
    buf_clear(&g_err);
    if (!mfl_parse(src, strlen(src), 0, &pr)) {
        buf_write(&g_err, "<parse error>\n", 14);
        mfl_parse_free(&pr);
        return -1;
    }
    if (!mfl_check(&pr.program, 0, &cr)) {
        buf_write(&g_err, "<check error>\n", 14);
        mfl_check_free(&cr);
        mfl_parse_free(&pr);
        return -1;
    }
    if (mfl_compile(&pr.program, &cr, compile_limit, &comp)) {
        status = (int)mfl_vm_run(&comp.chunk, buf_write, &g_out, vm_limit);
    } else if (comp.out_of_memory) {
        buf_write(&g_err, "<compile oom>\n", 14);
    }
    for (d = comp.diagnostics; d != NULL; d = d->next) {
        snprintf(line, sizeof line, "%d:%d: %s\n", d->line, d->column, d->message);
        buf_write(&g_err, line, strlen(line));
    }
    mfl_compile_free(&comp);
    mfl_check_free(&cr);
    mfl_parse_free(&pr);
    return status;
}

static int run(const char *src) { return run_limits(src, 0, 0); }

#define EXPECT_OUTPUT(src, expected)                                                  \
    do {                                                                              \
        int st_ = run(src);                                                           \
        g_checks++;                                                                   \
        if (st_ != MFL_VM_OK || strcmp(g_out.data, expected) != 0) {                  \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: status %d\nsource:\n%s\n--- expected:\n%s--- got:\n%s%s", \
                    __FILE__, __LINE__, st_, src, expected, g_out.data, g_err.data);  \
        }                                                                             \
    } while (0)

#define EXPECT_COMPILE_ERROR(src, expected)                                           \
    do {                                                                              \
        int st_ = run(src);                                                           \
        g_checks++;                                                                   \
        if (st_ != -1 || strcmp(g_err.data, expected) != 0) {                         \
            g_failures++;                                                             \
            fprintf(stderr, "%s:%d: source:\n%s\n--- expected:\n%s--- got:\n%s",          \
                    __FILE__, __LINE__, src, expected, g_err.data);                   \
        }                                                                             \
    } while (0)

/* ------------------------------------------------------------------------ */

static void test_conformance_heat_input(void)
{
    EXPECT_OUTPUT("let voltage = 20 V\n"
                  "let current = 150 A\n"
                  "let speed = 5 mm/s\n"
                  "let efficiency = 0.8\n"
                  "\n"
                  "let energy = efficiency * voltage * current / speed\n"
                  "\n"
                  "print(energy.to(J/mm))\n",
                  "480 J/mm\n");
    /* Same physics, other units: 0.48 kJ/mm, 480000 J/m. */
    EXPECT_OUTPUT("let q = 0.8 * 20 V * 150 A / (300 mm/min)\n"
                  "print(q.to(kJ/mm), q.to(J/m), q.value_in(J/mm))\n",
                  "0.48 kJ/mm 480000 J/m 480\n");
}

static void test_display_units(void)
{
    /* Literals keep the unit as written (normalised). */
    EXPECT_OUTPUT("print(0.1 mm, 5 mm/s, 45 W/(m*K), 7850 kg/m^3, 2 m^-2)",
                  "0.1 mm 5 mm/s 45 W/(m*K) 7850 kg/m^3 2 m^-2\n");
    EXPECT_OUTPUT("print(1 W/((m*K)), 3 kg*m/s^2, 1 J/(kg*K))",
                  "1 W/(m*K) 3 kg*m/s^2 1 J/(kg*K)\n");
    /* + - % and negation keep a common unit... */
    EXPECT_OUTPUT("print(1 mm + 2 mm, 5 s - 7 s, (7 s) % (2 s), -(3 V))",
                  "3 mm -2 s 1 s -3 V\n");
    /* ...otherwise the result is shown in coherent SI. */
    EXPECT_OUTPUT("print(1 m + 1 mm, 2 * (5 mm), (2 m) * (3 m), (10 m) / (2 s), (2 m)^3)",
                  "1.001 m 0.01 m 6 m^2 5 m/s 8 m^3\n");
    /* A registered SI symbol is used when exactly one matches. */
    EXPECT_OUTPUT("print(0.8 * 20 V * 150 A, (2 mA) * (3 kohm), (1 N) * (1 m), 1 kohm + 2 Mohm)",
                  "2400 W 6 V 1 J 2001000 ohm\n");
    /* Dimensionless results are plain numbers. */
    EXPECT_OUTPUT("print((1 m) / (1 mm), (3 s) / (1 min))", "1000 0.05\n");
}

static void test_conversions(void)
{
    EXPECT_OUTPUT("print((1 km).to(m), (1 m).to(km), (90 km/h).to(m/s))",
                  "1000 m 0.001 km 25 m/s\n");
    EXPECT_OUTPUT("print((1 kJ/mm).value_in(J/m), (15 L/min).to(m^3/s), (2400 W).to(kW))",
                  "1000000 0.00025 m^3/s 2.4 kW\n");
    EXPECT_OUTPUT("print((1 bar).to(kPa), (1 h).to(min), (1 g/cm^3).to(kg/m^3))",
                  "100 kPa 60 min 1000 kg/m^3\n");
    /* Converting to the unit a value already has is exact. */
    EXPECT_OUTPUT("let x = 0.1 mm\nprint(x.to(mm), x.value_in(mm))", "0.1 mm 0.1\n");
}

static void test_arithmetic(void)
{
    EXPECT_OUTPUT("print(1 + 2 * 3, (1 + 2) * 3, 2^10, 2^-1, 2^3^2, -2^2)",
                  "7 9 1024 0.5 512 -4\n");
    EXPECT_OUTPUT("print(7 % 3, -7 % 3, 7 % -3, 10 / 4)", "1 -1 1 2.5\n");
    EXPECT_OUTPUT("print(0.1 + 0.2, 1 / 3)", "0.30000000000000004 0.3333333333333333\n");
    /* IEEE 754: no run-time error for division by zero. */
    EXPECT_OUTPUT("print(1 / 0, -1 / 0, 0 / 0, 1e300 * 1e10)", "inf -inf nan inf\n");
}

static void test_comparisons_and_logic(void)
{
    EXPECT_OUTPUT("print(1 bar == 1e5 Pa, 1 km > 999 m, 1 mm == 1 mm, 2 mm != 2 mm)",
                  "true true true false\n");
    EXPECT_OUTPUT("print(1 < 2, 2 <= 2, 3 > 4, 4 >= 5, 0.1 + 0.2 == 0.3)",
                  "true true false false false\n");
    EXPECT_OUTPUT("print(nil == nil, nil == 1, \"a\" == \"a\", \"a\" != \"b\", true == false)",
                  "true false true true false\n");
    EXPECT_OUTPUT("print(true and false, true or false, not true, not (1 > 2))",
                  "false true false true\n");
    /* Short-circuit: the right side is skipped (it would print otherwise). */
    EXPECT_OUTPUT("let a = false and print(\"no\") == nil\n"
                  "let b = true or print(\"no\") == nil\n"
                  "let c = true and print(\"yes\") == nil\n"
                  "print(a, b, c)",
                  "yes\nfalse true true\n");
}

static void test_variables_and_print(void)
{
    EXPECT_OUTPUT("var t = 0 s\nt = t + 0.5 s\nt = t + 0.5 s\nprint(t)", "1 s\n");
    EXPECT_OUTPUT("var x = 1 m\nx = 5 mm\nprint(x, x + 1 m)", "5 mm 1.005 m\n");
    EXPECT_OUTPUT("let a = 1; let b = a + 1; print(a, b)", "1 2\n");
    EXPECT_OUTPUT("print()\nprint(nil)\nprint(\"\")", "\nnil\n\n");
    EXPECT_OUTPUT("print(\"tab:\\tq\\\"uote\\\" back\\\\slash\\nnext\")",
                  "tab:\tq\"uote\" back\\slash\nnext\n");
    EXPECT_OUTPUT("let p = print(1)\nprint(p)", "1\nnil\n");
    EXPECT_OUTPUT("", "");
}

static void test_compile_errors(void)
{
    EXPECT_COMPILE_ERROR("let x = 1e400", "1:9: number literal too large (beyond about 1.8e308)\n");
    EXPECT_COMPILE_ERROR("let x = 1e-400 m",
                         "1:9: number literal too small (rounds to zero)\n");
}

/* Compiled code for the heat-input program: exact stack needs. */
static void test_chunk_shape(void)
{
    const char *src = "let a = 20 V\nlet b = a * 150 A\nprint(b.to(kW))";
    MflParseResult pr;
    MflCheckResult cr;
    MflCompileResult comp;
    CHECK(mfl_parse(src, strlen(src), 0, &pr));
    CHECK(mfl_check(&pr.program, 0, &cr));
    CHECK(mfl_compile(&pr.program, &cr, 0, &comp));
    CHECK(comp.chunk.global_count == 2);
    CHECK(comp.chunk.max_stack == 2);
    CHECK(comp.chunk.code[comp.chunk.code_len - 1] == MFL_OP_HALT);
    CHECK(comp.chunk.unit_len == 4); /* V, A, W (default), kW */
    mfl_compile_free(&comp);
    mfl_check_free(&cr);
    mfl_parse_free(&pr);
}

/* Every memory budget fails cleanly or produces the right output. */
static void test_out_of_memory(void)
{
    const char *src = "let q = 0.8 * 20 V * 150 A / (5 mm/s)\nprint(q.to(J/mm), \"ok\")";
    size_t limit;
    int saw_oom = 0;
    int saw_ok = 0;

    for (limit = 1; limit < 6000; limit += 5) {
        int st = run_limits(src, limit, 0);
        if (st == MFL_VM_OK) {
            saw_ok = 1;
            CHECK(strcmp(g_out.data, "480 J/mm ok\n") == 0);
        } else {
            saw_oom = 1;
            CHECK(st == -1 && strstr(g_err.data, "<compile oom>") != NULL);
        }
    }
    CHECK(saw_oom && saw_ok);

    saw_oom = saw_ok = 0;
    for (limit = 1; limit < 400; limit++) {
        int st = run_limits(src, 0, limit);
        if (st == MFL_VM_OK) {
            saw_ok = 1;
        } else {
            saw_oom = 1;
            CHECK(st == MFL_VM_OUT_OF_MEMORY && g_out.len == 0);
        }
    }
    CHECK(saw_oom && saw_ok);
}

/* Damaged bytecode must be rejected, never crash. */
static void test_invalid_bytecode(void)
{
    const char *src = "let a = 20 V\nvar b = a * 150 A\nb = b + 1 W\n"
                      "print(b.to(kW), a > 1 V and b < 1 MW, not true, (b).value_in(W) % 7)";
    MflParseResult pr;
    MflCheckResult cr;
    MflCompileResult comp;
    unsigned long long seed = 77;
    int round;
    int ok_runs = 0;
    int rejected = 0;
    unsigned char *orig;
    size_t len;
    Buf sink = {NULL, 0, 0};

    CHECK(mfl_parse(src, strlen(src), 0, &pr));
    CHECK(mfl_check(&pr.program, 0, &cr));
    CHECK(mfl_compile(&pr.program, &cr, 0, &comp));
    len = comp.chunk.code_len;
    orig = (unsigned char *)malloc(len);
    if (orig == NULL) {
        exit(2);
    }
    memcpy(orig, comp.chunk.code, len);

    for (round = 0; round < 20000; round++) {
        int flips = 1 + (int)(seed % 3);
        int k;
        MflVmStatus st;
        memcpy(comp.chunk.code, orig, len);
        for (k = 0; k < flips; k++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            comp.chunk.code[(seed >> 33) % len] = (unsigned char)(seed >> 17);
        }
        /* Also try truncated code. */
        if (round % 7 == 0) {
            comp.chunk.code_len = (size_t)((seed >> 40) % len);
        }
        sink.len = 0;
        st = mfl_vm_run(&comp.chunk, buf_write, &sink, 0);
        comp.chunk.code_len = len;
        if (st == MFL_VM_OK) {
            ok_runs++;
        } else if (st == MFL_VM_INVALID_CODE) {
            rejected++;
        } else {
            CHECK(0);
        }
    }
    CHECK(rejected > 0);
    printf("test_vm: damaged bytecode: %d rejected, %d still ran\n", rejected, ok_runs);
    memcpy(comp.chunk.code, orig, len);
    free(orig);
    free(sink.data);
    mfl_compile_free(&comp);
    mfl_check_free(&cr);
    mfl_parse_free(&pr);
}

/* A host program (e.g. a game) may run in a locale with ',' as decimal
 * separator; programs must read and print exactly the same. */
static void test_comma_locale(void)
{
    static const char *const names[] = {"pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "de_DE.utf8",
                                        "pt-BR",       "de-DE",      "Portuguese_Brazil.1252",
                                        "German_Germany.1252"};
    size_t i;
    const char *used = NULL;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (setlocale(LC_ALL, names[i]) != NULL && strcmp(localeconv()->decimal_point, ",") == 0) {
            used = names[i];
            break;
        }
    }
    if (used == NULL) {
        setlocale(LC_ALL, "C");
        printf("test_vm: comma-locale test SKIPPED (no such locale installed)\n");
        return;
    }
    printf("test_vm: comma-locale test ran with %s\n", used);
    EXPECT_OUTPUT("let q = 0.8 * 20 V * 150 A / (5 mm/s)\nprint(q.to(J/mm), 0.8, 1.5e-5, 2.5 mm)",
                  "480 J/mm 0.8 1.5e-5 2.5 mm\n");
    EXPECT_OUTPUT("print(123456789012345678901234567890, 1 / 3)",
                  "1.2345678901234568e29 0.3333333333333333\n");
    setlocale(LC_ALL, "C");
}

int main(void)
{
    test_conformance_heat_input();
    test_display_units();
    test_conversions();
    test_arithmetic();
    test_comparisons_and_logic();
    test_variables_and_print();
    test_compile_errors();
    test_chunk_shape();
    test_out_of_memory();
    test_invalid_bytecode();
    test_comma_locale();

    free(g_out.data);
    free(g_err.data);
    printf("test_vm: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
