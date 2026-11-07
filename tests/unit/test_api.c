/*
 * test_api.c - tests of the public embedding API. Uses ONLY the public
 * header, like a host program would.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfl/mfl.h"

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

#define CHECK_STATUS(call, want)                                                       \
    do {                                                                               \
        MflStatus st_ = (call);                                                        \
        g_checks++;                                                                    \
        if (st_ != (want)) {                                                           \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s returned %s, want %s (message: %s)\n", __FILE__, \
                    __LINE__, #call, mfl_status_name(st_), mfl_status_name(want),      \
                    "see below");                                                      \
        }                                                                              \
    } while (0)

typedef struct {
    char   text[4096];
    size_t len;
} Capture;

static void capture(void *user, const char *text, size_t length)
{
    Capture *c = (Capture *)user;
    size_t room = sizeof c->text - 1 - c->len;
    size_t n = length < room ? length : room;
    memcpy(c->text + c->len, text, n);
    c->len += n;
    c->text[c->len] = '\0';
}

static MflState *new_capturing(Capture *cap, size_t limit)
{
    MflConfig cfg;
    cap->len = 0;
    cap->text[0] = '\0';
    cfg.output = capture;
    cfg.user = cap;
    cfg.memory_limit = limit;
    return mfl_state_new(&cfg);
}

static MflStatus eval(MflState *S, const char *src) { return mfl_eval(S, src, strlen(src), "t.mfl"); }

static int close_to(double a, double b) { return fabs(a - b) <= 1e-12 * fabs(b); }

/* ------------------------------------------------------------------------ */

static void test_heat_input(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    double q = 0.0;

    CHECK(S != NULL);
    CHECK_STATUS(mfl_set_number(S, "voltage", 20, "V"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "current", 150, "A"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "speed", 5, "mm/s"), MFL_OK);
    CHECK_STATUS(eval(S, "let energy = 0.8 * voltage * current / speed\nprint(energy.to(J/mm))"),
                 MFL_OK);
    CHECK(strcmp(cap.text, "480 J/mm\n") == 0);
    CHECK(strcmp(mfl_error_message(S), "") == 0);
    CHECK_STATUS(mfl_get_number(S, "energy", "J/mm", &q), MFL_OK);
    CHECK(q == 480.0);
    CHECK_STATUS(mfl_get_number(S, "energy", "kJ/mm", &q), MFL_OK);
    CHECK(q == 0.48);
    CHECK_STATUS(mfl_get_number(S, "energy", NULL, &q), MFL_OK); /* SI: J/m */
    CHECK(q == 480000.0);
    /* Inputs are readable too, in any compatible unit. */
    CHECK_STATUS(mfl_get_number(S, "speed", "mm/min", &q), MFL_OK);
    CHECK(q == 300.0);
    CHECK_STATUS(mfl_get_number(S, "speed", "m/s", &q), MFL_OK);
    CHECK(q == 0.005);
    mfl_state_free(S);
}

static void test_inputs(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    double v = 0.0;

    /* Inputs print in the unit the host gave (normalised). */
    CHECK_STATUS(mfl_set_number(S, "k", 45, "W/( m * K )"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "n", 3, NULL), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "x", 2.5, ""), MFL_OK);
    CHECK_STATUS(eval(S, "print(k, n, x, n * x)"), MFL_OK);
    CHECK(strcmp(cap.text, "45 W/(m*K) 3 2.5 7.5\n") == 0);

    /* Replacing an input; a new unit of the same dimension is converted. */
    cap.len = 0;
    CHECK_STATUS(mfl_set_number(S, "k", 0.045, "kW/(m*K)"), MFL_OK);
    CHECK_STATUS(eval(S, "print(k.to(W/(m*K)))"), MFL_OK);
    CHECK(strcmp(cap.text, "45 W/(m*K)\n") == 0);

    /* Inputs are immutable and cannot be redeclared. */
    CHECK_STATUS(eval(S, "k = 1 W/(m*K)"), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S),
                 "t.mfl:1:1: error: cannot assign to 'k': it is an input provided by the host "
                 "program") != NULL);
    CHECK_STATUS(eval(S, "let n = 4"), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S), "'n' is already defined as an input by the host program") !=
          NULL);

    /* Dimensional analysis sees the inputs' dimensions. */
    CHECK_STATUS(eval(S, "let bad = k + n"), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S), "cannot add Quantity[kg*m/(s^3*K)] and Number") != NULL);

    /* Clearing inputs. */
    mfl_clear_inputs(S);
    CHECK_STATUS(eval(S, "print(k)"), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S), "undefined variable 'k'") != NULL);
    CHECK_STATUS(mfl_get_number(S, "k", NULL, &v), MFL_OK); /* still from the last good run */
    mfl_state_free(S);
}

static void test_bad_arguments(void)
{
    MflState *S = mfl_state_new(NULL);
    double v;

    CHECK_STATUS(mfl_set_number(NULL, "a", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_get_number(NULL, "a", NULL, &v), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_eval(NULL, "", 0, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_script_run(NULL, NULL), MFL_ERR_ARGUMENT);
    CHECK(mfl_script_compile(NULL, "", 0, NULL) == NULL);
    CHECK(strcmp(mfl_error_message(NULL), "") == 0);
    mfl_state_free(NULL);
    mfl_script_free(NULL);
    mfl_clear_inputs(NULL);

    CHECK_STATUS(mfl_set_number(S, NULL, 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "2x", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "let", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "print", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "a b", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "a\n", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_set_number(S, "\xC2\xB5", 1, NULL), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "invalid input name") != NULL);
    CHECK_STATUS(mfl_set_number(S, "ok_name2", 1, NULL), MFL_OK);
    CHECK_STATUS(mfl_eval(S, NULL, 5, NULL), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_eval(S, NULL, 0, NULL), MFL_OK); /* empty program */
    CHECK_STATUS(mfl_get_number(S, "ok_name2", NULL, NULL), MFL_ERR_ARGUMENT);
    mfl_state_free(S);
}

static void test_units_in_api(void)
{
    MflState *S = mfl_state_new(NULL);
    double v = 0.0;

    CHECK_STATUS(mfl_set_number(S, "a", 1, "furlong"), MFL_ERR_UNIT);
    CHECK(strcmp(mfl_error_message(S), "invalid unit 'furlong': unknown unit 'furlong'") == 0);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "m/"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "m s"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "m^x"), MFL_ERR_UNIT);
    /* Absolute temperatures do not cross the API (no offset there): use K. */
    CHECK_STATUS(mfl_set_number(S, "a", 1, "degC"), MFL_ERR_UNIT);
    CHECK(strstr(mfl_error_message(S), "cannot be passed through the API; use K") != NULL);
    CHECK_STATUS(mfl_set_number(S, "a", 293.15, "K"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "a", 5, "percent"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "degF"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "5"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_set_number(S, "a", 1, "m + s"), MFL_ERR_UNIT);

    CHECK_STATUS(mfl_set_number(S, "rho", 7.85, "g/cm^3"), MFL_OK);
    CHECK_STATUS(eval(S, "let m = rho * (2 m^3)"), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "rho", "kg/m^3", &v), MFL_OK);
    CHECK(v == 7850.0);
    CHECK_STATUS(mfl_get_number(S, "m", "kg", &v), MFL_OK);
    CHECK(v == 15700.0);
    CHECK_STATUS(mfl_get_number(S, "m", "t", &v), MFL_ERR_UNIT); /* tonne is not a unit */
    CHECK_STATUS(mfl_get_number(S, "m", "s", &v), MFL_ERR_UNIT);
    CHECK(strcmp(mfl_error_message(S),
                 "cannot express 'm' (dimension kg) in 's' (dimension s)") == 0);
    /* A dimensionless value read with a dimensionless unit expression. */
    CHECK_STATUS(eval(S, "let ratio = (3 mm) / (1 m)"), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "ratio", NULL, &v), MFL_OK);
    CHECK(close_to(v, 0.003));
    CHECK_STATUS(mfl_get_number(S, "ratio", "mm/m", &v), MFL_OK);
    CHECK(close_to(v, 3.0));
    mfl_state_free(S);
}

static void test_results(void)
{
    MflState *S = mfl_state_new(NULL);
    double v = 0.0;

    CHECK_STATUS(mfl_get_number(S, "x", NULL, &v), MFL_ERR_NOT_RUN);
    CHECK_STATUS(eval(S, "let x = 2 s\nvar y = 1\ny = y + 1\nlet ok = true\nlet s = \"hi\""),
                 MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "x", "ms", &v), MFL_OK);
    CHECK(v == 2000.0);
    CHECK_STATUS(mfl_get_number(S, "y", NULL, &v), MFL_OK);
    CHECK(v == 2.0); /* final value of a var */
    CHECK_STATUS(mfl_get_number(S, "ok", NULL, &v), MFL_ERR_TYPE);
    CHECK(strcmp(mfl_error_message(S), "'ok' is a Bool, not a number") == 0);
    CHECK_STATUS(mfl_get_number(S, "s", NULL, &v), MFL_ERR_TYPE);
    CHECK_STATUS(mfl_get_number(S, "nope", NULL, &v), MFL_ERR_NOT_FOUND);

    /* A failed run keeps the previous results. */
    CHECK_STATUS(eval(S, "let x = 1 m + 1 s"), MFL_ERR_PROGRAM);
    CHECK_STATUS(mfl_get_number(S, "x", "s", &v), MFL_OK);
    CHECK(v == 2.0);
    /* A successful run replaces them. */
    CHECK_STATUS(eval(S, "let z = 1"), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "x", NULL, &v), MFL_ERR_NOT_FOUND);
    mfl_state_free(S);
}

static void test_program_errors(void)
{
    MflState *S = mfl_state_new(NULL);

    CHECK_STATUS(mfl_eval(S, "let = 1\nlet b = (2", 18, "model.mfl"), MFL_ERR_PROGRAM);
    CHECK(strcmp(mfl_error_message(S),
                 "model.mfl:1:5: error: expected variable name after 'let', found '='\n"
                 "model.mfl:2:11: error: expected ')' to close '(' at 2:9, found end of file\n") ==
          0);
    CHECK_STATUS(mfl_eval(S, "print(y)", 8, NULL), MFL_ERR_PROGRAM);
    CHECK(strcmp(mfl_error_message(S), "<script>:1:7: error: undefined variable 'y'\n") == 0);
    CHECK_STATUS(mfl_eval(S, "let x = 1e999", 13, "n.mfl"), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S), "n.mfl:1:9: error: number literal too large") != NULL);
    /* The source need not be NUL-terminated. */
    CHECK_STATUS(mfl_eval(S, "let a = 1GARBAGE", 9, NULL), MFL_OK);
    mfl_state_free(S);
}

static void test_compile_once_run_many(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    MflScript *sc;
    double q = 0.0;
    int i;
    const char *model = "let q = 0.8 * U * I / v\n";

    mfl_set_number(S, "U", 20, "V");
    mfl_set_number(S, "I", 150, "A");
    mfl_set_number(S, "v", 5, "mm/s");
    sc = mfl_script_compile(S, model, strlen(model), "model.mfl");
    CHECK(sc != NULL);
    for (i = 1; i <= 50; i++) {
        mfl_set_number(S, "v", (double)i, "mm/s");
        CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
        CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK);
        CHECK(close_to(q, 2400.0 / i));
    }
    /* Same dimension, different unit: still fine. */
    mfl_set_number(S, "v", 0.3, "m/min");
    CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK);
    CHECK(close_to(q, 480.0));
    /* Changed dimension or removed input: refused until recompiled. */
    mfl_set_number(S, "v", 5, "mm");
    CHECK_STATUS(mfl_script_run(S, sc), MFL_ERR_INPUTS_CHANGED);
    CHECK(strcmp(mfl_error_message(S),
                 "input 'v' no longer has dimension m/s; compile the script again") == 0);
    mfl_clear_inputs(S);
    CHECK_STATUS(mfl_script_run(S, sc), MFL_ERR_INPUTS_CHANGED);
    CHECK(strstr(mfl_error_message(S), "was removed") != NULL);
    /* Extra inputs do not matter. */
    mfl_set_number(S, "U", 20, "V");
    mfl_set_number(S, "I", 150, "A");
    mfl_set_number(S, "v", 5, "mm/s");
    mfl_set_number(S, "unused", 1, NULL);
    CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
    /* A script can outlive its state and run on another one. */
    mfl_state_free(S);
    S = new_capturing(&cap, 0);
    mfl_set_number(S, "v", 10, "mm/s");
    mfl_set_number(S, "I", 100, "A");
    mfl_set_number(S, "U", 25, "V");
    CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK);
    CHECK(close_to(q, 200.0));
    mfl_script_free(sc);
    mfl_state_free(S);
}

static void test_independent_states(void)
{
    MflState *A = mfl_state_new(NULL);
    MflState *B = mfl_state_new(NULL);
    double a = 0.0;
    double b = 0.0;
    mfl_set_number(A, "x", 1, "m");
    mfl_set_number(B, "x", 2, "m");
    CHECK_STATUS(eval(A, "let y = x * 2"), MFL_OK);
    CHECK_STATUS(eval(B, "let y = x * 3"), MFL_OK);
    CHECK_STATUS(mfl_get_number(A, "y", "m", &a), MFL_OK);
    CHECK_STATUS(mfl_get_number(B, "y", "m", &b), MFL_OK);
    CHECK(a == 2.0 && b == 6.0);
    mfl_state_free(A);
    mfl_state_free(B);
}

/* Every memory limit: clean failure or correct result, never a crash. */
static void test_memory_limits(void)
{
    size_t limit;
    int saw_oom = 0;
    int saw_ok = 0;
    for (limit = 1; limit < 5000; limit += 3) {
        Capture cap;
        MflState *S = new_capturing(&cap, limit);
        MflStatus st;
        double q = 0.0;
        if (S == NULL) {
            continue;
        }
        mfl_set_number(S, "U", 20, "V");
        mfl_set_number(S, "v", 5, "mm/s");
        /* (150 A) needs parentheses: `150 A / v` would read v as a unit. */
        st = eval(S, "let q = 0.8 * U * (150 A) / v\nprint(q.to(J/mm))");
        if (st == MFL_OK) {
            saw_ok = 1;
            CHECK(strcmp(cap.text, "480 J/mm\n") == 0);
            CHECK(mfl_get_number(S, "q", "J/mm", &q) == MFL_OK && q == 480.0);
        } else {
            saw_oom = 1;
            CHECK(st == MFL_ERR_MEMORY);
            CHECK(strstr(mfl_error_message(S), "out of memory") != NULL);
        }
        mfl_state_free(S);
    }
    CHECK(saw_oom && saw_ok);
}

/* v0.2: run-time errors reach the host, which can keep using MFL. */
static void test_runtime_errors(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    double q = 0.0;
    MflScript *model;
    const char *src = "fn heat(U: Quantity[V], I: Quantity[A], v: Quantity[mm/s]) -> Quantity[J/mm] {\n"
                      "    return 0.8 * U * I / v\n"
                      "}\n"
                      "let speeds = [5 mm/s, 10 mm/s]\n"
                      "let q = heat(voltage, 150 A, speeds[pick])\n";

    /* Index chosen by the host: valid, then out of range, then valid again. */
    mfl_set_number(S, "voltage", 20, "V");
    mfl_set_number(S, "pick", 0, NULL);
    model = mfl_script_compile(S, src, strlen(src), "model.mfl");
    CHECK(model != NULL);
    CHECK_STATUS(mfl_script_run(S, model), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK);
    CHECK(q == 480.0);
    mfl_set_number(S, "pick", 2, NULL);
    CHECK_STATUS(mfl_script_run(S, model), MFL_ERR_RUNTIME);
    CHECK(strcmp(mfl_error_message(S),
                 "model.mfl:5:30: runtime error: index 2 is out of range for an array of length 2 "
                 "(valid indices: 0 to 1)\n") == 0);
    CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK); /* previous results kept */
    CHECK(q == 480.0);
    mfl_set_number(S, "pick", 1, NULL);
    CHECK_STATUS(mfl_script_run(S, model), MFL_OK); /* the state is still usable */
    CHECK_STATUS(mfl_get_number(S, "q", "J/mm", &q), MFL_OK);
    CHECK(q == 240.0);
    mfl_script_free(model);

    /* Gradual typing: a dimension error only visible at run time. */
    CHECK_STATUS(eval(S, "fn add(a, b) { return a + b }\nlet r = add(1 m, 2 s)"), MFL_ERR_RUNTIME);
    CHECK(strstr(mfl_error_message(S), "t.mfl:1:25: runtime error: cannot add Quantity[m] and Quantity[s]") != NULL);
    /* Endless recursion. */
    CHECK_STATUS(eval(S, "fn f(n) { return f(n) }\nlet r = f(1)"), MFL_ERR_RUNTIME);
    CHECK(strstr(mfl_error_message(S), "call depth limit of 1000 exceeded") != NULL);
    /* An endless loop is stopped by the instruction limit... */
    mfl_set_instruction_limit(S, 100000);
    CHECK_STATUS(eval(S, "var i = 0\nwhile true { i = i + 1 }"), MFL_ERR_RUNTIME);
    CHECK(strstr(mfl_error_message(S), "instruction limit of 100000 exceeded") != NULL);
    /* ...which does not affect normal programs. */
    cap.len = 0;
    cap.text[0] = '\0';
    CHECK_STATUS(eval(S, "var s = 0\nfor i in 0..100 { s = s + i }\nprint(s)"), MFL_OK);
    CHECK(strcmp(cap.text, "4950\n") == 0);
    mfl_set_instruction_limit(S, 0);
    /* Output printed before a run-time error reaches the host. */
    cap.len = 0;
    cap.text[0] = '\0';
    CHECK_STATUS(eval(S, "print(\"before\")\nlet a = [1]\nprint(a[5])"), MFL_ERR_RUNTIME);
    CHECK(strcmp(cap.text, "before\n") == 0);
    CHECK(strcmp(mfl_status_name(MFL_ERR_RUNTIME), "MFL_ERR_RUNTIME") == 0);
    mfl_state_free(S);
}

/* v0.2 features through the API: functions, loops, arrays, temperatures. */
static void test_v02_programs(void)
{
    MflState *S = mfl_state_new(NULL);
    double v = 0.0;
    const char *src = "fn mean(xs: Array[Quantity[m/s]]) -> Quantity[m/s] {\n"
                      "    var s = xs[0]\n"
                      "    for k in 1..len(xs) { s = s + xs[k] }\n"
                      "    return s / len(xs)\n"
                      "}\n"
                      "let avg = mean([speed, 2 * speed, 3 * speed])\n"
                      "let preheat = (150 degC).to(K)\n";
    mfl_set_number(S, "speed", 5, "mm/s");
    CHECK_STATUS(mfl_eval(S, src, strlen(src), "v02.mfl"), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "avg", "mm/s", &v), MFL_OK);
    CHECK(close_to(v, 10.0));
    CHECK_STATUS(mfl_get_number(S, "preheat", "K", &v), MFL_OK);
    CHECK(close_to(v, 423.15));
    mfl_state_free(S);
}

static void test_status_names(void)
{
    CHECK(strcmp(mfl_status_name(MFL_OK), "MFL_OK") == 0);
    CHECK(strcmp(mfl_status_name(MFL_ERR_INPUTS_CHANGED), "MFL_ERR_INPUTS_CHANGED") == 0);
    CHECK(strcmp(mfl_status_name((MflStatus)999), "MFL_ERR_UNKNOWN") == 0);
    CHECK(strcmp(mfl_version(), MFL_VERSION_STRING) == 0);
}

static const char *g_source_dir = ".";

/* v0.3: imports through the API. File imports are off by default. */
static void test_imports(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    char name[1024];
    const char *uses_lib = "import \"lib/steel.mfl\"\n"
                           "let alpha = diffusivity(k_steel, rho_steel, cp_steel)\n";
    const char *modules = "import numerical\nlet m = numerical.mean([1 mm, 3 mm])\n"
                          "let T = 20 degC\n";
    double v = 0;

    snprintf(name, sizeof name, "%s/tests/language/imports/api_main.mfl", g_source_dir);
    CHECK_STATUS(mfl_eval(S, uses_lib, strlen(uses_lib), name), MFL_ERR_PROGRAM);
    CHECK(strstr(mfl_error_message(S), "file imports are not enabled") != NULL);
    CHECK(strstr(mfl_error_message(S), "api_main.mfl:1:1: error:") != NULL);

    mfl_set_file_imports(S, 1);
    CHECK_STATUS(mfl_eval(S, uses_lib, strlen(uses_lib), name), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "alpha", "mm^2/s", &v), MFL_OK);
    CHECK(fabs(v - 63.9 / (7832.0 * 434.0) * 1e6) < 1e-12);
    CHECK(strstr(cap.text, "steel.mfl loaded") != NULL);
    /* names of imported files are results too */
    CHECK_STATUS(mfl_get_number(S, "k_steel", "W/(m*K)", &v), MFL_OK);
    CHECK(v == 63.9);

    /* errors in an imported file name that file */
    {
        const char *bad = "import \"lib/fails.mfl\"\nlet x = checked_add(1 m, 1 s)\n";
        CHECK_STATUS(mfl_eval(S, bad, strlen(bad), name), MFL_ERR_RUNTIME);
        CHECK(strstr(mfl_error_message(S), "lib/fails.mfl:1:33: runtime error: cannot add") != NULL);
    }
    {
        const char *missing = "import \"lib/none.mfl\"\n";
        CHECK_STATUS(mfl_eval(S, missing, strlen(missing), name), MFL_ERR_PROGRAM);
        CHECK(strstr(mfl_error_message(S), "cannot read the imported file") != NULL);
    }

    /* module imports are always available; degC results read back in K */
    mfl_set_file_imports(S, 0);
    CHECK_STATUS(mfl_eval(S, modules, strlen(modules), "modules.mfl"), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "m", "mm", &v), MFL_OK);
    CHECK(v == 2.0);
    CHECK_STATUS(mfl_get_number(S, "T", "K", &v), MFL_OK);
    CHECK(fabs(v - 293.15) < 1e-12);
    mfl_state_free(S);
}

/* ------------------------------------------------------------------------ */
/* v0.3: 1D heat conduction through the API                                  */
/* ------------------------------------------------------------------------ */

static void configure_bar(MflHeat1D *h, double length, const char *lunit, double T0,
                          const char *tunit)
{
    CHECK_STATUS(mfl_heat1d_set(h, "length", length, lunit), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "conductivity", 45, "W/(m*K)"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "density", 7850, "kg/m^3"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "specific_heat", 460, "J/(kg*K)"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_nodes(h, 11), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_initial_temperature(h, T0, tunit), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_boundary_temperature(h, MFL_SIDE_LEFT, 500, "degC"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_boundary_insulated(h, MFL_SIDE_RIGHT), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "time_step", 1, "s"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "end_time", 1, "min"), MFL_OK);
}

static void test_heat_api(void)
{
    MflState *S = mfl_state_new(NULL);
    MflHeat1D *a = mfl_heat1d_new(S);
    MflHeat1D *b = mfl_heat1d_new(S);
    double v = 0;
    int reached = -1;
    size_t j;

    CHECK(a != NULL && b != NULL);
    CHECK(mfl_heat1d_new(NULL) == NULL);

    /* Equivalent units give bit-identical results: 100 mm / 0.1 m and
     * 20 degC / 293.15 K. */
    configure_bar(a, 100, "mm", 20, "degC");
    configure_bar(b, 0.1, "m", 293.15, "K");
    CHECK_STATUS(mfl_heat1d_run(a), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(b), MFL_OK);
    CHECK(mfl_heat1d_sample_count(a) == 61 && mfl_heat1d_node_count(a) == 11);
    for (j = 0; j < 11; j++) {
        CHECK(mfl_heat1d_profile(a, 60)[j] == mfl_heat1d_profile(b, 60)[j]);
    }
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 5, "degC", &v), MFL_OK);
    CHECK(fabs(v - 20) < 1e-12);
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 0, "K", &v), MFL_OK);
    CHECK(v == 773.15); /* prescribed end replaces the initial value at t = 0 */
    CHECK_STATUS(mfl_heat1d_time(a, 60, "min", &v), MFL_OK);
    CHECK(v == 1);
    CHECK_STATUS(mfl_heat1d_position(a, 10, "mm", &v), MFL_OK);
    CHECK(v == 100);
    CHECK_STATUS(mfl_heat1d_max_time_step(a, "ms", &v), MFL_OK);
    CHECK(fabs(v - 0.01 * 0.01 / (2 * 45.0 / (7850 * 460.0)) * 1000) < 1e-9);
    CHECK_STATUS(mfl_heat1d_first_time_reaching(a, 1, 100, "degC", "s", &v, &reached), MFL_OK);
    CHECK(reached == 1 && v > 0 && v < 60);
    CHECK_STATUS(mfl_heat1d_first_time_reaching(a, 10, 1000, "K", "s", &v, &reached), MFL_OK);
    CHECK(reached == 0); /* threshold outside the simulated range */

    /* Invalid calls: status and a message naming the parameter. */
    CHECK_STATUS(mfl_heat1d_set(NULL, "length", 1, "m"), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set(a, "lenght", 1, "m"), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "unknown parameter 'lenght'") != NULL);
    CHECK_STATUS(mfl_heat1d_set(a, NULL, 1, "m"), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set(a, "conductivity", 45, "W/m"), MFL_ERR_UNIT);
    CHECK(strstr(mfl_error_message(S), "conductivity: unit 'W/m' has dimension") != NULL);
    CHECK_STATUS(mfl_heat1d_set(a, "length", 1, NULL), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set(a, "length", 1, "parsec"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set(a, "time_step", NAN, "s"), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "time_step (time step) must be finite") != NULL);
    CHECK_STATUS(mfl_heat1d_set(a, "density", -1, "kg/m^3"), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "density (density rho) must be positive, found -1 kg/m^3") != NULL);
    CHECK_STATUS(mfl_heat1d_set(a, "end_time", INFINITY, "s"), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_nodes(a, 2), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_sample_every(a, 0), MFL_ERR_ARGUMENT);
    /* a temperature difference where an absolute temperature is expected */
    CHECK_STATUS(mfl_heat1d_set_initial_temperature(a, 20, "delta_degC"), MFL_ERR_UNIT);
    CHECK(strstr(mfl_error_message(S), "'delta_degC' is a temperature difference") != NULL);
    CHECK_STATUS(mfl_heat1d_set_boundary_temperature(a, MFL_SIDE_LEFT, 5, "delta_degC"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set_initial_temperature(a, -300, "degC"), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "above 0 K, found -300 degC") != NULL);
    CHECK_STATUS(mfl_heat1d_set_initial_temperature(a, 20, "m"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set_boundary_insulated(a, (MflSide)7), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_initial_profile(a, NULL, 3, "K"), MFL_ERR_ARGUMENT);
    {
        double prof[3] = {300, NAN, 300};
        CHECK_STATUS(mfl_heat1d_set_initial_profile(a, prof, 3, "K"), MFL_ERR_ARGUMENT);
        CHECK(strstr(mfl_error_message(S), "value 2 of the initial profile (node 1) must be finite") != NULL);
    }
    CHECK_STATUS(mfl_heat1d_temperature(a, 61, 0, "K", &v), MFL_ERR_ARGUMENT);
    CHECK(strstr(mfl_error_message(S), "sample 61 does not exist (valid: 0 to 60)") != NULL);
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 11, "K", &v), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 0, "delta_degC", &v), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 0, "s", &v), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_temperature(a, 0, 0, "K", NULL), MFL_ERR_ARGUMENT);
    CHECK(mfl_heat1d_profile(a, 61) == NULL && mfl_heat1d_profile(NULL, 0) == NULL);
    /* none of the failures above changed the configuration or the results */
    CHECK(mfl_heat1d_profile(a, 60)[3] == mfl_heat1d_profile(b, 60)[3]);

    /* Solver errors reach the host; the same objects recover. */
    CHECK_STATUS(mfl_heat1d_set(a, "time_step", 5, "s"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(a), MFL_ERR_SIMULATION);
    CHECK(strstr(mfl_error_message(S), "unstable time step: time_step = 5 s") != NULL);
    CHECK(mfl_heat1d_sample_count(a) == 61); /* previous results kept */
    CHECK_STATUS(mfl_heat1d_set(a, "time_step", 2, "s"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(a), MFL_OK);
    CHECK(mfl_heat1d_sample_count(a) == 31);
    CHECK(strcmp(mfl_error_message(S), "") == 0);

    /* Missing parameters, results before running. */
    {
        MflHeat1D *c = mfl_heat1d_new(S);
        CHECK_STATUS(mfl_heat1d_run(c), MFL_ERR_SIMULATION);
        CHECK(strstr(mfl_error_message(S), "missing parameter 'length'") != NULL);
        CHECK(mfl_heat1d_sample_count(c) == 0 && mfl_heat1d_profile(c, 0) == NULL);
        CHECK_STATUS(mfl_heat1d_time(c, 0, "s", &v), MFL_ERR_NOT_RUN);
        CHECK_STATUS(mfl_heat1d_export(c, "x.csv", "x.json"), MFL_ERR_NOT_RUN);
        CHECK_STATUS(mfl_heat1d_max_time_step(c, "s", &v), MFL_ERR_SIMULATION);
        mfl_heat1d_free(c);
    }
    mfl_heat1d_free(a);
    mfl_heat1d_free(b);
    mfl_heat1d_free(NULL);

    /* The state stays usable for scripts afterwards. */
    CHECK_STATUS(mfl_eval(S, "let q = 2 m", 11, "after.mfl"), MFL_OK);
    mfl_state_free(S);
}

/* Simulations run by scripts are readable through the state. */
static void test_heat_from_script(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    const char *src = "import heat\n"
                      "heat.bar(0.1 m, 45 W/(m*K), 7850 kg/m^3, 460 J/(kg*K))\n"
                      "heat.nodes(11)\n"
                      "heat.initial_temperature(20 degC)\n"
                      "heat.left_temperature(500 degC)\n"
                      "heat.right_insulated()\n"
                      "heat.time_step(1 s)\n"
                      "heat.end_time(T_end)\n"
                      "heat.run()\n"
                      "let T_center = heat.final_profile()[5]\n";
    const char *unstable = "import heat\n"
                           "heat.bar(0.1 m, 45 W/(m*K), 7850 kg/m^3, 460 J/(kg*K))\n"
                           "heat.nodes(11)\n"
                           "heat.initial_temperature(20 degC)\n"
                           "heat.left_insulated()\nheat.right_insulated()\n"
                           "heat.time_step(10 s)\nheat.end_time(1 min)\nheat.run()\n";
    const MflHeat1D *h;
    double v = 0;
    double center = 0;
    MflHeat1D *c;

    CHECK(mfl_state_heat1d(S) == NULL);
    CHECK_STATUS(mfl_set_number(S, "T_end", 30, "s"), MFL_OK);
    CHECK_STATUS(mfl_eval(S, src, strlen(src), "bar.mfl"), MFL_OK);
    h = mfl_state_heat1d(S);
    CHECK(h != NULL && mfl_heat1d_sample_count(h) == 31);
    CHECK_STATUS(mfl_heat1d_temperature(h, 30, 5, "K", &v), MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "T_center", "K", &center), MFL_OK);
    CHECK(v == center); /* script value and API value agree */
    /* state-owned: cannot be reconfigured, freeing it is ignored */
    CHECK_STATUS(mfl_heat1d_set_nodes((MflHeat1D *)h, 5), MFL_ERR_ARGUMENT);
    mfl_heat1d_free((MflHeat1D *)h);
    CHECK(mfl_state_heat1d(S) == h);

    /* A solver error in a script is a run-time error at the call; the
     * previous results stay, and the next script runs normally. */
    CHECK_STATUS(mfl_eval(S, unstable, strlen(unstable), "unstable.mfl"), MFL_ERR_RUNTIME);
    CHECK(strstr(mfl_error_message(S), "unstable.mfl:9:1: runtime error: heat.run: unstable time step") != NULL);
    CHECK(mfl_state_heat1d(S) == h);
    CHECK_STATUS(mfl_set_number(S, "T_end", 60, "s"), MFL_OK);
    CHECK_STATUS(mfl_eval(S, src, strlen(src), "bar.mfl"), MFL_OK);
    CHECK(mfl_heat1d_sample_count(mfl_state_heat1d(S)) == 61);

    /* The same case from C gives identical numbers. */
    c = mfl_heat1d_new(S);
    configure_bar(c, 0.1, "m", 20, "degC");
    CHECK_STATUS(mfl_heat1d_run(c), MFL_OK);
    CHECK(mfl_heat1d_profile(c, 60)[5] == mfl_heat1d_profile(mfl_state_heat1d(S), 60)[5]);
    mfl_heat1d_free(c);

    /* A script without simulations clears it. */
    CHECK_STATUS(mfl_eval(S, "let z = 1", 9, "z.mfl"), MFL_OK);
    CHECK(mfl_state_heat1d(S) == NULL);
    mfl_state_free(S);
}

/* Memory limits: every limit fails cleanly or succeeds; afterwards the
 * state runs the case without a limit. */
static void test_heat_memory(void)
{
    const char *src = "import heat\nimport numerical\n"
                      "heat.bar(0.1 m, 45 W/(m*K), 7850 kg/m^3, 460 J/(kg*K))\n"
                      "heat.mesh(numerical.linspace(0 m, 0.1 m, 21))\n"
                      "heat.initial_temperature(300 K)\n"
                      "heat.left_temperature(400 K)\nheat.right_insulated()\n"
                      "heat.time_step(0.5 s)\nheat.end_time(100 s)\nheat.run()\n"
                      "let m = numerical.mean(heat.final_profile())\n";
    size_t limit;
    int ok_runs = 0;
    int mem_failures = 0;
    for (limit = 64; limit <= 64 * 1024; limit = limit * 5 / 4 + 1) {
        Capture cap;
        MflState *S = new_capturing(&cap, limit);
        MflHeat1D *h = mfl_heat1d_new(S);
        MflStatus st = mfl_eval(S, src, strlen(src), "mem.mfl");
        CHECK(st == MFL_OK || st == MFL_ERR_MEMORY);
        ok_runs += st == MFL_OK;
        mem_failures += st == MFL_ERR_MEMORY;
        if (h != NULL) {
            configure_bar(h, 0.1, "m", 300, "K");
            CHECK_STATUS(mfl_heat1d_set(h, "end_time", 1000, "s"), MFL_OK);
            st = mfl_heat1d_run(h);
            CHECK(st == MFL_OK || st == MFL_ERR_MEMORY);
            if (st == MFL_ERR_MEMORY) {
                CHECK(mfl_heat1d_sample_count(h) == 0);
                /* recover: fewer samples fit */
                CHECK_STATUS(mfl_heat1d_set_sample_every(h, 1000), MFL_OK);
                st = mfl_heat1d_run(h);
                CHECK(st == MFL_OK || st == MFL_ERR_MEMORY);
            }
            mfl_heat1d_free(h);
        }
        mfl_state_free(S);
    }
    CHECK(ok_runs > 0 && mem_failures > 0);
}

static void test_heat_export_api(void)
{
    MflState *S = mfl_state_new(NULL);
    MflHeat1D *h = mfl_heat1d_new(S);
    char csv[1024];
    char json[1024];
    FILE *f;
    configure_bar(h, 0.1, "m", 300, "K");
    CHECK_STATUS(mfl_heat1d_run(h), MFL_OK);
    snprintf(csv, sizeof csv, "%s", "api_export.csv");
    snprintf(json, sizeof json, "%s", "api_export.json");
    CHECK_STATUS(mfl_heat1d_export(h, csv, json), MFL_OK);
    f = fopen(json, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "\"rows\": 671") != NULL); /* 61 samples x 11 nodes */
        CHECK(strstr(buf, "\"export_complete\": true") != NULL);
    }
    remove(csv);
    remove(json);
    CHECK_STATUS(mfl_heat1d_export(h, "no/such/dir/x.csv", "no/such/dir/x.json"), MFL_ERR_IO);
    CHECK(strstr(mfl_error_message(S), "no metadata file was written") != NULL);
    CHECK_STATUS(mfl_heat1d_export(h, NULL, json), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_export(h, csv, csv), MFL_ERR_IO);
    CHECK(strcmp(mfl_status_name(MFL_ERR_SIMULATION), "MFL_ERR_SIMULATION") == 0);
    CHECK(strcmp(mfl_status_name(MFL_ERR_IO), "MFL_ERR_IO") == 0);
    mfl_heat1d_free(h);
    mfl_state_free(S);
}

/* v0.4: in-memory sources, compiled by name, importing each other. */
static void test_sources(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    const char *steel = "let k_s = 30 W/(m*K)\n";
    const char *model = "import \"materials/steel.mfl\"\nimport welding\n"
                        "let Q = welding.heat_input(voltage: U, current: I, speed: v, efficiency: 0.8)\n"
                        "let t85 = welding.t85_thick(heat_input: Q, conductivity: k_s, preheat: 20 degC)\n";
    MflScript *sc;
    double v = 0;

    CHECK_STATUS(mfl_add_source(S, "game/materials/steel.mfl", steel, strlen(steel)), MFL_OK);
    CHECK_STATUS(mfl_add_source(S, "game/./weld.mfl", model, strlen(model)), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "U", 25, "V"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "I", 250, "A"), MFL_OK);
    CHECK_STATUS(mfl_set_number(S, "v", 5, "mm/s"), MFL_OK);
    sc = mfl_script_compile_source(S, "game/weld.mfl");
    CHECK(sc != NULL);
    if (sc != NULL) {
        CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
        CHECK_STATUS(mfl_get_number(S, "Q", "kJ/mm", &v), MFL_OK);
        CHECK(fabs(v - 1.0) < 1e-12);
        CHECK_STATUS(mfl_get_number(S, "t85", "s", &v), MFL_OK);
        CHECK(fabs(v - 1e6 / (2 * 3.14159265358979323846 * 30) * (1.0 / 480 - 1.0 / 780)) < 1e-9);
        /* run many: change an input, run the same compiled script */
        CHECK_STATUS(mfl_set_number(S, "v", 10, "mm/s"), MFL_OK);
        CHECK_STATUS(mfl_script_run(S, sc), MFL_OK);
        CHECK_STATUS(mfl_get_number(S, "Q", "kJ/mm", &v), MFL_OK);
        CHECK(fabs(v - 0.5) < 1e-12);
        mfl_script_free(sc);
    }
    /* replacing a source changes what the next compilation sees */
    CHECK_STATUS(mfl_add_source(S, "game/materials/steel.mfl", "let k_s = 45 W/(m*K)\n", 21), MFL_OK);
    sc = mfl_script_compile_source(S, "game/weld.mfl");
    CHECK(sc != NULL && mfl_script_run(S, sc) == MFL_OK);
    CHECK_STATUS(mfl_get_number(S, "k_s", "W/(m*K)", &v), MFL_OK);
    CHECK(v == 45);
    mfl_script_free(sc);
    /* errors */
    CHECK(mfl_script_compile_source(S, "game/none.mfl") == NULL);
    CHECK(strstr(mfl_error_message(S), "no source 'game/none.mfl'") != NULL);
    CHECK_STATUS(mfl_add_source(S, "game/bad.mfl", "import \"missing.mfl\"\n", 22), MFL_OK);
    CHECK(mfl_script_compile_source(S, "game/bad.mfl") == NULL);
    CHECK(strstr(mfl_error_message(S), "game/bad.mfl:1:1: error: cannot read the imported file") != NULL);
    CHECK_STATUS(mfl_add_source(NULL, "a.mfl", "", 0), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_add_source(S, NULL, "", 0), MFL_ERR_ARGUMENT);
    mfl_clear_sources(S);
    CHECK(mfl_script_compile_source(S, "game/weld.mfl") == NULL);
    mfl_state_free(S);
}

static void test_cooling_convection(void)
{
    Capture cap;
    MflState *S = new_capturing(&cap, 0);
    MflHeat1D *h = mfl_heat1d_new(S);
    const MflHeat1D *script;
    char filename[2048], source[4096];
    size_t length, i, j;
    FILE *file;
    double value = 0;
    snprintf(filename, sizeof filename, "%s/examples/thermal/bar_cooling_convection.mfl", g_source_dir);
    file = fopen(filename, "rb");
    CHECK(file != NULL);
    if (file == NULL) {
        mfl_heat1d_free(h);
        mfl_state_free(S);
        return;
    }
    length = fread(source, 1, sizeof source, file);
    CHECK(!ferror(file) && length < sizeof source);
    fclose(file);
    CHECK_STATUS(mfl_eval(S, source, length, filename), MFL_OK);
    script = mfl_state_heat1d(S);
    CHECK(script != NULL);
    CHECK_STATUS(mfl_heat1d_set(h, "length", 100, "mm"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "conductivity", 40, "W/(m*K)"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "density", 7800, "kg/m^3"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "specific_heat", 500, "J/(kg*K)"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_nodes(h, 41), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_initial_temperature(h, 500, "degC"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, MFL_SIDE_LEFT, 100, "W/(m^2*K)", 20, "degC"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, MFL_SIDE_RIGHT, 100, "W/(m^2*K)", 293.15, "K"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_method(h, MFL_HEAT_BACKWARD_EULER), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "time_step", 10, "s"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set(h, "end_time", 10, "min"), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(h), MFL_OK);
    CHECK(mfl_heat1d_sample_count(h) == 61 && mfl_heat1d_node_count(h) == 41);
    if (script != NULL) {
        for (i = 0; i < 61; i++) {
            const double *a = mfl_heat1d_profile(h, i);
            const double *b = mfl_heat1d_profile(script, i);
            CHECK(a != NULL && b != NULL);
            if (a != NULL && b != NULL) {
                for (j = 0; j < 41; j++) CHECK(fabs(a[j] - b[j]) < 1e-10);
            }
        }
        CHECK_STATUS(mfl_heat1d_set_method((MflHeat1D *)script, MFL_HEAT_FTCS), MFL_ERR_ARGUMENT);
    }
    /* Independent plane-wall series, Bi = h*(L/2)/k = 0.125, t = 600 s. */
    CHECK_STATUS(mfl_heat1d_temperature(h, 60, 20, "degC", &value), MFL_OK);
    CHECK(fabs(value - 384.39700827945603) < 0.3);
    CHECK_STATUS(mfl_heat1d_temperature(h, 60, 0, "degC", &value), MFL_OK);
    CHECK(fabs(value - 362.75788546897456) < 0.3);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, MFL_SIDE_LEFT, -1, "W/(m^2*K)", 20, "degC"), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, MFL_SIDE_LEFT, 1, "W/m", 20, "degC"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, MFL_SIDE_LEFT, 1, "W/(m^2*K)", 20, "delta_degC"), MFL_ERR_UNIT);
    CHECK_STATUS(mfl_heat1d_set_boundary_convection(h, (MflSide)9, 1, "W/(m^2*K)", 20, "degC"), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_method(h, (MflHeatMethod)99), MFL_ERR_ARGUMENT);
    CHECK_STATUS(mfl_heat1d_set_method(h, MFL_HEAT_FTCS), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(h), MFL_ERR_SIMULATION);
    CHECK(mfl_heat1d_sample_count(h) == 61); /* failed run keeps the old result */
    CHECK_STATUS(mfl_heat1d_set_method(h, MFL_HEAT_BACKWARD_EULER), MFL_OK);
    CHECK_STATUS(mfl_heat1d_run(h), MFL_OK);
    CHECK_STATUS(mfl_heat1d_set_method(NULL, MFL_HEAT_FTCS), MFL_ERR_ARGUMENT);
    CHECK_STATUS(eval(S, "import heat\nheat.left_convection(1 W/m, 20 degC)\n"), MFL_ERR_PROGRAM);
    CHECK_STATUS(eval(S, "import heat\nheat.left_convection(1 W/(m^2*K), 20 delta_degC)\n"), MFL_ERR_PROGRAM);
    CHECK_STATUS(eval(S, "import heat\nheat.left_convection(-1 W/(m^2*K), 20 degC)\n"), MFL_ERR_RUNTIME);
    CHECK_STATUS(eval(S, "import heat\nheat.method(\"typo\")\n"), MFL_ERR_RUNTIME);
    CHECK_STATUS(mfl_eval(S, source, length, filename), MFL_OK);
    mfl_heat1d_free(h);
    mfl_state_free(S);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        g_source_dir = argv[1];
    }
    test_heat_input();
    test_inputs();
    test_bad_arguments();
    test_units_in_api();
    test_results();
    test_program_errors();
    test_compile_once_run_many();
    test_independent_states();
    test_memory_limits();
    test_runtime_errors();
    test_v02_programs();
    test_status_names();
    test_imports();
    test_heat_api();
    test_heat_from_script();
    test_heat_memory();
    test_heat_export_api();
    test_sources();
    test_cooling_convection();
    printf("test_api: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
