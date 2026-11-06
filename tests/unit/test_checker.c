/*
 * test_checker.c - unit tests for src/runtime/units and src/compiler/checker.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/checker.h"
#include "parser/parser.h"
#include "runtime/units.h"

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

static int close_to(double a, double b) { return fabs(a - b) <= 1e-14 * fabs(b); }

static MflDim dim7(int m, int kg, int s, int A, int K, int mol, int cd)
{
    MflDim d;
    d.exp[0] = (signed char)m;
    d.exp[1] = (signed char)kg;
    d.exp[2] = (signed char)s;
    d.exp[3] = (signed char)A;
    d.exp[4] = (signed char)K;
    d.exp[5] = (signed char)mol;
    d.exp[6] = (signed char)cd;
    return d;
}

/* ------------------------------------------------------------------------ */
/* Unit registry                                                             */
/* ------------------------------------------------------------------------ */

static void expect_unit(const char *name, double scale, MflDim dim, int line)
{
    MflScale exact = mfl_scale_one();
    double sc;
    MflDim d = mfl_dim_none();
    MflUnitStatus st = mfl_unit_lookup(name, strlen(name), &exact, &d, NULL);
    sc = mfl_scale_to_double(exact);
    g_checks++;
    if (st != MFL_UNIT_OK || !close_to(sc, scale) || !mfl_dim_equal(d, dim)) {
        char ds[96];
        mfl_dim_format(d, ds, sizeof ds);
        g_failures++;
        fprintf(stderr, "%s:%d: unit '%s': status %d, scale %.17g (want %.17g), dim %s\n",
                __FILE__, line, name, (int)st, sc, scale, ds);
    }
}

#define EXPECT_UNIT(name, scale, dim) expect_unit(name, scale, dim, __LINE__)

static void test_unit_registry(void)
{
    MflDim length = dim7(1, 0, 0, 0, 0, 0, 0);
    MflDim mass = dim7(0, 1, 0, 0, 0, 0, 0);
    MflDim time = dim7(0, 0, 1, 0, 0, 0, 0);
    MflDim pressure = dim7(-1, 1, -2, 0, 0, 0, 0);
    MflScale sc;
    MflDim d;

    /* Base units. */
    EXPECT_UNIT("m", 1.0, length);
    EXPECT_UNIT("kg", 1.0, mass);
    EXPECT_UNIT("g", 1e-3, mass);
    EXPECT_UNIT("s", 1.0, time);
    EXPECT_UNIT("A", 1.0, dim7(0, 0, 0, 1, 0, 0, 0));
    EXPECT_UNIT("K", 1.0, dim7(0, 0, 0, 0, 1, 0, 0));
    EXPECT_UNIT("mol", 1.0, dim7(0, 0, 0, 0, 0, 1, 0));
    EXPECT_UNIT("cd", 1.0, dim7(0, 0, 0, 0, 0, 0, 1));
    /* Derived units reduce to the right base dimensions. */
    EXPECT_UNIT("N", 1.0, dim7(1, 1, -2, 0, 0, 0, 0));
    EXPECT_UNIT("Pa", 1.0, pressure);
    EXPECT_UNIT("J", 1.0, dim7(2, 1, -2, 0, 0, 0, 0));
    EXPECT_UNIT("W", 1.0, dim7(2, 1, -3, 0, 0, 0, 0));
    EXPECT_UNIT("C", 1.0, dim7(0, 0, 1, 1, 0, 0, 0));
    EXPECT_UNIT("V", 1.0, dim7(2, 1, -3, -1, 0, 0, 0));
    EXPECT_UNIT("ohm", 1.0, dim7(2, 1, -3, -2, 0, 0, 0));
    EXPECT_UNIT("Hz", 1.0, dim7(0, 0, -1, 0, 0, 0, 0));
    /* Non-SI. */
    EXPECT_UNIT("min", 60.0, time);
    EXPECT_UNIT("h", 3600.0, time);
    EXPECT_UNIT("L", 1e-3, dim7(3, 0, 0, 0, 0, 0, 0));
    EXPECT_UNIT("bar", 1e5, pressure);
    /* Prefixes. */
    EXPECT_UNIT("pm", 1e-12, length);
    EXPECT_UNIT("nm", 1e-9, length);
    EXPECT_UNIT("um", 1e-6, length);
    EXPECT_UNIT("mm", 1e-3, length);
    EXPECT_UNIT("cm", 1e-2, length);
    EXPECT_UNIT("dm", 1e-1, length);
    EXPECT_UNIT("km", 1e3, length);
    EXPECT_UNIT("Mm", 1e6, length);
    EXPECT_UNIT("Gm", 1e9, length);
    EXPECT_UNIT("mg", 1e-6, mass);
    EXPECT_UNIT("ug", 1e-9, mass);
    EXPECT_UNIT("ms", 1e-3, time);
    EXPECT_UNIT("kJ", 1e3, dim7(2, 1, -2, 0, 0, 0, 0));
    EXPECT_UNIT("MPa", 1e6, pressure);
    EXPECT_UNIT("GPa", 1e9, pressure);
    EXPECT_UNIT("kW", 1e3, dim7(2, 1, -3, 0, 0, 0, 0));
    EXPECT_UNIT("mA", 1e-3, dim7(0, 0, 0, 1, 0, 0, 0));
    EXPECT_UNIT("kohm", 1e3, dim7(2, 1, -3, -2, 0, 0, 0));
    EXPECT_UNIT("mL", 1e-6, dim7(3, 0, 0, 0, 0, 0, 0));
    EXPECT_UNIT("mK", 1e-3, dim7(0, 0, 0, 0, 1, 0, 0));
    EXPECT_UNIT("kmol", 1e3, dim7(0, 0, 0, 0, 0, 1, 0));
    /* Exact symbols win over prefixed readings; case matters. */
    EXPECT_UNIT("min", 60.0, time);                          /* not milli-"in" */
    EXPECT_UNIT("cd", 1.0, dim7(0, 0, 0, 0, 0, 0, 1));       /* not centi-"d" */
    EXPECT_UNIT("Pa", 1.0, pressure);                        /* pascal */
    EXPECT_UNIT("pA", 1e-12, dim7(0, 0, 0, 1, 0, 0, 0));     /* picoampere */
    EXPECT_UNIT("mm", 1e-3, length);                         /* milli-metre */

    /* Not units / not prefixable. */
    CHECK(mfl_unit_lookup("x", 1, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);
    CHECK(mfl_unit_lookup("speed", 5, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);
    CHECK(mfl_unit_lookup("mkg", 3, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);   /* prefix on kg */
    CHECK(mfl_unit_lookup("kmin", 4, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);  /* prefix on min */
    CHECK(mfl_unit_lookup("kh", 2, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);
    CHECK(mfl_unit_lookup("mbar", 4, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);
    CHECK(mfl_unit_lookup("mmm", 3, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);   /* one prefix only */
    CHECK(mfl_unit_lookup("M", 1, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);     /* prefix alone */
    CHECK(mfl_unit_lookup("v", 1, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);     /* case-sensitive */
    {
        /* v0.2 temperature units (docs/units.md section 5) */
        int affine = -1;
        CHECK(mfl_unit_lookup("degC", 4, &sc, &d, &affine) == MFL_UNIT_OK && affine == 1 &&
              mfl_dim_equal(d, dim7(0, 0, 0, 0, 1, 0, 0)) && mfl_scale_to_double(sc) == 1.0);
        CHECK(mfl_unit_lookup("delta_degC", 10, &sc, &d, &affine) == MFL_UNIT_OK && affine == 0 &&
              mfl_dim_equal(d, dim7(0, 0, 0, 0, 1, 0, 0)));
        CHECK(mfl_unit_lookup("percent", 7, &sc, &d, &affine) == MFL_UNIT_OK && affine == 0 &&
              mfl_dim_is_none(d) && mfl_scale_to_double(sc) == 0.01);
        CHECK(mfl_unit_lookup("mdegC", 5, &sc, &d, NULL) == MFL_UNIT_UNKNOWN); /* no prefix */
        CHECK(mfl_unit_lookup("degF", 4, &sc, &d, NULL) == MFL_UNIT_UNSUPPORTED);
    }
    CHECK(mfl_unit_lookup("", 0, &sc, &d, NULL) == MFL_UNIT_UNKNOWN);
}

static void expect_format(MflDim d, const char *want, int line)
{
    char buf[96];
    mfl_dim_format(d, buf, sizeof buf);
    g_checks++;
    if (strcmp(buf, want) != 0) {
        g_failures++;
        fprintf(stderr, "%s:%d: format: got '%s', want '%s'\n", __FILE__, line, buf, want);
    }
}

#define EXPECT_FORMAT(d, want) expect_format(d, want, __LINE__)

static void test_dimensions(void)
{
    MflDim d;
    MflDim big = dim7(100, 0, 0, 0, 0, 0, 0);

    EXPECT_FORMAT(mfl_dim_none(), "1");
    EXPECT_FORMAT(dim7(1, 0, 0, 0, 0, 0, 0), "m");
    EXPECT_FORMAT(dim7(1, 0, -1, 0, 0, 0, 0), "m/s");
    EXPECT_FORMAT(dim7(0, 0, -1, 0, 0, 0, 0), "1/s");
    EXPECT_FORMAT(dim7(2, 1, -3, -1, 0, 0, 0), "kg*m^2/(s^3*A)");
    EXPECT_FORMAT(dim7(-3, 1, 0, 0, 0, 0, 0), "kg/m^3");
    EXPECT_FORMAT(dim7(1, 1, -3, 0, -1, 0, 0), "kg*m/(s^3*K)");
    EXPECT_FORMAT(dim7(1, 2, 3, 4, 5, 6, 7), "kg^2*m*s^3*A^4*K^5*mol^6*cd^7");

    CHECK(mfl_dim_mul(dim7(1, 0, 0, 0, 0, 0, 0), dim7(0, 0, -1, 0, 0, 0, 0), &d) &&
          mfl_dim_equal(d, dim7(1, 0, -1, 0, 0, 0, 0)));
    CHECK(mfl_dim_div(dim7(1, 0, 0, 0, 0, 0, 0), dim7(1, 0, 0, 0, 0, 0, 0), &d) &&
          mfl_dim_is_none(d));
    CHECK(mfl_dim_pow(dim7(1, 0, -1, 0, 0, 0, 0), -2, &d) &&
          mfl_dim_equal(d, dim7(-2, 0, 2, 0, 0, 0, 0)));
    CHECK(mfl_dim_pow(mfl_dim_none(), 1000000, &d) && mfl_dim_is_none(d));
    /* Overflow is detected, never wrapped. */
    CHECK(!mfl_dim_mul(big, big, &d));
    CHECK(!mfl_dim_pow(dim7(1, 0, 0, 0, 0, 0, 0), 128, &d));
    CHECK(!mfl_dim_pow(dim7(2, 0, 0, 0, 0, 0, 0), 64, &d));
    CHECK(mfl_dim_pow(dim7(1, 0, 0, 0, 0, 0, 0), 127, &d) && d.exp[0] == 127);
}

/* ------------------------------------------------------------------------ */
/* Checker                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
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

static Buf g_out;
static Buf g_diags;

/* Parses and checks; collects "line:col: message\n" and the symbol dump. */
static int run(const char *src)
{
    MflParseResult pr;
    MflCheckResult cr;
    const MflDiagnostic *d;
    char line[600];
    int ok;

    g_out.len = 0;
    g_diags.len = 0;
    buf_write(&g_out, "", 0);
    buf_write(&g_diags, "", 0);
    if (!mfl_parse(src, strlen(src), 0, &pr)) {
        buf_write(&g_diags, "<parse error>\n", 14);
        mfl_parse_free(&pr);
        return 0;
    }
    ok = mfl_check(&pr.program, 0, &cr);
    for (d = cr.diagnostics; d != NULL; d = d->next) {
        snprintf(line, sizeof line, "%d:%d: %s\n", d->line, d->column, d->message);
        buf_write(&g_diags, line, strlen(line));
    }
    mfl_check_print_symbols(&cr, buf_write, &g_out);
    mfl_check_free(&cr);
    mfl_parse_free(&pr);
    return ok;
}

#define EXPECT_TYPES(src, expected)                                               \
    do {                                                                          \
        int ok_ = run(src);                                                       \
        g_checks++;                                                               \
        if (!ok_ || strcmp(g_out.data, expected) != 0) {                          \
            g_failures++;                                                         \
            fprintf(stderr, "%s:%d: source:\n%s\n--- expected:\n%s--- got:\n%s%s",    \
                    __FILE__, __LINE__, src, expected, g_out.data, g_diags.data); \
        }                                                                         \
    } while (0)

#define EXPECT_ERRORS(src, expected)                                              \
    do {                                                                          \
        int ok_ = run(src);                                                       \
        g_checks++;                                                               \
        if (ok_ || strcmp(g_diags.data, expected) != 0) {                         \
            g_failures++;                                                         \
            fprintf(stderr, "%s:%d: source:\n%s\n--- expected:\n%s--- got:\n%s",      \
                    __FILE__, __LINE__, src, expected, g_diags.data);             \
        }                                                                         \
    } while (0)

static void test_heat_input(void)
{
    EXPECT_TYPES("let voltage = 20 V\n"
                 "let current = 150 A\n"
                 "let speed = 5 mm/s\n"
                 "let efficiency = 0.8\n"
                 "let energy = efficiency * voltage * current / speed\n"
                 "print(energy.to(J/mm))\n",
                 "1:1 let voltage: Quantity[kg*m^2/(s^3*A)]\n"
                 "2:1 let current: Quantity[A]\n"
                 "3:1 let speed: Quantity[m/s]\n"
                 "4:1 let efficiency: Number\n"
                 "5:1 let energy: Quantity[kg*m/s^2]\n");
    /* J/mm and kJ/mm have the dimension of energy (heat input) per length. */
    EXPECT_TYPES("let q = 1.2 kJ/mm\nlet r = q.to(J/mm)\nlet n = q.value_in(J/m)\n",
                 "1:1 let q: Quantity[kg*m/s^2]\n2:1 let r: Quantity[kg*m/s^2]\n"
                 "3:1 let n: Number\n");
}

static void test_dimensional_algebra(void)
{
    EXPECT_TYPES("let k = 45 W/(m*K)\n"
                 "let rho = 7850 kg/m^3\n"
                 "let cp = 460 J/(kg*K)\n"
                 "let alpha = k / (rho * cp)\n",
                 "1:1 let k: Quantity[kg*m/(s^3*K)]\n"
                 "2:1 let rho: Quantity[kg/m^3]\n"
                 "3:1 let cp: Quantity[m^2/(s^2*K)]\n"
                 "4:1 let alpha: Quantity[m^2/s]\n");
    EXPECT_TYPES("let a = (2 m)^2\nlet b = (2 m)^-1\nlet c = 2^0.5\nlet d = (3 s)^0\n",
                 "1:1 let a: Quantity[m^2]\n2:1 let b: Quantity[1/m]\n"
                 "3:1 let c: Number\n4:1 let d: Number\n");
    EXPECT_TYPES("let x = 5 m / (2 s)\n", "1:1 let x: Quantity[m/s]\n");
    EXPECT_TYPES("let e = (1 m) / (1 m)\n", "1:1 let e: Number\n");
    EXPECT_TYPES("let p = 1 bar + 2 kPa - 3 MPa\n", "1:1 let p: Quantity[kg/(m*s^2)]\n");
    EXPECT_TYPES("let f = 15 L/min\n", "1:1 let f: Quantity[m^3/s]\n");
    EXPECT_TYPES("let r = 7 % 3\nlet t = (7 s) % (2 s)\n",
                 "1:1 let r: Number\n2:1 let t: Quantity[s]\n");
    EXPECT_TYPES("let n = -(3 V)\n", "1:1 let n: Quantity[kg*m^2/(s^3*A)]\n");
}

static void test_bools_strings_nil(void)
{
    EXPECT_TYPES("let a = 1 m < 2 m and not (3 s >= 4 s) or true\n", "1:1 let a: Bool\n");
    EXPECT_TYPES("let b = \"x\" == \"y\"\nlet c = nil\nlet d = c == 1\nlet e = print(1)\n",
                 "1:1 let b: Bool\n2:1 let c: Nil\n3:1 let d: Bool\n4:1 let e: Nil\n");
    EXPECT_TYPES("let s = \"text\"\n", "1:1 let s: String\n");
}

static void test_variables(void)
{
    EXPECT_TYPES("var t = 0 s\nt = t + 0.1 s\nt = 5 min\n", "1:1 var t: Quantity[s]\n");
    EXPECT_ERRORS("print(y)\n", "1:7: undefined variable 'y'\n");
    EXPECT_ERRORS("let x = x\n", "1:9: undefined variable 'x'\n");
    EXPECT_ERRORS("let a = 1\nlet a = 2\n", "2:1: 'a' is already declared at 1:1\n");
    EXPECT_ERRORS("var a = 1\nlet a = 2\n", "2:1: 'a' is already declared at 1:1\n");
    EXPECT_ERRORS("let a = 1\na = 2\n",
                  "2:1: cannot assign to 'a': it is declared with 'let' at 1:1 (use 'var' "
                  "for a variable that changes)\n");
    EXPECT_ERRORS("b = 2\n", "1:1: undefined variable 'b' (declare it first with 'var')\n");
    EXPECT_ERRORS("var T = 300 K\nT = 5 s\n",
                  "2:1: cannot assign Quantity[s] to 'T', which is a Quantity[K] (declared at "
                  "1:1)\n");
    EXPECT_ERRORS("var n = 1\nn = \"a\"\n",
                  "2:1: cannot assign String to 'n', which is a Number (declared at 1:1)\n");
    EXPECT_ERRORS("let print = 3\n",
                  "1:1: 'print' is a built-in function and cannot be redefined\n");
    EXPECT_ERRORS("print = 3\n", "1:1: 'print' is a built-in function and cannot be assigned\n");
    EXPECT_ERRORS("let p = print\n",
                  "1:9: 'print' is a built-in function; call it, e.g. print(x)\n");
}

static void test_dimension_errors(void)
{
    EXPECT_ERRORS("let x = 20 V + 150 A\n",
                  "1:14: cannot add Quantity[kg*m^2/(s^3*A)] and Quantity[A]: dimensions "
                  "differ\n");
    EXPECT_ERRORS("let x = 1 m - 1 s\n",
                  "1:13: cannot subtract Quantity[m] and Quantity[s]: dimensions differ\n");
    EXPECT_ERRORS("let x = 1 + 1 m\n",
                  "1:11: cannot add Number and Quantity[m]: dimensions differ\n");
    EXPECT_ERRORS("let x = (1 m) % (1 s)\n",
                  "1:15: cannot take the remainder of Quantity[m] and Quantity[s]: dimensions "
                  "differ\n");
    EXPECT_ERRORS("let x = 1 m < 1 s\n",
                  "1:13: cannot compare Quantity[m] and Quantity[s]: dimensions differ\n");
    EXPECT_ERRORS("let x = 1 m == 1 s\n",
                  "1:13: cannot compare Quantity[m] and Quantity[s]: dimensions differ\n");
    EXPECT_ERRORS("let v = 5 mm/s\nprint(v.to(J/mm))\n",
                  "2:13: cannot convert Quantity[m/s] to a unit of dimension kg*m/s^2: "
                  "dimensions differ\n");
    EXPECT_ERRORS("let n = 1\nprint(n.to(m))\n",
                  "2:12: cannot convert Number to a unit of dimension m: dimensions differ\n");
    /* One mistake, one message: errors do not cascade. */
    EXPECT_ERRORS("let x = (1 m + 1 s) * 2 + 3 V\nlet y = x + 1\n",
                  "1:14: cannot add Quantity[m] and Quantity[s]: dimensions differ\n");
}

static void test_power_errors(void)
{
    EXPECT_ERRORS("let x = 2 ^ (1 m)\n", "1:14: exponent must be dimensionless, found Quantity[m]\n");
    EXPECT_ERRORS("let n = 2\nlet x = (1 m) ^ n\n",
                  "2:17: the exponent of a Quantity[m] must be an integer literal, so that its "
                  "dimension is known before running\n");
    EXPECT_ERRORS("let x = (1 m) ^ 0.5\n",
                  "1:17: the exponent of a Quantity[m] must be an integer literal, so that its "
                  "dimension is known before running\n");
    EXPECT_ERRORS("let x = (1 m) ^ 200\n", "1:15: dimension exponent out of range\n");
    EXPECT_ERRORS("let x = 1 m^200\n", "1:12: unit exponent out of range\n");
}

static void test_type_errors(void)
{
    EXPECT_ERRORS("let b = not 1\n", "1:9: 'not' needs a Bool, found Number\n");
    EXPECT_ERRORS("let b = -true\n", "1:9: operator '-' needs a number, found Bool\n");
    EXPECT_ERRORS("let s = \"a\" + 1\n",
                  "1:13: operator '+' needs numbers, found String and Number\n");
    EXPECT_ERRORS("let s = \"a\" * \"b\"\n",
                  "1:13: operator '*' needs numbers, found String and String\n");
    EXPECT_ERRORS("let b = 1 and true\n", "1:11: 'and' needs Bool operands, found Number and Bool\n");
    EXPECT_ERRORS("let b = \"a\" < \"b\"\n",
                  "1:13: operator '<' needs numbers, found String and String\n");
    EXPECT_ERRORS("let n = 1 == \"a\"\n", "1:11: cannot compare Number and String\n");
}

static void test_unit_errors(void)
{
    EXPECT_ERRORS("let x = 5 furlong\n", "1:11: unknown unit 'furlong'\n");
    EXPECT_ERRORS("let t = 2 s\nlet d = 5 mm / t\n",
                  "2:16: unknown unit 't' ('t' is a variable; to combine a quantity with it, "
                  "put the quantity in parentheses, e.g. (5 mm) / t)\n");
    EXPECT_ERRORS("let x = 25 degF\n",
                  "1:12: unit 'degF' is not supported (Fahrenheit is not part of MFL); use K or degC\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to(5))\n",
                  "2:12: expected a unit (such as J/mm), found a number\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to(\"mm\"))\n",
                  "2:12: expected a unit (such as J/mm), found a string\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to(mm + cm))\n",
                  "2:15: expected a unit (such as J/mm), found an expression\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to(mm, cm))\n",
                  "2:7: '.to' takes exactly one unit, e.g. x.to(J/mm); found 2 arguments\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to())\n",
                  "2:7: '.to' takes exactly one unit, e.g. x.to(J/mm); found 0 arguments\n");
    EXPECT_ERRORS("let x = 1 m\nprint(x.to(mm^x))\n",
                  "2:15: unit exponent must be a small integer\n");
    EXPECT_ERRORS("let x = \"a\".to(m)\n",
                  "1:9: '.to' needs a number or quantity, found String\n");
    EXPECT_ERRORS("let x = 1 m\nlet y = x.to\n",
                  "2:9: '.to' must be called with a unit, e.g. x.to(J/mm)\n");
    EXPECT_ERRORS("let x = 1 m\nlet y = x.foo\n",
                  "2:9: unknown property 'foo' on Quantity[m] (quantities have: to, "
                  "value_in)\n");
}

static void test_call_errors(void)
{
    EXPECT_ERRORS("f(1)\n", "1:1: undefined function 'f'\n");
    EXPECT_ERRORS("f(y)\n",
                  "1:1: undefined function 'f'\n"
                  "1:3: undefined variable 'y'\n"); /* sorted by position */
    EXPECT_ERRORS("let U = 1 V\nU(2)\n",
                  "2:1: cannot call 'U': it is a Quantity[kg*m^2/(s^3*A)], not a function\n");
    EXPECT_ERRORS("let x = (1)(2)\n", "1:10: cannot call a value of type Number\n");
    EXPECT_ERRORS("let v = 1\nlet w = v[0]\n",
                  "2:9: cannot index a Number (only arrays can be indexed)\n");
    EXPECT_ERRORS("print(1 m + 1 s, y)\n",
                  "1:11: cannot add Quantity[m] and Quantity[s]: dimensions differ\n"
                  "1:18: undefined variable 'y'\n");
}

/* The checker records SI scale factors for the compiler. */
static void test_unit_scales(void)
{
    static const struct {
        const char *src;
        double      scale;
    } cases[] = {
        {"let q = 5 mm/s", 1e-3},
        {"let q = 1 kJ/mm", 1e6},
        {"let q = 1 L/min", 1e-3 / 60.0},
        {"let q = 1 mm^2", 1e-6},
        {"let q = 1 cm^-3", 1e6},
        {"let q = 1 g/cm^3", 1e3},
        {"let q = 1 km/h", 1000.0 / 3600.0},
        {"let q = 1 W/(m*K)", 1.0},
        {"let q = 1 MPa", 1e6},
        {"let q = 1 bar", 1e5},
        {"let q = 1 ug", 1e-9},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        MflParseResult pr;
        MflCheckResult cr;
        const MflExpr *q;
        int ok = mfl_parse(cases[i].src, strlen(cases[i].src), 0, &pr);
        CHECK(ok);
        if (!ok) {
            mfl_parse_free(&pr);
            continue;
        }
        CHECK(mfl_check(&pr.program, 0, &cr));
        q = pr.program.first->value;
        CHECK(q->kind == MFL_EXPR_QUANTITY);
        CHECK(q->as.quantity.unit->type.kind == MFL_TYPE_UNIT);
        if (!close_to(mfl_scale_to_double(q->as.quantity.unit->unit_scale), cases[i].scale)) {
            g_failures++;
            fprintf(stderr, "%s: scale %.17g, want %.17g\n", cases[i].src,
                    mfl_scale_to_double(q->as.quantity.unit->unit_scale), cases[i].scale);
        }
        g_checks++;
        mfl_check_free(&cr);
        mfl_parse_free(&pr);
    }

    /* The target of .to() gets its scale too: J/mm = 1000 J/m. */
    {
        const char *src = "let e = 1 J/m\nprint(e.to(J/mm))";
        MflParseResult pr;
        MflCheckResult cr;
        const MflExpr *call;
        CHECK(mfl_parse(src, strlen(src), 0, &pr));
        CHECK(mfl_check(&pr.program, 0, &cr));
        call = pr.program.first->next->value->as.call.args; /* e.to(J/mm) */
        CHECK(call->kind == MFL_EXPR_CALL);
        CHECK(close_to(mfl_scale_to_double(call->as.call.args->unit_scale), 1e3));
        CHECK(call->as.call.args->unit_scale.exp10 == 3); /* exact: 10^3 */
        CHECK(call->type.kind == MFL_TYPE_NUMBER);
        mfl_check_free(&cr);
        mfl_parse_free(&pr);
    }
}

static void test_limits(void)
{
    MflParseResult pr;
    MflCheckResult cr;
    char *src;
    size_t i;
    size_t limit;
    int saw_oom = 0;
    int saw_ok = 0;
    const char *line = "print(x)\n";
    size_t n = 200;

    /* Diagnostics are capped. */
    src = (char *)malloc(strlen(line) * n + 1);
    if (src == NULL) {
        exit(2);
    }
    for (i = 0; i < n; i++) {
        memcpy(src + i * strlen(line), line, strlen(line));
    }
    src[n * strlen(line)] = '\0';
    CHECK(mfl_parse(src, strlen(src), 0, &pr));
    CHECK(!mfl_check(&pr.program, 0, &cr));
    CHECK(cr.diagnostic_count == MFL_CHECK_MAX_DIAGNOSTICS);
    mfl_check_free(&cr);
    mfl_parse_free(&pr);
    free(src);

    /* Every memory limit fails cleanly or succeeds. */
    {
        const char *prog = "let a = 1 m\nvar b = a + 2 m\nb = 3 s\nprint(c)\nlet d = a.to(mm)\n";
        CHECK(mfl_parse(prog, strlen(prog), 0, &pr));
        for (limit = 1; limit < 4000; limit += 3) {
            mfl_check(&pr.program, limit, &cr);
            if (cr.out_of_memory) {
                saw_oom = 1;
            } else {
                saw_ok = 1;
                CHECK(cr.diagnostic_count == 2);
            }
            mfl_check_free(&cr);
        }
        mfl_parse_free(&pr);
        CHECK(saw_oom && saw_ok);
    }
}

int main(void)
{
    test_unit_registry();
    test_dimensions();
    test_heat_input();
    test_dimensional_algebra();
    test_bools_strings_nil();
    test_variables();
    test_dimension_errors();
    test_power_errors();
    test_type_errors();
    test_unit_errors();
    test_call_errors();
    test_unit_scales();
    test_limits();

    free(g_out.data);
    free(g_diags.data);
    printf("test_checker: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
