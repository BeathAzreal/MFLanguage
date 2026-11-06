# MFL Architecture — Draft 0

> **Status:** sections 2–6 and 9 are implemented (v0.3: lexer, parser,
> loader, checker, compiler, VM, C modules, numerical library, embedding
> API); section 7 (WebAssembly) is implemented too; section 8 (library integration) is a plan.

## 1. Goals and constraints

| Constraint | Decision |
|------------|----------|
| Language | C11, standard library + `libm` only |
| Platforms | Linux, macOS, Windows; WebAssembly (wasm32-wasi, browser and Node) |
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

> **Implemented** (`src/compiler/chunk.h`). v0.2 added the dimension at
> run time, arrays and functions.

```c
typedef struct {
    MflValueKind  kind;   /* NIL BOOL NUMBER STRING ARRAY FUNCTION (UNSET: global not yet set) */
    int           unit;   /* NUMBER: index into the chunk's unit table, or none (SI) */
    MflDim        dim;    /* NUMBER: 7 SI exponents */
    unsigned char affine; /* NUMBER: absolute temperature (degC) */
    union { double number; int boolean; struct { const char *chars; size_t length; } string;
            MflArray *array; int function; } as;
} MflValue;

typedef struct { double num, den; int exp10; } MflScale;  /* exact: num/den * 10^exp10 */
typedef struct { const char *text; MflScale scale; MflDim dim; unsigned char affine;
                 double offset; } MflUnitEntry;           /* offset: 273.15 for degC */
```

- A number stores its magnitude **in its display unit** (`5 mm/s` is `5`
  with unit "mm/s"); arithmetic on different units converts both operands
  to coherent SI first. Printing never re-converts, so literals print as
  written; a result without a unit prints with the SI name of its dimension.
- **Dimensions are stored at run time** (since v0.2): with gradual typing
  some operations can only be checked when they run (spec §4.1). For fully
  annotated or inferred code the checks never fail, because the checker
  already proved them.
- Arrays are heap objects owned by the run (freed when it ends).
- *(open question)* rational exponents (e.g. `m^(1/2)`) — integers only.

## 4. Bytecode VM

> **Implemented** (`src/compiler/`, `src/vm/`; listing with
> `mfl --bytecode file.mfl`).

- Stack-based, 1-byte opcodes, 0–2 operands of 16 bits (little-endian).
  Every instruction records the source line and column it came from.
- Opcodes (v0.2): `CONST NIL TRUE FALSE POP GET_GLOBAL SET_GLOBAL GET_LOCAL
  SET_LOCAL ADD SUB MUL DIV MOD POW NEG NOT EQ NE LT LE GT GE JUMP
  JUMP_IF_FALSE JUMP_IF_TRUE LOOP CONVERT VALUE_IN CALL RETURN PRINT LEN PUSH
  ARRAY INDEX SET_INDEX CHECK_TYPE CHECK_LIKE_GLOBAL CHECK_LIKE_LOCAL HALT`.
  (`DEFAULT_UNIT` of v0.1 was removed: results print from their dimension.)
- Layout: the main program first, ending in `HALT`, then each function.
  A call places the callee and its arguments on the value stack; the
  arguments become the first local slots of the new frame. The compiler
  computes each frame's maximum stack depth; the VM grows the value stack
  and the frame array on demand, up to 1000 nested calls (a run-time error
  beyond that) and within the host's memory limit. Calls do not recurse in
  C, so deep MFL recursion cannot overflow the C stack.
- Gradual typing: the compiler emits `CHECK_TYPE` (against a type
  descriptor) or `CHECK_LIKE_*` (same type as the variable's current value)
  where the checker returned `MFL_COMPAT_RUNTIME`; every operation also
  re-checks its operands.
- Run-time errors carry the source position of the failing instruction.
  An optional instruction limit stops endless loops (embedding API).
- The VM validates every instruction, operand and stack access, and
  reports `MFL_VM_INVALID_CODE` for damaged bytecode instead of
  misbehaving. All output goes through a host-supplied write callback.
- Memory (planned): a garbage collector for arrays and a host-supplied
  allocator (`MflAllocFn`). Today arrays live until the end of a run.

## 5. Module system

> **Implemented** (v0.3). Spec: `docs/language-spec.md` §13.

```
main.mfl ─▶ Loader (src/compiler/loader.c) ─▶ one program, files in dependency order
                │  import "file.mfl": resolve, read once, detect cycles
                ▼
            Checker: per-file visibility, `import name` of C modules,
                     calls typed from native descriptors
                ▼
            Compiler: CALL_NATIVE f n ─▶ VM ─▶ src/modules/natives.c (binding)
                                                   │ SI doubles only
                                                   ▼
                                     modules/numerical/ (library mfl_numerical)
                                     vector.c · heat1d.c · heat1d_export.c
```

- **Three layers.** `modules/numerical/` is plain C11 on SI doubles (no
  parser, compiler or VM types; built as its own static library and tested
  on its own). `src/modules/` is the binding: a constant descriptor table
  (parameter kinds and dimensions, result kind) used by the checker before
  running and re-checked on the values at run time, plus the conversion
  of values to SI. The VM only knows the opcode `CALL_NATIVE`.
- **State.** C modules have no global state. The `heat` module keeps its
  configuration and last results in an `MflNativeSession` owned by the
  caller of the VM (the `mfl` command or the embedding API) and passed to
  `mfl_vm_execute_ex`; the caller reads the results after the run.
- **Files.** Every statement and instruction records its file, so
  diagnostics and run-time errors name the imported file.
- **Reproducibility.** The core is compiled with `-ffp-contract=off` (no
  FMA contraction), so the solver gives bit-identical results on x86-64
  and ARM64.
- Not yet: modules written in MFL and imported by name; a host API to
  register new C modules.

## 6. Embedding C API

> **Implemented** (v0.1 subset). Reference and usage guide:
> [`embedding.md`](embedding.md); header: `include/mfl/mfl.h`;
> implementation: `src/api/mfl_api.c`.

```c
MflState  *mfl_state_new(const MflConfig *cfg);          /* output callback, memory limit */
void       mfl_state_free(MflState *S);
MflStatus  mfl_set_number(MflState *S, const char *name, double v, const char *unit);
MflStatus  mfl_eval(MflState *S, const char *src, size_t len, const char *name);
MflScript *mfl_script_compile(MflState *S, const char *src, size_t len, const char *name);
MflStatus  mfl_script_run(MflState *S, MflScript *script);
MflStatus  mfl_get_number(MflState *S, const char *name, const char *unit, double *out);
const char *mfl_error_message(const MflState *S);
```

Use case: a game (e.g. *SoldamatGamification*) passes the player's chosen
voltage, current and travel speed, runs a hidden MFL model and reads back
the heat input. Players never see MFL code.

Differences from the first draft of this section: `mfl_set_quantity` and
`mfl_get_quantity` became `mfl_set_number`/`mfl_get_number` (with a unit
argument); `mfl_eval_file` was dropped (the host reads files itself);
compile-once/run-many scripts were added for interactive use; a
host-supplied allocator is still pending (only a memory limit exists).

Design rules: opaque handles, no global state, no `exit()` calls, errors
returned as codes + message, all strings UTF-8, output callback for
`print`. Internally, host inputs are declared to the checker before the
script (so dimensional analysis knows them) and enter the VM as the first
global slots; after a run, every global is copied back to the state.

## 7. WebAssembly

> **Implemented** (v0.3): `wasm/` (build script, loader, tests) and
> `src/wasm/mfl_wasm.c`. See [`wasm/README.md`](../wasm/README.md).

The core builds unchanged for `wasm32-wasi` (reactor). The VM never
touches files, so the browser build needs no WASI functions at all: files
for `import` come from an in-memory file system, and output goes to one
imported JavaScript function. The design constraints below are why this
worked without changes to the interpreter.

### Original plan

The same C sources will compile with Emscripten or WASI-SDK. Requirements
this imposes now: no threads in the core, no direct filesystem dependency
in the VM (I/O goes through host callbacks), no platform-specific headers
outside an isolated `platform` layer.

## 8. Scientific library integration (future)

Optional modules may link BLAS/LAPACK, SUNDIALS, PETSc or Fortran codes via
`ISO_C_BINDING`. These are opt-in CMake options and never required by the
core.

## 9. Error reporting

> **Implemented.** (The first draft proposed named categories such as
> `DimensionError`; the implementation uses two kinds, distinguished by
> when they are found.)

Every error carries `file:line:column` and a readable message; dimensions
are written in SI base units:

```
heat.mfl:4:18: error: cannot add Quantity[kg*m^2/(s^3*A)] and Quantity[A]: dimensions differ
heat.mfl:7:7: runtime error: index 3 is out of range for an array of length 3 (valid indices: 0 to 2)
```

- `error:` — found before running (syntax, names, types, dimensions); all
  of them are reported, sorted by position, up to 50.
- `runtime error:` — found while running (v0.2: values of type `Any`,
  indices, call depth, ...); the run stops at the first one.

The CLI exits with status 1 in both cases; the embedding API returns
`MFL_ERR_PROGRAM` or `MFL_ERR_RUNTIME` with the same text.
