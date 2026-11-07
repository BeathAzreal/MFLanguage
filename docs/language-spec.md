# MFL Language Specification — v0.3

> **Status:** sections marked **Implemented** describe the behaviour of the
> current interpreter (v0.3, pre-alpha) and are covered by tests in
> `tests/`. Syntax and semantics may still change before v1.0. Sections
> marked *(open question)* are explicitly undecided.

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
  characters are allowed **only inside strings and comments** (v0.1, v0.2).
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

**Exception (v0.3): `->`.** The return type of a function may start the
line after its parameter list (blank and comment lines may come between).
`->` cannot begin a statement, so this never changes a valid program:

```mfl
fn heat_input(U: Quantity[V], I: Quantity[A],
              v: Quantity[mm/s])
    -> Quantity[J/mm] {
    return 0.8 * U * I / v
}
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
in code; use `u`/`ohm` (see `units.md`).

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
| `Array` | ordered, growable, homogeneous sequence (§8.4) |
| `Function` | first-class function value, no closures (§8.1) |

A `Number` is semantically a dimensionless `Quantity`. A `Quantity` in
`degC` is an *absolute* temperature (`docs/units.md` §5).

### 4.0 Static checking

> **Implemented.** Inspect inferred types with `mfl --types file.mfl`.

Every expression gets a type before the program runs. When the type can be
known — literals, annotated names, operations on known types, calls of
functions with an annotated return type — all the rules below are checked
**before running** and reported as `file:line:col: error: ...`. When it
cannot be known (§4.1), the same rules are checked **when the operation
runs** and reported as `file:line:col: runtime error: ...`.

| Rule | Example error |
|------|---------------|
| Names must be declared before use (top-level code); no redeclaration in the same scope | `undefined variable 'y'`, `'U' is already declared at 2:1` |
| `let` bindings, parameters and loop variables cannot be assigned | `cannot assign to 'p': it is a parameter (immutable)` |
| A `var` keeps the type and dimension of its initial value (or its annotation) | `cannot assign Quantity[s] to 'T', which is a Quantity[K] (declared at 1:1)` |
| `+ - %` and `< <= > >=` need numbers with equal dimensions | `cannot add Quantity[kg*m^2/(s^3*A)] and Quantity[A]: dimensions differ` |
| `* /` combine dimensions | `W/(m*K) / (kg/m^3 * J/(kg*K))` → `Quantity[m^2/s]` |
| `^` needs a dimensionless exponent; if the base has a dimension, an integer literal (statically) or a whole number (at run time) | `the exponent of a Quantity[m] must be an integer literal …` |
| `== !=` need the same type (numbers: same dimension); anything may be compared with `nil`; arrays cannot be compared | `cannot compare Number and String` |
| `and or not` and conditions of `if`/`while` need `Bool` (no "truthy" numbers) | `the condition of 'if' must be a Bool, found Number` |
| `.to(u)` / `.value_in(u)` need a unit with the same dimension | `cannot convert Quantity[m/s] to a unit of dimension kg*m/s^2` |
| Calls: exact number of arguments; arguments compatible with annotated parameters | `argument 3 of 'heat_input' ('speed') must be Quantity[m/s] (declared as Quantity[mm/s]), found Quantity[kg*m^2/s^3]` |
| Returns compatible with the annotated return type; an annotated function must return on every path | `function 'f' is declared to return Number, but can reach the end of its body without 'return'` |
| Array elements: same type and dimension | `array elements must all have the same type and dimension: element 2 is Quantity[s], …` |
| Absolute temperatures (degC): only `±` a difference, `degC − degC`, comparisons, `.to()` | `cannot add two absolute temperatures (degC + degC) …` |

Types are written `Number` (dimensionless), `Quantity[<SI base units>]`,
`Quantity[degC]` (absolute temperature), `Bool`, `String`, `Nil`,
`Array[T]`, `Function`, and `Any` (known only at run time). Dimensions are
shown in SI base units (`kg*m/s^2`), because one dimension can have several
names (J/m and N); where an annotation was written differently, messages add
it: `(declared as Quantity[J/mm])`.

### 4.1 Gradual typing

> **Implemented.**

Annotations are optional (`let x: T = …`, `fn f(p: T) -> R`). Where they
are missing, the type is **inferred** when possible and is **`Any`**
otherwise. `Any` appears for:

- unannotated parameters (inside the function);
- the result of a call to a function without a return annotation, or of a
  call through a value of unknown type;
- operations whose result depends on an `Any` operand (e.g. `x * y` with
  `x: Any`). `x + 1 m` with `x: Any` has the type `Quantity[m]`, because
  `+` requires equal dimensions.

Rules for code without annotations:

1. The program is accepted if nothing is provably wrong; every operation on
   `Any` values is checked when it runs, with the same rules and messages.
2. A value of type `Any` assigned to an annotated name, passed to an
   annotated parameter, or returned from a function with an annotated
   return type is checked at that point when it runs.
3. A `var` initialised with an `Any` value keeps the type and dimension of
   its first value (ADR-017 at run time).
4. Conditions, `and`/`or` operands, range bounds and the iterable of `for`
   are checked when they run if their type is `Any`.

```mfl
fn heat_input(eta: Number, U: Quantity[V], I: Quantity[A], v: Quantity[mm/s])
    -> Quantity[J/mm] {
    return eta * U * I / v               # checked before running
}

fn add(a, b) { return a + b }            # a, b: Any
print(add(1 m, 2 mm))                    # 1.002 m
print(add(1 m, 2 s))                     # runtime error: cannot add Quantity[m] and Quantity[s]
```

`Quantity[unit]` constrains the **dimension**, not the exact unit: passing
`0.3 m/min` where `Quantity[mm/s]` is expected is valid. `Quantity[degC]` is
an absolute temperature, `Quantity[K]` a temperature (or difference) in kelvin.

Supported annotations: `Number`, `Bool`, `String`, `Quantity[unit]`,
`Array[T]` (nested: `Array[Array[Number]]`). There is no annotation for
functions or `Any`: leave the annotation out instead.

## 5. Quantities and unit expressions

A **quantity literal** is a number immediately followed by a *unit
expression*:

```mfl
let speed = 5 mm/s
let k     = 45 W/(m*K)
let rho   = 7850 kg/m^3
```

> **Implemented**: parsing, unit resolution and dimension checks
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
| `45 W/(m*K)` | `(quantity 45 (/ W (* m K)))` | `(` after `/` or `*` opens a unit group when **everything up to its `)`** is a unit |
| `5 m / (2 s)` | `(/ (quantity 5 m) (quantity 2 s))` | the group holds a number: ordinary division |
| `6 W/K * (T - T_amb)` | `(* (quantity 6 (/ W K)) (- T T_amb))` | the group holds an operator that units do not use: the quantity ends (v0.3) |
| `2 m^-2` | `(quantity 2 (^ m (- 2)))` | integer exponent, optional sign |
| `5 mm * 2` | `(* (quantity 5 mm) 2)` | `*` followed by a number ends the unit |
| `5 mm / t` | `(quantity 5 (/ mm t))` | **`t` is read as a unit**, not a variable |
| `(5 mm) / t` | `(/ (quantity 5 mm) t)` | parentheses end the quantity |
| `5 mm.to(m)` | `(call (. (quantity 5 mm) to) m)` | `.` ends the unit |

So, in a quantity literal, `/` and `*` bind to the unit: `5 mm/s` is five
millimetres per second, not `5 mm` divided by a variable `s`. A
parenthesised group after `/` or `*` belongs to the unit only if it
contains nothing but unit names, `*`, `/`, nested parentheses and integer
exponents; anything else (`+`, `-` between terms, numbers, calls, `.to`)
ends the quantity, so `6 W/K * (T - T_amb)` multiplies as it reads. To
divide or multiply a quantity by a single variable, put the quantity in
parentheses: `(5 mm) / t`. Unit
symbols live in their own namespace, so a variable named `s` does not
shadow the second.

A space between number and unit is optional: `5mm` and `5 mm` are the same.
Exponents must be integer literals (`m^2.5` and `m^x` are syntax errors).
`%` is **not** a unit (it is the remainder operator); write `0.5` or `50 percent`
instead of `50 %`.

### 5.1 Conversion

```mfl
energy.to(J/mm)       # convert to a compatible unit; error if dimensions differ
energy.value_in(J/mm) # plain Number in the given unit
energy.dim            # dimension descriptor (planned, not implemented)
```

The argument of `.to`/`.value_in` is read as a **unit expression** (unit
names, `*`, `/`, integer `^`), not as an ordinary expression. Dimension
compatibility is checked before running (§4.0).

## 6. Declarations and scope

> **Implemented.**

```mfl
let x = 10 mm               # immutable binding
var t = 0 s                 # mutable binding
t = t + 0.1 s               # assignment
let v: Quantity[m/s] = 5 mm/s   # annotated (§4.1)
a[2] = 3 mm                 # array element (§8.4)
```

- **Globals** are `let`/`var` at the top level, outside any block. They are
  visible in top-level code after their declaration and in every function
  body. Reading a global from a function before its declaration has run is
  a run-time error: `'limit' is used before its declaration has run`.
- **Locals** are declared inside a block `{ … }` or a function. They are
  visible from their declaration to the end of the enclosing block.
- A local may **shadow** a global or a function of the same name; it may not
  redeclare a name already declared in the same function (including its
  parameters and enclosing blocks): `'b' is already declared in this
  function at 17:11`. Sibling blocks may reuse a name.
- Parameters and loop variables are immutable.
- Built-in names (`print`, `len`, `push`) cannot be declared.
- `let` fixes the binding, not the contents of an array (§8.4).

## 7. Expressions

> **Implemented.**

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

## 8. Control flow, functions and arrays

> **Implemented** (v0.2). `break`, `continue` and nested functions/closures
> are not part of v0.2 (§12).

### 8.1 Functions

```mfl
fn name(p1: T1, p2, ...) -> R {    # annotations optional
    ...
    return value
}
```

- Declared at the **top level** only (`functions can only be declared at the
  top level (no nested functions in v0.2)`). Functions are **hoisted**: a
  function can be called anywhere, including before its declaration and
  from other functions (recursion and mutual recursion are allowed).
- Calls need exactly the declared number of arguments (no defaults, no
  variadic functions).
- **Named arguments** (v0.4): `heat_input(voltage: 25 V, current: 250 A,
  speed: 5 mm/s, efficiency: 0.8)`. Positional arguments come first, then
  named ones in any order (`f(1, c: 3, b: 2)`); a positional argument after
  a named one is a syntax error. Each parameter is given exactly once:
  `'f' has no parameter 'c' (its parameters are: a, b)`,
  `argument 'a' of 'f' is given twice`, `missing argument 'a' of 'f'`.
  Arguments are **evaluated in parameter order**, whatever order they are
  written in, so `f(b: g(), a: h())` calls `h` first. Named arguments work
  for top-level functions and module functions (`welding.t85_thin(...)`);
  built-ins (`print`, `len`, `.to`, ...) and calls through a value whose
  function is only known at run time reject them with a compile error.
- A function is a value: `let g = twice; apply(g, 4 s)`; printing it shows
  `<fn twice>`. Calls through a value are checked when they run (arity,
  annotated parameters).
- No closures: a function body sees its parameters and locals, the globals
  and the other functions — never the locals of another call.

### 8.2 Return values

| Function | `return expr` | `return` (no value) | End of body reached |
|----------|---------------|---------------------|---------------------|
| with `-> R` | must be compatible with `R` | compile error | **compile error** (every path must return) |
| without `-> R` | any value | returns `nil` | returns `nil` |

"Every path returns" is decided syntactically: a `return`, a block that
always returns, or an `if` with an `else` whose branches all return.
Loops never count. `return` outside a function is a syntax error.

**Call depth:** at most 1000 nested calls; deeper recursion is the run-time
error `call depth limit of 1000 exceeded (endless recursion?)`, reported at
the call. The VM never overflows the C stack (calls do not recurse in C).

### 8.3 Conditions and loops

```mfl
if q < 0.5 kJ/mm {
    print("low")
} else if q < 1.5 kJ/mm {
    print("medium")
} else {
    print("high")
}

while t < 10 s { t = t + dt }

for i in 0..n { ... }        # i = 0, 1, ..., while i < n   (half-open)
for v in speeds { ... }      # each element, in order
```

- Conditions must be `Bool`. `else` may start the line after `}`.
- `for i in a..b`: `a` and `b` are plain numbers (dimensionless), evaluated
  once; `i` starts at `a` and grows by 1 while `i < b`. `0..0` runs zero
  times; `0..2.5` gives 0, 1, 2.
- `for x in array`: visits indices 0, 1, … while the index is below the
  array's **current** length (elements pushed during the loop are visited).
- Every block (`if`, `else`, `while`, `for`, `fn`, or a bare `{ … }`) has its
  own scope. The loop variable belongs to the loop body.
- `and` / `or` short-circuit: the right operand is not evaluated when the
  left one decides.

### 8.4 Arrays

```mfl
let speeds = [5 mm/s, 0.005 m/s, 0.3 m/min]   # Array[Quantity[m/s]]
print(speeds[0], len(speeds))                  # 5 mm/s 3
var log = []                                   # empty: element type fixed by the first push
push(log, "first")
speeds[1] = 6 mm/s
```

- **Homogeneous:** all elements have the same type; numbers the same
  dimension (and absolute-temperature flag). Each element keeps its own
  unit, so equivalent quantities in different units share one array.
  Mixing dimensions is an error (before running if known, otherwise when
  the array is built, an element is stored or pushed).
- **Indices** start at 0 and must be whole, dimensionless numbers. Any other
  index is a run-time error with its position: `index 3 is out of range for
  an array of length 3 (valid indices: 0 to 2)`, `array index must be a whole
  number, found 1.5`.
- **References:** `let b = a` refers to the same array; `push(b, 3)` changes
  `a` too. `let` fixes the binding, not the contents.
- `==` / `!=` on arrays is an error (compare the elements).
- Printing: `[5 mm/s, 0.005 m/s]`; strings inside arrays are quoted.
- Arrays live until the end of the run (no garbage collector yet); memory
  is bounded by the host's limit.

## 9. Built-ins

> **Implemented** (compiler `src/compiler/compiler.c`, VM `src/vm/vm.c`).

| Name | Description |
|------|-------------|
| `print(x, ...)` | Print values separated by single spaces, then a newline; returns `nil` |
| `len(a)` | Number of elements of an array |
| `push(a, x)` | Append `x` to array `a` (same type and dimension as its elements); returns `nil` |
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
  or `nan`, as in C, Fortran or NumPy (it is not an error; test the divisor
  with `if` when it matters, as `examples/welding/heat_input_v02.mfl` does).
- Run-time errors (v0.2) exist only where something could not be checked
  before running: operations on `Any` values, array indices, calls through
  values, call depth, globals read before their declaration ran, and the
  optional instruction limit of the embedding API. They stop the program
  and are reported as `file:line:col: runtime error: message`; output
  printed before stays.
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

## 11. Conformance example #2 — v0.2 acceptance demonstration

`examples/welding/heat_input_v02.mfl` (checked by the `acceptance_*` CTest
tests) combines an annotated function, an array of speeds in different
units, loops, a zero-speed branch, unannotated code and degC. Its output:

```
v = 5 mm/s -> 480 J/mm
v = 0.005 m/s -> 480 J/mm
v = 10 mm/s -> 240 J/mm
v = 0 mm/s -> arc not moving: heat input not defined, skipped
computed 3 of 4 cases; mean = 400 J/mm
unchecked v = 0: inf J/mm
interpass 150 degC is 100 delta_degC below the limit 250 degC
```

## 12. Open questions

- ~~Temperature: affine units (`degC`) vs. absolute (`K`)~~ — resolved in
  v0.2, see `units.md` §5.
- `break` / `continue`; nested functions and closures.
- Should integer types exist, or only binary64? (Only binary64 so far;
  indices are whole binary64 numbers.)
- ~~Module syntax: `import welding` vs. `use welding.*`~~ — resolved in
  v0.3 (§13): `import name` for C modules, `import "file.mfl"` for files.
- Private names in imported files; qualified access to file imports
  (`import "steel.mfl" as steel`); modules written in MFL and imported by
  name.
- Vector/matrix types with element-wise units (beyond homogeneous arrays).
- A garbage collector for arrays (they live until the end of a run).

## 13. Modules and imports

> **Implemented** (v0.3). Loader: `src/compiler/loader.c`; module
> functions: `src/modules/natives.c` (binding) and `modules/numerical/`
> (numerics). Tests: `tests/language/imports/`,
> `tests/language/conformance_modules.mfl`, `tests/language/errors/*modules*`.

```mfl
import numerical               # a module implemented in C, used as numerical.sum(...)
import "materials/steel.mfl"   # another MFL file: its top-level names become visible
```

### 13.1 Where imports may appear

`import` statements come **first** in a file: only comments, blank lines
and other imports may precede them. They are not allowed inside blocks or
functions. Both rules are syntax errors.

### 13.2 Module imports: `import name`

| Module | Members |
|--------|---------|
| `math` | `sin(x)`, `cos(x)`, `exp(x)`, `log(x)` — `x` dimensionless; `sqrt(x)` — any quantity whose dimension has even exponents, the result has half the exponents (`math.sqrt(16 m^2)` is `4 m`); constant `math.pi` |
| `numerical` | `linspace(start, stop, count)`, `sum(values)`, `mean(values)` (§13.4) |
| `heat` | 1D transient heat conduction (`docs/heat-conduction.md`) |
| `welding` | heat input (EN 1011-1), Rosenthal thick/thin plate fields, t8/5 (closed forms and EN 1011-2), peak temperatures and fused-zone/HAZ widths (`docs/welding.md`, v0.4) |

- Members are always written qualified: `numerical.sum(v)`. The module
  name is visible **only in the file that imports it**; using it elsewhere
  is the error `module 'numerical' is not imported; add 'import numerical'
  at the top of the file`.
- A module name cannot be used as a value, redeclared in that file, or
  imported twice. An unknown module or member is an error that lists the
  available names.
- Module function parameters may be quantities of a fixed dimension,
  absolute temperatures, numbers, arrays or strings (`welding.efficiency("MAG")`,
  v0.4). They accept named arguments (§8.1).
- Calls are checked like calls of annotated functions: before running when
  the argument types are known, when they run otherwise (§4.1), with
  messages such as `argument 2 of 'numerical.linspace' ('stop') must be
  Quantity[m] (like argument 1), found Quantity[s]`.

### 13.3 File imports: `import "path.mfl"`

**Path resolution.** The path is relative to the **directory of the file
that contains the import** (not to the current directory), uses `/` as the
separator on every platform and must end in `.mfl`. Absolute paths,
backslashes, `//` and directory names are rejected. `..` is allowed. Paths
are normalised lexically (`lib/./sub/../steel.mfl` is `lib/steel.mfl`;
symbolic links are not resolved), and the normalised path identifies the
file.

**Loading and execution.** Every file is loaded **once**, however many
files import it. Before any code runs, the files are joined into one
program in dependency order (depth-first, in the order of the import
statements): a file's top-level code runs after the code of everything it
imports, and the main file runs last. Functions are hoisted across files.

**Visibility.** A file may use the top-level names (`let`, `var`, `fn`) of
the files it imports, **directly or through other imports** (imports are
transitive), and never the names of files that import it:
`'main_only' is declared in main.mfl, which this file does not import`.
All top-level names of a program share one namespace, so a name may be
declared only once across all its files
(`'room' is already declared at lib/sub/constants.mfl:1:1`). A `var` can
be assigned only by code in the file that declares it. Module imports
(§13.2) are per file and are not transitive.

**Errors.** Reported at the import statement as `file:line:col: error:`:

| Situation | Message |
|-----------|---------|
| missing or unreadable file | `cannot read the imported file "lib/x.mfl" (looked for 'examples/lib/x.mfl')` |
| cycle (a file imports itself, directly or not) | `import cycle: a.mfl -> b.mfl -> a.mfl (...)` |
| invalid path | `invalid import path "lib": the file name must end in .mfl` |
| more than 64 files, more than 32 nested imports | `too many imported files (limit 64)` |
| embedding API with file imports disabled | `cannot import "...": file imports are not enabled here ...` (`mfl_set_file_imports`) |

Errors and run-time errors inside an imported file name that file:
`lib/fails.mfl:1:33: runtime error: cannot add Quantity[m] and Quantity[s]`.

### 13.4 Numerical operations

| Function | Result | Rules |
|----------|--------|-------|
| `numerical.linspace(start, stop, count)` | `Array` of `count` values of the type of `start` | `stop` must have the same type (dimension and absolute flag) as `start`; `count` a whole number ≥ 2 (at most 10⁷). Values are in the unit of `start`; the first is exactly `start` and the last exactly `stop` converted to that unit; `x_i = start + (stop − start)·i/(count − 1)`. |
| `numerical.sum(values)` | element type | Neumaier compensated summation. If all elements have the same unit, the result keeps it (`[1 mm, 2 mm]` → `3 mm`); otherwise it is in coherent SI (`[1 m, 2 mm]` → `1.002 m`). Absolute temperatures (degC) cannot be added. |
| `numerical.mean(values)` | element type | `sum / len`, same unit rule. Absolute temperatures are allowed (`mean([20 degC, 30 degC])` is `25 degC`). |

**Empty arrays:** `sum` and `mean` of an empty array are run-time errors
(`numerical.sum of an empty array is not defined (its unit and dimension
are unknown)`): an empty array carries no dimension at run time, so there
is no correctly-typed zero to return. Test `len(values) > 0` first.
