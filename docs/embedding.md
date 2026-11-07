# Embedding MFL in a host program

> **Status:** implemented (v0.3; compatible with v0.1 and v0.2 hosts). Public header:
> [`include/mfl/mfl.h`](../include/mfl/mfl.h). Complete example:
> [`examples/embedding/heat_input_host.c`](../examples/embedding/heat_input_host.c).

MFL is designed to run physical models **behind the scenes** of another
program, such as an educational game (*SoldamatGamification*, *Oficina de
Materiais*). The player changes parameters in the game; the game passes them
to an MFL model as quantities with units, runs it and reads back results in
the units its interface needs. Players never see MFL code.

## 1. Building and linking

The core is a static library, `mfl_core`, built with CMake. It is written in
C11 and needs only the C standard library and `libm`.

```cmake
add_subdirectory(MFLanguage)          # or FetchContent / ExternalProject
target_link_libraries(my_game PRIVATE mfl_core)
```

Include only `mfl/mfl.h`; everything else is internal. The header compiles
as C and as C++ (`extern "C"`), which is checked by the `api_cpp` test.

## 2. Workflow

```c
#include "mfl/mfl.h"

MflState *S = mfl_state_new(NULL);            /* print() -> stdout */

/* 1. Inputs, with units. They exist before compiling: the model's
 *    dimensional analysis uses their dimensions.                      */
mfl_set_number(S, "voltage", 20, "V");
mfl_set_number(S, "current", 150, "A");
mfl_set_number(S, "speed",   5, "mm/s");

/* 2. Compile the model once. */
MflScript *model = mfl_script_compile(S, src, strlen(src), "heat_input.mfl");
if (model == NULL) { show_error(mfl_error_message(S)); }

/* 3. Every time the player changes something: new values, run, read. */
mfl_set_number(S, "speed", player_speed_mm_s, "mm/s");
if (mfl_script_run(S, model) == MFL_OK) {
    double q;
    mfl_get_number(S, "energy", "kJ/mm", &q);  /* any unit of the right dimension */
}

mfl_script_free(model);
mfl_state_free(S);
```

`mfl_eval(S, src, len, name)` does compile + run + free in one call, for
models that run only once.

## 3. Rules

| Topic | Behaviour |
|-------|-----------|
| Inputs | `mfl_set_number(S, name, value, unit)`. `unit` is an MFL unit expression (`"V"`, `"mm/s"`, `"W/(m*K)"`); `NULL`/`""` = plain number. Inside the script an input is an immutable variable (like `let`); redeclaring or assigning it is a program error. |
| Changing inputs | Values may change freely between runs. The unit may change if the dimension stays the same (`"mm/s"` → `"m/min"`). If a script's input changed dimension or was removed, `mfl_script_run` returns `MFL_ERR_INPUTS_CHANGED`: compile the script again. |
| Results | After a successful run, every input and top-level `let`/`var` is readable with `mfl_get_number(S, name, unit, &out)`. The unit must have the variable's dimension (`MFL_ERR_UNIT` otherwise); `NULL` returns the value in coherent SI. A failed run keeps the previous results. |
| Errors | Every function returns an `MflStatus`. `mfl_error_message(S)` describes the last failure; program errors use one line each, `name:line:col: error: text`, the same format as the `mfl` command. Nothing calls `exit()` or `abort()`. |
| Output | `print()` goes to `MflConfig.output(user, text, length)`; default: stdout. |
| Memory | `MflConfig.memory_limit` caps each compilation stage and each run, in bytes (0 = no limit). Exceeding it returns `MFL_ERR_MEMORY` cleanly; this is tested for every limit from 1 to 5000 bytes. |
| Run-time errors (v0.2) | A model can fail while running (array index out of range, a type or dimension only known at run time, call depth, instruction limit): `MFL_ERR_RUNTIME`, message `name:line:col: runtime error: text`. Output printed before the error has already reached the host. The previous results stay readable and the state stays usable: change the inputs and run again. |
| Endless loops (v0.2) | `mfl_set_instruction_limit(S, n)` stops a run after `n` VM instructions with `MFL_ERR_RUNTIME` (0 = no limit, the default). Recommended for interactive hosts such as games. |
| Temperatures | Inputs and outputs use K (an absolute temperature in degC cannot cross the API: `MFL_ERR_UNIT`). Inside a script, `(T_input).to(degC)` gives the Celsius value. |
| Threads | No global state: different `MflState`s may be used from different threads at the same time. One state must not be used by two threads at once. A compiled script may be run on any state that provides its inputs. |
| Locale | Numbers are read and printed with `.` whatever the host's locale (tested under pt_BR). |

## 4. Status codes

| Code | Meaning |
|------|---------|
| `MFL_OK` | success |
| `MFL_ERR_ARGUMENT` | NULL pointer, invalid input name (must be an ASCII identifier, not a keyword or `print`) |
| `MFL_ERR_MEMORY` | out of memory or over `memory_limit` |
| `MFL_ERR_PROGRAM` | the source has errors (syntax, names, units, dimensions) |
| `MFL_ERR_UNIT` | unknown or malformed unit text, or a dimension mismatch in `mfl_get_number` |
| `MFL_ERR_NOT_FOUND` | no such variable in the last run |
| `MFL_ERR_TYPE` | the variable is not a number (a `Bool`, `String` or `Nil`) |
| `MFL_ERR_INPUTS_CHANGED` | recompile the script |
| `MFL_ERR_NOT_RUN` | nothing has run successfully yet |
| `MFL_ERR_INTERNAL` | a bug in MFL; please report it |
| `MFL_ERR_RUNTIME` | the program failed while running (v0.2; appended at the end of the enum, so existing values are unchanged) |
| `MFL_ERR_SIMULATION` | (v0.3) a simulation cannot run: missing parameter, unstable time step, size limit |
| `MFL_ERR_IO` | (v0.3) an export file could not be written; no metadata file is left |

## 5. Example output

`examples/embedding/heat_input_host.c` (built as `mfl_embed_example`, and
its output is checked by the `run_embed_example` test):

```
setting                 U [V]  I [A]  v [mm/s]     P [kW]    Q [kJ/mm]
MIG/MAG, thin sheet      18.0  120.0       8.0      1.728        0.216
MIG/MAG, conformance     20.0  150.0       5.0      2.400        0.480
MIG/MAG, thick plate     28.0  280.0       4.0      6.272        1.568
```

## 6. Imports and simulations (v0.3)

- `import numerical`, `import math`, `import heat` work in every script.
  `import "file.mfl"` is **disabled by default** in the API (a host
  running untrusted scripts never opens files); enable it with
  `mfl_set_file_imports(S, 1)`. Paths are then resolved relative to the
  directory of the `source_name` passed to `mfl_eval` /
  `mfl_script_compile`, exactly like the `mfl` command. Errors in imported
  files name those files.
- **1D heat conduction from C**: `mfl_heat1d_new`, `mfl_heat1d_set(h,
  "length", 100, "mm")`, ..., `mfl_heat1d_run`, then read results by
  sample and node (`mfl_heat1d_time`, `mfl_heat1d_position`,
  `mfl_heat1d_temperature`, `mfl_heat1d_profile`,
  `mfl_heat1d_first_time_reaching`) and `mfl_heat1d_export`. A script's
  last `heat.run()` is available as `mfl_state_heat1d(S)`.
- **Ownership**: `mfl_heat1d_new` objects belong to the host (free them
  with `mfl_heat1d_free` before the state); `mfl_state_heat1d` results
  belong to the state and are valid until its next run; profile pointers
  are valid until the next successful run of their object or its
  destruction. Details in the header and in `docs/heat-conduction.md` §9.
- Example: `examples/embedding/heat_bar_host.c` (`mfl_heat_host`) runs the
  CLI case three ways and checks they agree within 1e‑9 K (measured 0 K).
- Fix in v0.3: a top-level degC variable read with `mfl_get_number(S,
  name, "K", &v)` now includes the 273.15 K offset.

## 7. Sources held in memory (v0.4)

A host that ships its models inside the application (a game, a web page)
gives the state the files instead of letting it open them:

```c
mfl_add_source(S, "models/weld.mfl", weld_text, strlen(weld_text));
mfl_add_source(S, "models/steel.mfl", steel_text, strlen(steel_text)); /* imported by weld.mfl */
MflScript *weld = mfl_script_compile_source(S, "models/weld.mfl");
mfl_set_number(S, "U", 25, "V");
mfl_script_run(S, weld);           /* run again after changing the inputs */
mfl_get_number(S, "t85", "s", &t85);
```

- Paths are normalised (`a/./b/../c.mfl` is `a/c.mfl`); adding a path again
  replaces its text. `mfl_clear_sources(S)` removes them all.
- `import "file.mfl"` in a script compiled from a source looks among the
  sources first and, only if `mfl_set_file_imports(S, 1)` was called, on
  disk. Without file imports enabled, nothing is ever read from disk.
- Text is copied: the host may free its buffers after `mfl_add_source`.
- In the browser the same API is `mfl.createState()` of the `mfl-wasm`
  package (`addSource`, `setNumber`, `compile(path).run()`, `getNumber`);
  see `wasm/README.md`.

## 8. Not available yet

- A host-supplied allocator (only a memory limit for now).
- Strings, booleans and arrays as inputs/outputs through the API (scripts
  can use them internally).
- Calling MFL functions from C, or C functions from MFL (MFL functions
  exist since v0.2, but only scripts can call them).
- Reusing results between runs other than through inputs.
