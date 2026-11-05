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

- Encoding: UTF-8. Identifiers are ASCII in v0.1 (`[A-Za-z_][A-Za-z0-9_]*`).
- Line comments start with `#`.
- Statements are separated by newlines; `;` is allowed as an explicit
  separator. A line ending inside `(`/`[` or after a binary operator
  continues on the next line.

## 3. Lexical elements

| Token kind | Examples |
|------------|----------|
| Number | `20`, `0.8`, `1.5e-3`, `6.02e23` |
| String | `"Hello"`, with escapes `\n \t \" \\` |
| Identifier | `voltage`, `heat_input`, `T0` |
| Keyword | `let`, `var`, `fn`, `return`, `if`, `else`, `while`, `for`, `in`, `true`, `false`, `nil`, `and`, `or`, `not`, `import` |
| Operators | `+ - * / ^ % == != < <= > >= = . , ( ) [ ] { } :` |

Unit symbols (`V`, `A`, `mm`, `J`, …) are lexed as identifiers; the parser
decides whether an identifier is a unit (see §5).

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

Unit expression grammar:

```
unit_expr   := unit_term (('*' | '/') unit_term)*
unit_term   := unit_atom ('^' signed_int)?
unit_atom   := UNIT_SYMBOL | '(' unit_expr ')'
```

In a quantity literal, `/` and `*` bind to the unit (so `5 mm/s` is five
millimetres per second, not `5 mm` divided by a variable `s`). Unit symbols
live in their own namespace, so a variable named `s` does not shadow the
second.

*(open question)* whether to require a space or allow `5mm`. Draft: both
are accepted.

### 5.1 Conversion

```mfl
energy.to(J/mm)       # convert to a compatible unit; error if dimensions differ
energy.value_in(J/mm) # plain Number in the given unit
energy.dim            # dimension descriptor, e.g. M·T⁻²
```

## 6. Declarations

```mfl
let x = 10 mm      # immutable binding
var t = 0 s        # mutable binding
t = t + 0.1 s
```

## 7. Expressions

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

Dimensional rules: `+ - < <= > >= == !=` require equal dimensions;
`* /` combine dimensions; `^` requires a dimensionless, compile-time-known
exponent when the base has a dimension.

## 8. Control flow and functions *(v0.2)*

```mfl
if T > 1500 degC { print("melted") } else { print("solid") }

while t < 10 s { t = t + dt }

for i in 0..n { ... }

fn area(r) { return pi * r^2 }
```

## 9. Built-ins (v0.1)

| Name | Description |
|------|-------------|
| `print(x, ...)` | Print values separated by spaces, followed by newline |
| `.to(unit)` | Unit conversion (see §5.1) |

### 9.1 Output format of quantities

`print` writes the magnitude using the shortest representation that
round-trips (`%.17g` trimmed; integers without decimal point), a single
space, and the unit as written by the user:

```
480 J/mm
```

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
