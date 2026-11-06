# MFL Architecture — Draft 0

> **Status:** sections 2–4 and 9 are implemented for the v0.1 subset
> (lexer, parser, checker, compiler, VM); sections 5–8 (modules, embedding
> API, WebAssembly, library integration) are still plans.

## 1. Goals and constraints

| Constraint | Decision |
|------------|----------|
| Language | C11, standard library + `libm` only |
| Platforms | Linux, macOS, Windows; WebAssembly later (Emscripten/WASI) |
| Numeric default | IEEE-754 binary64 (`double`) |
| Embeddability | No global state; everything hangs off an `MflState*` |
| Dependencies | No React/JS/Unity/Unreal/graphics engine; no third-party C libs in the core |
| Build | CMake ≥ 3.16 |

## 2. Pipeline

```
source ─▶ Lexer ─▶ tokens ─▶ Parser ─▶ AST ─▶ Resolver/Checker ─▶ Compiler ─▶ Chunk (bytecode)
                                                                                │
                                                                                ▼
                                                                    VM (stack-based) + Runtime
```

| Stage | Directory | Responsibility |
|-------|-----------|----------------|
| Lexer | `src/lexer/` | UTF-8 source → token stream with line/column |
| Parser | `src/parser/` | Pratt/recursive-descent parser → AST; unit-expression sub-grammar |
| Checker | `src/compiler/checker.c` | Name resolution, unit resolution (`src/runtime/units.c`), static types and dimensions; annotates the AST with types and SI scale factors (**implemented**) |
| Compiler | `src/compiler/` | AST → bytecode chunk + constant pool |
| VM | `src/vm/` | Dispatch loop, call frames, value stack |
| Runtime | `src/runtime/` | Values, objects, GC, unit registry, built-ins, module loader |

A full AST (rather than single-pass compilation) is kept on purpose: it
enables static unit checking, gradual-type checking, better error messages
and future tooling (formatter, LSP).

## 3. Value representation

> **Implemented** (v0.1, `src/compiler/chunk.h`). This replaces the first
> draft, which stored every quantity in SI with its dimension.

```c
typedef struct {
    MflValueKind kind;   /* NIL, BOOL, NUMBER, STRING */
    int          unit;   /* NUMBER: index into the chunk's unit table, or none */
    union { double number; int boolean; struct { const char *chars; size_t length; } string; } as;
} MflValue;

typedef struct { double num, den; int exp10; } MflScale;  /* exact: num/den * 10^exp10 */
typedef struct { const char *text; MflScale scale; } MflUnitEntry;
```

- A number stores its magnitude **in its display unit** (`5 mm/s` is `5`
  with unit "mm/s"); arithmetic on different units converts both operands
  to coherent SI first. Printing never re-converts, so literals print as
  written.
- **Dimensions are not stored at run time**: the checker has proven every
  operation consistent (spec §4.0), so the VM does no dimension work.
- *(open question)* rational exponents (e.g. `m^(1/2)`) — v0.1 uses integers.

## 4. Bytecode VM

> **Implemented** (v0.1, `src/compiler/`, `src/vm/`; listing with
> `mfl --bytecode file.mfl`).

- Stack-based, 1-byte opcodes, operands as 16-bit little-endian integers.
- v0.1 opcodes: `CONST, NIL, TRUE, FALSE, POP, GET_GLOBAL, SET_GLOBAL, ADD,
  SUB, MUL, DIV, MOD, POW, NEG, NOT, EQ, NE, LT, LE, GT, GE, JUMP_IF_FALSE,
  JUMP_IF_TRUE, DEFAULT_UNIT, CONVERT, VALUE_IN, PRINT, HALT`. Locals,
  loops and calls (`GET_LOCAL`, `JUMP`, `LOOP`, `CALL`, `RETURN`) come with
  functions and control flow in v0.2.
- Quantity literals compile to a constant holding the literal magnitude
  and its unit. `DEFAULT_UNIT` gives an arithmetic result its coherent SI
  display unit; `CONVERT`/`VALUE_IN` re-express a value in another unit.
- The compiler computes the exact maximum stack depth; the VM allocates the
  stack and globals once, validates every instruction, operand and stack
  access, and reports `MFL_VM_INVALID_CODE` for damaged bytecode instead of
  misbehaving. All output goes through a host-supplied write callback.
- Memory: tracing mark-and-sweep GC for heap objects (strings, arrays,
  closures). Allocation goes through an allocator supplied by the host
  (`MflAllocFn`) so games can control memory.

## 5. Module system

- Scientific modules (`modules/math`, `units`, `numerical`, `welding`) are
  **separate from the core** and register native functions through a small
  native-module API:

```c
typedef MflValue (*MflNativeFn)(MflState *S, int argc, const MflValue *argv);
int mfl_register_module(MflState *S, const char *name,
                        const MflNativeReg *fns, size_t count);
```

- Modules may also be written in MFL itself once functions exist.
- The core binary can be built without any module (minimal embed size).

## 6. Embedding C API (planned)

```c
MflState   *mfl_state_new(const MflConfig *cfg);
void        mfl_state_free(MflState *S);
MflResult   mfl_eval_string(MflState *S, const char *src, const char *chunk_name);
MflResult   mfl_eval_file(MflState *S, const char *path);
int         mfl_set_quantity(MflState *S, const char *name, double v, const char *unit);
int         mfl_get_quantity(MflState *S, const char *name, const char *unit, double *out);
const char *mfl_last_error(const MflState *S);
```

Use case — a game (e.g. *SoldamatGamification*) passes the player's chosen
voltage, current and travel speed, runs a hidden MFL thermal model and reads
back peak temperature or heat input. Players never see MFL code.

Design rules for the API: opaque handles, no global state, no `exit()`
calls, errors returned as codes + message, all strings UTF-8, host-provided
allocator and output callback (`print` can be redirected).

## 7. WebAssembly (future)

The same C sources will compile with Emscripten or WASI-SDK. Requirements
this imposes now: no threads in the core, no direct filesystem dependency
in the VM (I/O goes through host callbacks), no platform-specific headers
outside an isolated `platform` layer.

## 8. Scientific library integration (future)

Optional modules may link BLAS/LAPACK, SUNDIALS, PETSc or Fortran codes via
`ISO_C_BINDING`. These are opt-in CMake options and never required by the
core.

## 9. Error reporting

All errors carry `file:line:column`, a category (`SyntaxError`,
`DimensionError`, `TypeError`, `RuntimeError`) and, for dimension errors,
both dimensions in readable form:

```
heat.mfl:4:18: DimensionError: cannot add V (M·L²·T⁻³·I⁻¹) and A (I)
```
