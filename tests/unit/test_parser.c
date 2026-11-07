/*
 * test_parser.c - unit tests for src/parser.
 *
 * Programs are parsed and compared through the S-expression printer, so
 * each expectation reads like the tree it describes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"

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

/* Growable output buffer for the printer. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_write(void *ctx, const char *s, size_t n)
{
    Buf *b = (Buf *)ctx;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *p;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        p = (char *)realloc(b->data, cap);
        if (p == NULL) {
            fprintf(stderr, "test_parser: out of memory\n");
            exit(2);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_reset(Buf *b)
{
    b->len = 0;
    if (b->data) {
        b->data[0] = '\0';
    }
}

/* Parses src; returns printed AST and "line:col: message\n" diagnostics. */
static int parse_str(const char *src, Buf *ast, Buf *diags)
{
    MflParseResult r;
    const MflDiagnostic *d;
    int ok = mfl_parse(src, strlen(src), 0, &r);
    char line[512];

    buf_reset(ast);
    buf_reset(diags);
    buf_write(ast, "", 0);
    buf_write(diags, "", 0);
    mfl_ast_print_program(&r.program, buf_write, ast);
    for (d = r.diagnostics; d != NULL; d = d->next) {
        snprintf(line, sizeof line, "%d:%d: %s\n", d->line, d->column, d->message);
        buf_write(diags, line, strlen(line));
    }
    mfl_parse_free(&r);
    return ok;
}

static Buf g_ast, g_diags;

/* The program must parse without errors and print exactly `expected`. */
#define EXPECT_AST(src, expected)                                              \
    do {                                                                       \
        int ok_ = parse_str(src, &g_ast, &g_diags);                            \
        g_checks++;                                                            \
        if (!ok_ || strcmp(g_ast.data, expected) != 0) {                       \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: source:\n%s\n--- expected:\n%s--- got:\n%s%s", \
                    __FILE__, __LINE__, src, expected, g_ast.data, g_diags.data); \
        }                                                                      \
    } while (0)

/* The program must fail with exactly these diagnostics. */
#define EXPECT_DIAGS(src, expected)                                            \
    do {                                                                       \
        int ok_ = parse_str(src, &g_ast, &g_diags);                            \
        g_checks++;                                                            \
        if (ok_ || strcmp(g_diags.data, expected) != 0) {                      \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: source:\n%s\n--- expected:\n%s--- got:\n%s", \
                    __FILE__, __LINE__, src, expected, g_diags.data);          \
        }                                                                      \
    } while (0)

/* Single-expression shortcut: "let r = <expr>" -> "1:1 (let r <tree>)". */
#define EXPECT_EXPR(expr, tree) EXPECT_AST("let r = " expr, "1:1 (let r " tree ")\n")

/* ------------------------------------------------------------------------ */

static void test_heat_input(void)
{
    EXPECT_AST("# welding heat input\n"
               "let voltage = 20 V\n"
               "let current = 150 A\n"
               "let speed = 5 mm/s\n"
               "let efficiency = 0.8\n"
               "\n"
               "let energy = efficiency * voltage * current / speed\n"
               "\n"
               "print(energy.to(J/mm))\n",
               "2:1 (let voltage (quantity 20 V))\n"
               "3:1 (let current (quantity 150 A))\n"
               "4:1 (let speed (quantity 5 (/ mm s)))\n"
               "5:1 (let efficiency 0.8)\n"
               "7:1 (let energy (/ (* (* efficiency voltage) current) speed))\n"
               "9:1 (expr (call print (call (. energy to) (/ J mm))))\n");
}

static void test_literals(void)
{
    EXPECT_EXPR("1.5e-3", "1.5e-3");
    EXPECT_EXPR("\"text\"", "\"text\"");
    EXPECT_EXPR("true", "true");
    EXPECT_EXPR("false", "false");
    EXPECT_EXPR("nil", "nil");
    EXPECT_EXPR("x", "x");
    EXPECT_EXPR("(((x)))", "x");
}

static void test_precedence(void)
{
    EXPECT_EXPR("1 + 2 * 3", "(+ 1 (* 2 3))");
    EXPECT_EXPR("(1 + 2) * 3", "(* (+ 1 2) 3)");
    EXPECT_EXPR("1 - 2 - 3", "(- (- 1 2) 3)");          /* left-assoc */
    EXPECT_EXPR("8 / 4 / 2", "(/ (/ 8 4) 2)");
    EXPECT_EXPR("7 % 3 * 2", "(* (% 7 3) 2)");
    EXPECT_EXPR("2 ^ 3 ^ 2", "(^ 2 (^ 3 2))");          /* right-assoc */
    EXPECT_EXPR("-x ^ 2", "(- (^ x 2))");               /* ^ binds tighter */
    EXPECT_EXPR("2 ^ -1", "(^ 2 (- 1))");
    EXPECT_EXPR("- - x", "(- (- x))");
    EXPECT_EXPR("a < b == c > d", "(== (< a b) (> c d))");
    EXPECT_EXPR("a <= b != c >= d", "(!= (<= a b) (>= c d))");
    EXPECT_EXPR("a or b and c", "(or a (and b c))");
    EXPECT_EXPR("not a and b", "(and (not a) b)");
    EXPECT_EXPR("not a == b", "(== (not a) b)");        /* unary binds tighter */
    EXPECT_EXPR("a + b < c * d and e", "(and (< (+ a b) (* c d)) e)");
}

static void test_postfix(void)
{
    EXPECT_EXPR("f()", "(call f)");
    EXPECT_EXPR("f(1, 2, 3)", "(call f 1 2 3)");
    EXPECT_EXPR("f(g(x))", "(call f (call g x))");
    EXPECT_EXPR("f(1)(2)", "(call (call f 1) 2)");
    EXPECT_EXPR("a.b.c", "(. (. a b) c)");
    EXPECT_EXPR("a.b(1).c", "(. (call (. a b) 1) c)");
    EXPECT_EXPR("v[1 + 2][0]", "(index (index v (+ 1 2)) 0)");
    EXPECT_EXPR("-a.b ^ 2", "(- (^ (. a b) 2))");
    EXPECT_EXPR("f(a,\n  b)", "(call f a b)");
}

static void test_quantities(void)
{
    EXPECT_EXPR("20 V", "(quantity 20 V)");
    EXPECT_EXPR("5mm", "(quantity 5 mm)");
    EXPECT_EXPR("5 mm/s", "(quantity 5 (/ mm s))");
    EXPECT_EXPR("45 W/(m*K)", "(quantity 45 (/ W (* m K)))");
    EXPECT_EXPR("460 J/(kg*K)", "(quantity 460 (/ J (* kg K)))");
    EXPECT_EXPR("7850 kg/m^3", "(quantity 7850 (/ kg (^ m 3)))");
    EXPECT_EXPR("1.2e-5 m^2/s", "(quantity 1.2e-5 (/ (^ m 2) s))");
    EXPECT_EXPR("2 m^-2", "(quantity 2 (^ m (- 2)))");
    EXPECT_EXPR("2 m^+2", "(quantity 2 (^ m 2))");
    EXPECT_EXPR("3 kg*m/s^2", "(quantity 3 (/ (* kg m) (^ s 2)))");
    /* The unit ends where '*' or '/' is not followed by a unit. */
    EXPECT_EXPR("5 mm * 2", "(* (quantity 5 mm) 2)");
    EXPECT_EXPR("5 mm / 2 + 1 m", "(+ (/ (quantity 5 mm) 2) (quantity 1 m))");
    /* '(' after '/' continues the unit only if a name (or '(') follows it. */
    EXPECT_EXPR("5 m / (2 s)", "(/ (quantity 5 m) (quantity 2 s))");
    EXPECT_EXPR("5 m * (2 + 3)", "(* (quantity 5 m) (+ 2 3))");
    EXPECT_EXPR("1 W/((m*K))", "(quantity 1 (/ W (* m K)))");
    EXPECT_EXPR("1 J/(kg*K) * (2 K)", "(* (quantity 1 (/ J (* kg K))) (quantity 2 K))");
    /* Per the spec, '/' followed by a name binds to the unit... */
    EXPECT_EXPR("5 mm / t", "(quantity 5 (/ mm t))");
    /* ...so dividing a quantity by a variable needs parentheses. */
    EXPECT_EXPR("(5 mm) / t", "(/ (quantity 5 mm) t)");
    EXPECT_EXPR("-5 mm", "(- (quantity 5 mm))");
    EXPECT_EXPR("5 mm.to(m)", "(call (. (quantity 5 mm) to) m)");
    EXPECT_EXPR("x.to(J/mm)", "(call (. x to) (/ J mm))");
    EXPECT_EXPR("0.1 s + t", "(+ (quantity 0.1 s) t)");
    EXPECT_EXPR("f(5 mm, 2 s)", "(call f (quantity 5 mm) (quantity 2 s))");
}

static void test_statements(void)
{
    EXPECT_AST("", "");
    EXPECT_AST("\n\n# only comments\n\n", "");
    EXPECT_AST("var t = 0 s\nt = t + 0.1 s\n",
               "1:1 (var t (quantity 0 s))\n2:1 (assign t (+ t (quantity 0.1 s)))\n");
    EXPECT_AST("a; b;; c\n", "1:1 (expr a)\n1:4 (expr b)\n1:8 (expr c)\n");
    EXPECT_AST("let x = 1 +\n        2", "1:1 (let x (+ 1 2))\n");
    EXPECT_AST("  print(1)", "1:3 (expr (call print 1))\n");
    EXPECT_AST("x == 1", "1:1 (expr (== x 1))\n");
    EXPECT_AST("let a = 1\r\nlet b = a\r\n", "1:1 (let a 1)\n2:1 (let b a)\n");
}

static void test_errors(void)
{
    EXPECT_DIAGS("let = 5", "1:5: expected variable name after 'let', found '='\n");
    EXPECT_DIAGS("var x 5", "1:7: expected '=' after 'x', found '5'\n");
    EXPECT_DIAGS("let x =", "1:8: expected expression, found end of file\n");
    /* After '=' the line continues (spec 2.1), so the error is at EOF... */
    EXPECT_DIAGS("let x = \n", "2:1: expected expression, found end of file\n");
    /* ...unless the next line starts a statement. */
    EXPECT_DIAGS("let x =\nlet y = 2\n", "1:8: expected expression, found end of line\n");
    EXPECT_DIAGS("let x = (1 + 2", "1:15: expected ')' to close '(' at 1:9, found end of file\n");
    EXPECT_DIAGS("f(1 2)", "1:5: expected ')' after arguments, found '2'\n");
    EXPECT_DIAGS("f(1,)", "1:5: expected expression, found ')'\n");
    EXPECT_DIAGS("v[1", "1:4: expected ']' after index, found end of file\n");
    EXPECT_DIAGS("a.1", "1:3: expected property name after '.', found '1'\n");
    EXPECT_DIAGS("a b", "1:3: expected end of statement (newline or ';'), found 'b'\n");
    EXPECT_DIAGS("1 = 2", "1:3: invalid assignment target (only a variable or an array element can be assigned)\n");
    EXPECT_DIAGS("a.b = 2", "1:5: invalid assignment target (only a variable or an array element can be assigned)\n");
    EXPECT_DIAGS("x = 1 = 2", "1:7: expected end of statement (newline or ';'), found '='\n");
    EXPECT_DIAGS("let u = )", "1:9: expected expression, found ')'\n");
    EXPECT_DIAGS("}", "1:1: expected expression, found '}'\n");
    /* Unit errors. */
    EXPECT_DIAGS("let z = 5 m^x", "1:13: expected integer unit exponent, found 'x'\n");
    EXPECT_DIAGS("let z = 5 m^2.5", "1:13: expected integer unit exponent, found '2.5'\n");
    /* `(m s)` is neither a unit group nor an expression. */
    EXPECT_DIAGS("let s = 5 mm/(x y)", "1:17: expected ')' to close '(' at 1:14, found 'y'\n");
    /* Lexical errors are reported with the lexer's message. */
    EXPECT_DIAGS("let w = 5 @ 2", "1:11: unexpected character\n");
    EXPECT_DIAGS("let s = \"open", "1:9: unterminated string\n");
    EXPECT_DIAGS("let n = !x", "1:9: unexpected '!' (use 'not' for logical negation)\n");
}

static void test_v02_syntax(void)
{
    /* Functions, with and without annotations. */
    EXPECT_AST("fn f(a, b: Number) -> Quantity[J/mm] {\n  return a * b\n}\n",
               "1:1 (fn f (params a (b : Number)) -> Quantity[(/ J mm)] (block (return (* a b))))\n");
    EXPECT_AST("fn g() { return }\nfn h(x) { print(x) }",
               "1:1 (fn g (params) (block (return)))\n2:1 (fn h (params x) (block (expr (call print x))))\n");
    /* if / else if / else, also with else on the next line. */
    EXPECT_AST("if a { b } else if c { d } else { e }",
               "1:1 (if a (block (expr b)) (if c (block (expr d)) (block (expr e))))\n");
    EXPECT_AST("if a {\n  b\n}\nelse {\n  c\n}\n", "1:1 (if a (block (expr b)) (block (expr c)))\n");
    EXPECT_AST("if v == 0 m/s { x = 1 }",
               "1:1 (if (== v (quantity 0 (/ m s))) (block (assign x 1)))\n");
    /* while, for over a range and over an array, blocks. */
    EXPECT_AST("while t < 10 s { t = t + dt }",
               "1:1 (while (< t (quantity 10 s)) (block (assign t (+ t dt))))\n");
    EXPECT_AST("for i in 0..n { print(i) }",
               "1:1 (for i (range 0 n) (block (expr (call print i))))\n");
    EXPECT_AST("for v in [5 mm/s, 0.005 m/s] { }",
               "1:1 (for v (array (quantity 5 (/ mm s)) (quantity 0.005 (/ m s))) (block))\n");
    EXPECT_AST("{ let a = 1; print(a) }", "1:1 (block (let a 1) (expr (call print a)))\n");
    /* Arrays, element assignment, annotations. */
    EXPECT_AST("let a: Array[Quantity[m/s]] = []\na[0] = 1 m/s\nvar n: Number = len(a)",
               "1:1 (let a : Array[Quantity[(/ m s)]] (array))\n2:1 (assign (index a 0) (quantity 1 (/ m s)))\n"
               "3:1 (var n : Number (call len a))\n");
    EXPECT_AST("let m = [[1, 2], [3]]", "1:1 (let m (array (array 1 2) (array 3)))\n");
    EXPECT_AST("let b: Bool = true; let s: String = \"x\"",
               "1:1 (let b : Bool true)\n1:21 (let s : String \"x\")\n");
}

/* v0.3: a parenthesis after '*' or '/' opens a unit group only when its
 * whole content is a unit; otherwise the quantity ends and the parentheses
 * are an ordinary expression. */
static void test_unit_groups(void)
{
    EXPECT_EXPR("6 W/K * (T - T_amb)", "(* (quantity 6 (/ W K)) (- T T_amb))");
    EXPECT_EXPR("5 mm/(x + 1)", "(/ (quantity 5 mm) (+ x 1))");
    EXPECT_EXPR("3 J/(kg*K) * (T - 20 K)", "(* (quantity 3 (/ J (* kg K))) (- T (quantity 20 K)))");
    EXPECT_EXPR("2 m * (a.to(m))", "(* (quantity 2 m) (call (. a to) m))");
    EXPECT_EXPR("5 m/(f(x))", "(/ (quantity 5 m) (call f x))");
    /* unit groups are unchanged */
    EXPECT_EXPR("45 W/(m*K)", "(quantity 45 (/ W (* m K)))");
    EXPECT_EXPR("2 kg*m^2/(s^2*K)", "(quantity 2 (/ (* kg (^ m 2)) (* (^ s 2) K)))");
    EXPECT_EXPR("5 m/(s^-1)", "(quantity 5 (/ m (^ s (- 1))))");
    EXPECT_EXPR("1 W/((m*K))", "(quantity 1 (/ W (* m K)))");
    EXPECT_EXPR("5 m / (2 s)", "(/ (quantity 5 m) (quantity 2 s))");
    /* a single name in parentheses is still a unit (documented rule) */
    EXPECT_EXPR("5 mm/(t)", "(quantity 5 (/ mm t))");
}

/* v0.3: the return type may start the line after the parameters. */
static void test_arrow_on_next_line(void)
{
    EXPECT_AST("fn f(a: Quantity[m],\n     b: Quantity[s])\n    -> Quantity[m/s] {\n    return a / b\n}",
               "1:1 (fn f (params (a : Quantity[m]) (b : Quantity[s])) -> Quantity[(/ m s)] "
               "(block (return (/ a b))))\n");
    EXPECT_AST("fn g()\n\n# comment\n-> Number { return 1 }", "1:1 (fn g (params) -> Number (block (return 1)))\n");
    /* elsewhere '->' still cannot start a line */
    EXPECT_DIAGS("let a = 1\n-> 2", "2:1: expected expression, found '->'\n");
}

/* v0.4: named arguments. */
static void test_named_args(void)
{
    EXPECT_EXPR("f(1, b: 2, c: x + 1)", "(call f 1 b: 2 c: (+ x 1))");
    EXPECT_EXPR("m.f(speed: 5 mm/s)", "(call (. m f) speed: (quantity 5 (/ mm s)))");
    EXPECT_DIAGS("f(a: 1, 2)", "1:9: a positional argument cannot follow a named one (write the "
                               "positional arguments first)\n");
}

static void test_imports(void)
{
    EXPECT_AST("import numerical\nimport \"lib/steel.mfl\"\n# comment\nimport math\nlet a = 1",
               "1:1 (import numerical)\n2:1 (import \"lib/steel.mfl\")\n4:1 (import math)\n"
               "5:1 (let a 1)\n");
}

static void test_v02_errors(void)
{
    EXPECT_DIAGS("return 1", "1:1: 'return' outside a function\n");
    EXPECT_DIAGS("if x { return 1 }", "1:8: 'return' outside a function\n");
    EXPECT_DIAGS("fn f() { fn g() { } }",
                 "1:10: functions can only be declared at the top level (no nested functions in v0.2)\n");
    EXPECT_DIAGS("if x { fn g() { } }",
                 "1:8: functions can only be declared at the top level (no nested functions in v0.2)\n");
    EXPECT_DIAGS("else { }", "1:1: 'else' without a matching 'if'\n");
    EXPECT_DIAGS("let a = 1\nimport numerical",
                 "2:1: 'import' must come before the other statements of the file (only comments "
                 "and other imports may precede it)\n");
    EXPECT_DIAGS("if x { import math }", "1:8: 'import' is only allowed at the top level of a file\n");
    EXPECT_DIAGS("fn f() { import math }", "1:10: 'import' is only allowed at the top level of a file\n");
    EXPECT_DIAGS("import 3",
                 "1:8: expected a module name (import numerical) or a file path in quotes (import "
                 "\"materials.mfl\") after 'import', found '3'\n");
    EXPECT_DIAGS("import math numerical", "1:13: expected end of statement (newline or ';'), found 'numerical'\n");
    EXPECT_DIAGS("if x print(1)", "1:6: expected '{' after the condition of 'if', found 'print'\n");
    EXPECT_DIAGS("while x { a", "1:12: expected '}' to close the block opened at 1:9, found end of file\n");
    EXPECT_DIAGS("for 1 in a { }", "1:5: expected loop variable name after 'for', found '1'\n");
    EXPECT_DIAGS("for i a { }", "1:7: expected 'in' after the loop variable, found 'a'\n");
    EXPECT_DIAGS("fn (x) { }", "1:4: expected function name after 'fn', found '('\n");
    EXPECT_DIAGS("fn f(1) { }", "1:6: expected parameter name, found '1'\n");
    EXPECT_DIAGS("let x: Foo = 1", "1:8: unknown type 'Foo' (expected Number, Quantity[unit], Bool, String or Array[type])\n");
    EXPECT_DIAGS("let x: Quantity = 1", "1:17: expected '[' after Quantity (e.g. Quantity[m/s]), found '='\n");
    EXPECT_DIAGS("let x: Quantity[3] = 1", "1:17: expected a unit inside Quantity[...], found '3'\n");
    EXPECT_DIAGS("let a = [1, 2", "1:14: expected ']' to close the array, found end of file\n");
    EXPECT_DIAGS("f() = 1", "1:5: invalid assignment target (only a variable or an array element can be assigned)\n");
    /* Errors inside a block do not hide the rest of the block or the program. */
    EXPECT_DIAGS("if x {\n  let = 1\n  print(\n}\nlet ok = 1\nlet = 2\n",
                 "2:7: expected variable name after 'let', found '='\n"
                 "4:1: expected expression, found '}'\n"
                 "6:5: expected variable name after 'let', found '='\n");
}

static void test_recovery(void)
{
    MflParseResult r;
    const char *src = "let = 5\n"
                      "let a = 1\n"
                      "let b = (1 +\n"
                      "let c = 2\n"
                      "f(1 2)\n"
                      "let d = 3\n";

    /* One error per broken line; the good lines are still parsed. */
    EXPECT_DIAGS(src, "1:5: expected variable name after 'let', found '='\n"
                      "3:13: expected expression, found end of line\n"
                      "5:5: expected ')' after arguments, found '2'\n");
    CHECK(mfl_parse(src, strlen(src), 0, &r) == 0);
    CHECK(r.program.count == 3); /* a, c, d */
    mfl_parse_free(&r);

    /* An unclosed '(' does not swallow the following statements. */
    EXPECT_DIAGS("let y = (1 + 2\nlet z = 5 m^x\n1 = 2\n",
                 "1:15: expected ')' to close '(' at 1:9, found end of line\n"
                 "2:13: expected integer unit exponent, found 'x'\n"
                 "3:3: invalid assignment target (only a variable or an array element can be assigned)\n");
}

static void test_positions(void)
{
    MflParseResult r;
    const char *src = "let e = a * b\n  f(x).y\n";
    const MflStmt *s1;
    const MflStmt *s2;

    CHECK(mfl_parse(src, strlen(src), 0, &r) == 1);
    s1 = r.program.first;
    s2 = s1->next;
    CHECK(s1->line == 1 && s1->column == 1);
    CHECK(s1->value->kind == MFL_EXPR_BINARY && s1->value->column == 11); /* at '*' */
    CHECK(s1->value->as.binary.left->column == 9);
    CHECK(s2->line == 2 && s2->column == 3);
    CHECK(s2->value->kind == MFL_EXPR_MEMBER && s2->value->column == 3); /* at start */
    CHECK(s2->value->as.member.object->kind == MFL_EXPR_CALL);
    CHECK(s2->value->as.member.object->as.call.argc == 1);
    mfl_parse_free(&r);
}

static char *repeat(const char *head, const char *unit, int n, const char *tail)
{
    size_t hl = strlen(head), ul = strlen(unit), tl = strlen(tail);
    char *s = (char *)malloc(hl + ul * (size_t)n + tl + 1);
    int i;
    if (s == NULL) {
        exit(2);
    }
    memcpy(s, head, hl);
    for (i = 0; i < n; i++) {
        memcpy(s + hl + ul * (size_t)i, unit, ul);
    }
    memcpy(s + hl + ul * (size_t)n, tail, tl + 1);
    return s;
}

static void test_limits(void)
{
    MflParseResult r;
    char *s;
    int i;

    /* Deep nesting is an error, not a stack overflow. */
    s = repeat("let x = ", "(", 100000, "1");
    CHECK(mfl_parse(s, strlen(s), 0, &r) == 0);
    CHECK(r.diagnostic_count == 1 && strstr(r.diagnostics->message, "nested too deeply"));
    mfl_parse_free(&r);
    free(s);

    s = repeat("let x = ", "-", 100000, "1");
    CHECK(mfl_parse(s, strlen(s), 0, &r) == 0);
    CHECK(r.diagnostic_count == 1 && strstr(r.diagnostics->message, "nested too deeply"));
    mfl_parse_free(&r);
    free(s);

    s = repeat("let x = 1", "^1", 100000, "");
    CHECK(mfl_parse(s, strlen(s), 0, &r) == 0);
    CHECK(r.diagnostic_count == 1);
    mfl_parse_free(&r);
    free(s);

    s = repeat("let x = 1 ", "m*(", 100000, "s");
    CHECK(mfl_parse(s, strlen(s), 0, &r) == 0);
    CHECK(r.diagnostic_count == 1);
    mfl_parse_free(&r);
    free(s);

    /* Below the limit is fine. */
    s = repeat("let x = ", "(", MFL_PARSE_MAX_DEPTH / 2, "1");
    {
        size_t n = strlen(s);
        char *t = (char *)malloc(n + MFL_PARSE_MAX_DEPTH / 2 + 1);
        if (t == NULL) {
            exit(2);
        }
        memcpy(t, s, n);
        for (i = 0; i < MFL_PARSE_MAX_DEPTH / 2; i++) {
            t[n + (size_t)i] = ')';
        }
        t[n + MFL_PARSE_MAX_DEPTH / 2] = '\0';
        CHECK(mfl_parse(t, strlen(t), 0, &r) == 1);
        mfl_parse_free(&r);
        free(t);
    }
    free(s);

    /* The number of diagnostics is capped. */
    s = repeat("", ")\n", 1000, "");
    CHECK(mfl_parse(s, strlen(s), 0, &r) == 0);
    CHECK(r.diagnostic_count == MFL_PARSE_MAX_DIAGNOSTICS);
    {
        const MflDiagnostic *d = r.diagnostics;
        while (d->next != NULL) {
            d = d->next;
        }
        CHECK(strcmp(d->message, "too many errors; stopping") == 0);
    }
    mfl_parse_free(&r);
    free(s);
}

/* Every possible memory limit must fail cleanly or succeed. */
static void test_out_of_memory(void)
{
    const char *src = "let speed = 5 mm/s\nlet q = 0.8 * 20 V * 150 A / speed\n"
                      "print(q.to(J/mm))\nlet = 1\n";
    size_t limit;
    int saw_oom = 0;
    int saw_full = 0;

    for (limit = 1; limit < 20000; limit += 7) {
        MflParseResult r;
        mfl_parse(src, strlen(src), limit, &r);
        if (r.out_of_memory) {
            saw_oom = 1;
            CHECK(r.program.first == NULL && r.program.count == 0);
        } else {
            saw_full = 1;
            CHECK(r.program.count == 3 && r.diagnostic_count == 1);
        }
        mfl_parse_free(&r);
    }
    CHECK(saw_oom && saw_full);
}

/* Random token soup: must terminate, cap diagnostics and print cleanly. */
static void test_random_programs(void)
{
    static const char *const pieces[] = {
        "let ", "var ", "x", "y ", "=", "==", "5", "2.5e3", " mm", "/", "*", "^", "-", "+",
        "(", ")", "[", "]", ",", ".", "to", "\n", ";", " ", "\"s\"", "not ", "and ", "or ",
        "if ", "{", "}", "fn ", "@", "%", "<", ">=", "!", "m", "s", "#c\n", "\r\n", "true",
        "nil", ".."};
    unsigned long seed = 4242UL;
    int round;
    Buf out = {NULL, 0, 0};

    for (round = 0; round < 3000; round++) {
        char src[600];
        size_t len = 0;
        int n = (int)((seed >> 8) % 60);
        int i;
        MflParseResult r;
        int ok;

        for (i = 0; i < n; i++) {
            const char *piece;
            size_t pl;
            seed = seed * 1103515245UL + 12345UL;
            piece = pieces[(seed >> 16) % (sizeof pieces / sizeof pieces[0])];
            pl = strlen(piece);
            if (len + pl >= sizeof src) {
                break;
            }
            memcpy(src + len, piece, pl);
            len += pl;
        }
        ok = mfl_parse(src, len, 0, &r);
        CHECK(r.diagnostic_count <= MFL_PARSE_MAX_DIAGNOSTICS);
        CHECK(ok == (r.diagnostic_count == 0));
        buf_reset(&out);
        mfl_ast_print_program(&r.program, buf_write, &out);
        if (ok && out.data != NULL) {
            /* Successful parses never contain error placeholders. */
            CHECK(strstr(out.data, "<error>") == NULL && strstr(out.data, "<null>") == NULL);
        }
        mfl_parse_free(&r);
        seed = seed * 1103515245UL + 12345UL;
    }
    free(out.data);
}

int main(void)
{
    test_heat_input();
    test_literals();
    test_precedence();
    test_postfix();
    test_quantities();
    test_statements();
    test_errors();
    test_v02_syntax();
    test_v02_errors();
    test_imports();
    test_unit_groups();
    test_named_args();
    test_arrow_on_next_line();
    test_recovery();
    test_positions();
    test_limits();
    test_out_of_memory();
    test_random_programs();

    free(g_ast.data);
    free(g_diags.data);
    printf("test_parser: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
