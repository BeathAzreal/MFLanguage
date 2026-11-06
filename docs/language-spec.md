# MFL Language Specification — Draft 0

> **Status:** preliminary proposal. Nothing in this document is implemented
> yet. Syntax and semantics may change before v0.1. Sections marked
> *(open question)* are explicitly undecided.

## 1. Design principles

1. **Readable by engineers first.** A welding engineer should be able to read
   an MFL model without training in programming-language theory.
2. **Units are part of the value.** `5 mm/s` is a single value (a
   *quantity*), not a number plus a comment.
3. **Fail loudly on dimensional errors.** Adding `V` to `A` is an error, not a
   silent number.
4. **Small core, scientific modules outside.** The core knows numbers,
   quantities, strings, collections and functions; physics lives in modules.
5. **`double` by default.** All real numbers are IEEE-754 binary64.
6. **Reproducible.** Programs should be able to record the parameters, units,
   methods and results of a run.

## 2. Source text

> **Implemented** (lexer, v0.1): everything in sections 2 and 3.
> Reference implementation: `src/lexer/lexer.c`; inspect with
> `mfl --tokens file.mfl`.

- Encoding: UTF-8; a leading byte-order mark is ignored. Non-ASCII
  characters are allowed **only inside strings and comments** in v0.1.
- Identifiers are ASCII: `[A-Za-z_][A-Za-z0-9_]*`, case-sensitive.
- Line comments start with `#` and run to the end of the line.
- Whitespace: space, tab, carriage return. `\r\n` behaves like `\n`.
- Positions are reported as `line:column`, both 1-based; columns count
  Unicode code points (a tab counts as one column).

### 2.1 Statement separators

Statements are separated by a newline or by `;`. The lexer emits a single
`NEWLINE` token for any run of line breaks, blank lines and comment-only
lines. A line break does **not** produce `NEWLINE` when:

1. it is inside `( … )` or `[ … ]` (at any nesting depth);
2. the previous token cannot end an expression: a binary operator
   (`+ - * / ^ % == != < <= > >= and or`), `not`, `=`, `,`, `.`, `..`,
   `->`, `:`;
3. the previous token is `;` or `{`, or it is the start of the file.

**Exception (error recovery):** a line that starts with a keyword that can
only begin a statement — `let var fn if while for return import` — always
ends the previous statement, even inside an unclosed `(`/`[` or after a
trailing operator. These keywords cannot appear inside an expression, so
this never changes a valid program; it only makes an unclosed group or a
dangling operator an error at that line instead of swallowing the rest of
the file.

So an expression can be continued by ending a line with an operator, but
**not** by starting the next line with one:

```mfl
let q = eta * U * I /     # continues
        v
let r = a
- b                       # two statements: `a`, then `-b`
```

## 3. Lexical elements

| Token kind | Rule | Examples |
|------------|------|----------|
| Number | `digits ('.' digits)? ([eE] [+-]? digits)?` | `20`, `0.8`, `1.5e-3`, `6.02e23` |
| String | `"…"` on one line; escapes `\n \t \" \\` | `"Hello"` |
| Identifier | see §2 | `voltage`, `heat_input`, `T0`, `mm` |
| Keyword | reserved identifiers | `let var fn return if else while for in true false nil and or not import` |
| Operators | longest match | `+ - * / ^ % = == != < <= > >= . .. -> , : ; ( ) [ ] { }` |

Number details:

- No sign: `-3` is `MINUS NUMBER`.
- A `.` belongs to the number only if a digit follows, so `0..n` is
  `NUMBER DOT_DOT IDENT` and `5.to(J)` is `NUMBER DOT IDENT …`.
- An exponent needs at least one digit; otherwise `e` starts an
  identifier (`2e` is `NUMBER IDENT`).
- A unit may follow with or without a space: `5mm` and `5 mm` both give
  `NUMBER IDENT`.
- The lexer only validates the shape. Conversion to `double` happens in
  a later stage (it must be locale-independent and correctly rounded).

Unit symbols (`V`, `A`, `mm`, `J`, …) are lexed as identifiers; the parser
decides whether an identifier is a unit (see §5). `µ`/`Ω` cannot be used
in v0.1 code; use `u`/`ohm` (see `units.md`).

### 3.1 Lexical errors

Errors do not stop lexing; each produces an error token with its position
and scanning continues, so several errors are reported at once:

| Input | Error |
|-------|-------|
| `@`, `$`, `` ` ``, control characters, NUL | unexpected character |
| `!` not followed by `=` | unexpected `!` (use `not`) |
| non-ASCII outside a string/comment | one error per code point |
| `"abc` (end of line or file) | unterminated string |
| `"a\qb"` | invalid escape sequence |

## 4. Values and types

| Type | Description |
|------|-------------|
| `Number` | binary64 real, dimensionless |
| `Quantity` | binary64 magnitude + dimension vector + display unit |
| `Bool` | `true` / `false` |
| `String` | immutable UTF-8 text |
| `Nil` | absence of value |
| `Array` | ordered, growable sequence *(v0.2)* |
| `Function` | first-class function / closure *(v0.2)* |

A `Number` is semantically a dimensionless `Quantity`.

### 4.0 Static checking in v0.1

> **Implemented** (semantic checker, `src/compiler/checker.c`; inspect with
> `mfl --types file.mfl`).

v0.1 has no user functions and no control flow, so the type of every
expression, including the **dimension** of every quantity, is known before
the program runs, and all checks below are done before execution:

| Rule | Example error |
|------|---------------|
| Names must be declared before use; no redeclaration | `undefined variable 'y'`, `'U' is already declared at 2:1` |
| `let` bindings cannot be assigned | `cannot assign to 'U': it is declared with 'let' …` |
| A `var` keeps the type and dimension of its initial value | `cannot assign Quantity[s] to 'T', which is a Quantity[K] …` |
| `+ - %` and `< <= > >=` need numbers with equal dimensions | `cannot add Quantity[kg*m^2/(s^3*A)] and Quantity[A]: dimensions differ` |
| `* /` combine dimensions | `W/(m*K) / (kg/m^3 * J/(kg*K))` → `Quantity[m^2/s]` |
| `^` needs a dimensionless exponent; if the base has a dimension, an integer literal | `the exponent of a Quantity[m] must be an integer literal …` |
| `== !=` need the same type (numbers: same dimension); anything may be compared with `nil` | `cannot compare Number and String` |
| `and or not` need `Bool` (no "truthy" numbers) | `'not' needs a Bool, found Number` |
| `.to(u)` / `.value_in(u)` need a unit with the same dimension | `cannot convert Quantity[m/s] to a unit of dimension kg*m/s^2` |
| `print` is the only callable; it cannot be redefined or used as a value | `undefined function 'f' (v0.1 has only the built-in 'print')` |

Types are written `Number` (dimensionless), `Quantity[<SI base units>]`,
`Bool`, `String`, `Nil`. Dimensions are always shown in SI base units
(`kg*m/s^2`), because one dimension can have several names (J/m and N).

### 4.1 Gradual typing *(v0.2+)*

Annotations are optional. Without them, checks happen at run time. With them,
the compiler checks statically where possible:

```mfl
fn heat_input(eta: Number, U: Quantity[V], I: Quantity[A], v: Quantity[mm/s])
    -> Quantity[J/mm] {
    return eta * U * I / v
}
```

`Quantity[unit]` constrains the **dimension**, not the exact unit: passing
`0.3 m/min` where `Quantity[mm/s]` is expected is valid (same dimension
`L·T⁻¹`).

## 5. Quantities and unit expressions

A **quantity literal** is a number immediately followed by a *unit
expression*:

```mfl
let speed = 5 mm/s
let k     = 45 W/(m*K)
let rho   = 7850 kg/m^3
```

> **Implemented** (v0.1): parsing, unit resolution and dimension checks
> (§4.0), and exact conversion at run time (`docs/units.md` §6).

Unit expression grammar:

```
quantity    := NUMBER unit_expr          -- unit_expr must start with a name
unit_expr   := unit_term (('*' | '/') unit_term)*
unit_term   := unit_atom ('^' ('+' | '-')? INTEGER)?
unit_atom   := UNIT_SYMBOL | '(' unit_expr ')'
```

Binding rules (the parser applies them; `mfl --ast` shows the result):

| Source | Tree | Why |
|--------|------|-----|
| `5 mm/s` | `(quantity 5 (/ mm s))` | `/` followed by a name continues the unit |
| `45 W/(m*K)` | `(quantity 45 (/ W (* m K)))` | `(` + name after `/` or `*` opens a unit group |
| `5 m / (2 s)` | `(/ (quantity 5 m) (quantity 2 s))` | `(` + number: ordinary division |
| `2 m^-2` | `(quantity 2 (^ m (- 2)))` | integer exponent, optional sign |
| `5 mm * 2` | `(* (quantity 5 mm) 2)` | `*` followed by a number ends the unit |
| `5 mm / t` | `(quantity 5 (/ mm t))` | **`t` is read as a unit**, not a variable |
| `(5 mm) / t` | `(/ (quantity 5 mm) t)` | parentheses end the quantity |
| `5 mm.to(m)` | `(call (. (quantity 5 mm) to) m)` | `.` ends the unit |

So, in a quantity literal, `/` and `*` bind to the unit: `5 mm/s` is five
millimetres per second, not `5 mm` divided by a variable `s`. To divide or
multiply a quantity by a variable, put the quantity in parentheses. Unit
symbols live in their own namespace, so a variable named `s` does not
shadow the second.

A space between number and unit is optional: `5mm` and `5 mm` are the same.
Exponents must be integer literals (`m^2.5` and `m^x` are syntax errors).
`%` is **not** a unit in v0.1 (it is the remainder operator); write `0.5`
instead of `50 %`.

### 5.1 Conversion

```mfl
energy.to(J/mm)       # convert to a compatible unit; error if dimensions differ
energy.value_in(J/mm) # plain Number in the given unit
energy.dim            # dimension descriptor (planned, not in v0.1)
```

The argument of `.to`/`.value_in` is read as a **unit expression** (unit
names, `*`, `/`, integer `^`), not as an ordinary expression. Dimension
compatibility is checked before running (§4.0).

## 6. Declarations

> **Parsing implemented** (v0.1).

```mfl
let x = 10 mm      # immutable binding
var t = 0 s        # mutable binding
t = t + 0.1 s      # assignment
```

Only a plain variable can be assigned in v0.1 (`a.b = 1` and `f() = 1` are
syntax errors). Whether the variable exists and is mutable is checked by a
later stage.

## 7. Expressions

> **Parsing implemented** (v0.1).

Precedence (high → low):

| Level | Operators | Assoc. |
|-------|-----------|--------|
| 1 | call `f()`, member `.`, index `[]` | left |
| 2 | `^` | right |
| 3 | unary `-`, `not` | right |
| 4 | `*`, `/`, `%` | left |
| 5 | `+`, `-` | left |
| 6 | `<`, `<=`, `>`, `>=` | left |
| 7 | `==`, `!=` | left |
| 8 | `and` | left |
| 9 | `or` | left |

Consequences worth knowing: `-x^2` is `-(x^2)`; `2^-1` is allowed;
`2^3^2` is `2^(3^2)`; `not a == b` is `(not a) == b`; comparisons do not
chain (`a < b < c` is `(a < b) < c`).

Dimensional rules: `+ - < <= > >= == !=` require equal dimensions;
`* /` combine dimensions; `^` requires a dimensionless, compile-time-known
exponent when the base has a dimension.

## 8. Control flow and functions *(v0.2)*

Not parsed in v0.1: a statement starting with `fn`, `if`, `else`, `while`,
`for`, `return` or `import` is reported as "not supported yet (planned for
v0.2)" and skipped, including its braced block.

```mfl
if T > 1500 degC { print("melted") } else { print("solid") }

while t < 10 s { t = t + dt }

for i in 0..n { ... }

fn area(r) { return pi * r^2 }
```

## 9. Built-ins (v0.1)

> **Implemented** (compiler `src/compiler/compiler.c`, VM `src/vm/vm.c`).

| Name | Description |
|------|-------------|
| `print(x, ...)` | Print values separated by single spaces, then a newline; returns `nil` |
| `.to(unit)` | Same quantity, expressed in `unit` (see §5.1) |
| `.value_in(unit)` | Plain `Number`: the magnitude in `unit` |

### 9.1 Output format

| Value | Printed as |
|-------|-----------|
| Number | shortest decimal that reads back to the same double: `480`, `0.8`, `0.30000000000000004`; exponent form outside 1e-4 ≤ \|x\| < 1e16: `1.5e-5`, `6.02e23`, `1e16`; `inf`, `-inf`, `nan` |
| Quantity | number, one space, display unit (`docs/units.md` §4): `480 J/mm`, `45 W/(m*K)` |
| Bool, Nil | `true`, `false`, `nil` |
| String | its characters, escapes decoded |

The decimal separator is always `.`, whatever the locale of the host
program; reading literals is locale-independent too.

### 9.2 Arithmetic semantics

- All numbers are IEEE 754 binary64. Division by zero gives `inf`, `-inf`
  or `nan`, as in C, Fortran or NumPy; v0.1 has **no run-time errors**,
  because names, types and dimensions are checked before running.
- `a % b` is the C `fmod`: the result has the sign of `a` (`-7 % 3` is `-1`).
- `0.1 + 0.2 == 0.3` is `false`, as in any binary floating-point system.
- `and`/`or` short-circuit: the right operand is not evaluated when the
  left one decides the result.

## 10. Conformance example #1 — welding heat input

```mfl
let voltage = 20 V
let current = 150 A
let speed = 5 mm/s
let efficiency = 0.8

let energy = efficiency * voltage * current / speed

print(energy.to(J/mm))
```

Expected standard output:

```
480 J/mm
```

Derivation: `0.8 · 20 V · 150 A / 5 mm/s = 480 V·A·s/mm = 480 W·s/mm = 480 J/mm`.

Files: `tests/language/conformance_heat_input.mfl` /
`.expected`. This test becomes mandatory for the v0.1 release.

## 11. Open questions

- Temperature: affine units (`degC`) vs. absolute (`K`) — see `units.md` §5.
- Should integer types exist, or only binary64? (Draft: only binary64 in v0.1.)
- Module syntax: `import welding` vs. `use welding.*`.
- Vector/matrix literals and element-wise units.
