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
    printf("test_api: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
