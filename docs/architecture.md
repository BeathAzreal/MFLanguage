# MFL Architecture — Draft 0

> **Status:** planned architecture. Only the CLI stub (`src/main.c`) and
> `mfl_version()` exist today.

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
| Checker | `src/compiler/` | Scope resolution, static dimension checks where types are known |
| Compiler | `src/compiler/` | AST → bytecode chunk + constant pool |
| VM | `src/vm/` | Dispatch loop, call frames, value stack |
| Runtime | `src/runtime/` | Values, objects, GC, unit registry, built-ins, module loader |

A full AST (rather than single-pass compilation) is kept on purpose: it
enables static unit checking, gradual-type checking, better error messages
and future tooling (formatter, LSP).

## 3. Value representation

```c
typedef enum { MFL_NIL, MFL_BOOL, MFL_NUMBER, MFL_QUANTITY, MFL_OBJ } MflValueType;

typedef struct {
    int8_t exp[7];          /* SI base exponents: m, kg, s, A, K, mol, cd */
} MflDim;

typedef struct {
    double   value;         /* magnitude in coherent SI units */
    MflDim   dim;
    uint16_t display_unit;  /* index into the unit registry, for printing */
} MflQuantity;
```

- Quantities store the magnitude **in SI base units**; the display unit is
  only for output. `5 mm/s` is stored as `0.005` with dim `L·T⁻¹`.
- Dimension arithmetic is integer addition/subtraction of exponents — cheap
  enough to do at run time on every operation.
- *(open question)* rational exponents (e.g. `m^(1/2)`) — v0.1 uses integers.

## 4. Bytecode VM

- Stack-based, 1-byte opcodes with inline operands.
- Initial opcode set: `CONST, NIL, TRUE, FALSE, POP, GET_LOCAL, SET_LOCAL,
  GET_GLOBAL, DEFINE_GLOBAL, SET_GLOBAL, ADD, SUB, MUL, DIV, POW, NEG, NOT,
  EQ, LT, GT, JUMP, JUMP_IF_FALSE, LOOP, CALL, RETURN, CONVERT, PRINT`.
- Quantity literals compile to a constant (already converted to SI).
- `CONVERT` checks dimension equality and sets the display unit.
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
