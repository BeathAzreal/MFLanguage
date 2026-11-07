# MFL — Materials & Manufacturing Language

[![CI](https://github.com/BeathAzreal/MFLanguage/actions/workflows/ci.yml/badge.svg)](https://github.com/BeathAzreal/MFLanguage/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**MFL** is a lightweight, cross-platform programming language for scientific
computing, materials engineering and physical simulation. It is designed so
that students, researchers and engineers can write mathematical models, test
hypotheses and run reproducible simulations — for undergraduate research
(IC), final-year projects (TCC), graduate work and industrial applications.

> ⚠️ **Early stage (pre-alpha, v0.4).** The interpreter runs functions,
> control flow, arrays and gradually typed code with physical units,
> dimensional analysis, absolute temperatures and unit conversion; it has
> modules (`import`), a `numerical` module and a verified **1D transient
> heat conduction solver** with CSV/JSON export, usable from MFL, from the
> `mfl` command and from C. The `welding` module provides analytical
> Rosenthal and EN 1011-1/-2 models, with named arguments and a web
> package for applications. Numerical moving heat sources and Goldak
> remain planned. See [Current status](#current-status).

---

## Why MFL?

Engineering calculations are full of physical quantities. Mixing up `mm` and
`m`, or `W` and `J`, is one of the most common sources of error in student
and industrial models alike. MFL treats **units and dimensional analysis as
first-class language features**, so the following is meant to be a
complete, checked program:

```mfl
let voltage = 20 V
let current = 150 A
let speed = 5 mm/s
let efficiency = 0.8

let energy = efficiency * voltage * current / speed

print(energy.to(J/mm))   # expected: 480 J/mm
```

*This is the first conformance test of the language. It runs and prints
`480 J/mm` (`./build/mfl examples/welding/heat_input.mfl`).*

With v0.2 the same physics becomes a reusable, checked function:

```mfl
fn heat_input(voltage: Quantity[V], current: Quantity[A], speed: Quantity[mm/s],
              efficiency: Number) -> Quantity[J/mm] {
    return efficiency * voltage * current / speed
}

for v in [5 mm/s, 0.005 m/s, 10 mm/s, 0 mm/s] {
    if v == 0 m/s {
        print("v =", v, "-> arc not moving: heat input not defined, skipped")
    } else {
        print("v =", v, "->", heat_input(20 V, 150 A, v, 0.8).to(J/mm))
    }
}
```

```
v = 5 mm/s -> 480 J/mm
v = 0.005 m/s -> 480 J/mm
v = 10 mm/s -> 240 J/mm
v = 0 mm/s -> arc not moving: heat input not defined, skipped
```

Passing a power where the speed is expected is rejected before running:
`argument 3 of 'heat_input' ('speed') must be Quantity[m/s] (declared as
Quantity[mm/s]), found Quantity[kg*m^2/s^3]`. Full example:
[`examples/welding/heat_input_v02.mfl`](examples/welding/heat_input_v02.mfl).

## v0.3: 1D heat conduction in a bar

```mfl
import numerical
import heat
import "materials/carbon_steel.mfl"           # k_steel, rho_steel, cp_steel

let length = 100 mm
heat.bar(length, k_steel, rho_steel, cp_steel)
heat.mesh(numerical.linspace(0 mm, length, 21))
heat.initial_temperature(20 degC)
heat.left_temperature(500 degC)               # x = 0 held at 500 degC
heat.right_insulated()                        # x = L: no heat flux
heat.time_step(0.5 s)                         # stability limit: 1.003 s
heat.end_time(10 min)
heat.sample_every(20)
heat.run()
print(heat.first_time_reaching(50 mm, 150 degC))   # 90 s (first sample at or above)
```

```bash
./build/mfl examples/thermal/bar_heated_end.mfl --csv out/bar_heated_end.csv
./build/mfl_heat_host examples/thermal/bar_heated_end.mfl out/bar_heated_end.csv
```

The first command prints the report and writes the temperature history
(`time_s,position_m,temperature_K`) and a JSON metadata file (version,
properties, initial and boundary conditions, discretization, sampling,
units, source files); the second runs the same case from a C host and
checks it gives the same numbers. Model, method (explicit finite
differences, stability `α·Δt/Δx² ≤ 1/2`), units, limits and the
verification against the analytical solution (observed order 2.00) are in
[`docs/heat-conduction.md`](docs/heat-conduction.md); a small exported case
is in [`examples/thermal/output/`](examples/thermal/output/).

> **Scope.** This milestone ends with 1D conduction in a homogeneous bar.
> The example is the verification of a bar model, **not a welding
> simulation**: moving heat sources and 2D/3D welding models are reserved
> for a later milestone.

## v0.4: named arguments and welding thermal models

```mfl
import welding

let Q = welding.heat_input(voltage: 25 V, current: 250 A, speed: 5 mm/s,
                           efficiency: welding.efficiency("MAG"))      # 1 kJ/mm
print(welding.en1011_transition(heat_input: Q, preheat: 20 degC).to(mm))   # 14.66 mm -> 2-D
print(welding.t85_en1011(heat_input: Q, thickness: 10 mm, preheat: 20 degC)) # 11.36 s
print(welding.peak_thin(power: 5 kW, speed: 5 mm/s, thickness: 10 mm,
                        conductivity: 30 W/(m*K), density: 7850 kg/m^3,
                        specific_heat: 640 J/(kg*K), preheat: 20 degC,
                        distance: 8 mm).to(degC))                        # 609.8 degC
```

- **Named arguments** for functions and module functions, checked before
  running (`'f' has no parameter 'c' (its parameters are: a, b)`).
- **`welding` module**: EN 1011-1 efficiency factors and heat input;
  Rosenthal thick- and thin-plate temperature fields; t8/5 in closed form
  and by EN 1011-2 (with the 2-D/3-D transition thickness); exact peak
  temperatures and the distance at which a peak is reached (fused-zone
  half-width, HAZ width). Formulas, limits and the verification (t8/5
  closed forms against the cooling measured on the fields, peaks against
  the far-field results) are in [`docs/welding.md`](docs/welding.md).
  Full example: [`examples/welding/thermal_cycle.mfl`](examples/welding/thermal_cycle.mfl).
- **Web package** `mfl-wasm` (ESM, CommonJS, TypeScript types): a host
  keeps a model loaded, sets named inputs with units, compiles once and
  runs many times, and reads named results in any unit
  ([`wasm/README.md`](wasm/README.md)). The same in C: `mfl_add_source`
  and `mfl_script_compile_source` ([`docs/embedding.md`](docs/embedding.md)).

> **Scope.** These are analytical models (constant properties, no surface
> losses, singular at the arc). They estimate the heat-affected zone and
> cooling times; they do not predict the weld pool, penetration or the
> microstructure. The numerical moving-source simulation is a later
> milestone.

## Scientific goals

- Provide a small, auditable language for **mathematical modelling** of
  materials and manufacturing processes.
- Make **dimensional consistency** a compile-time / run-time guarantee
  rather than a convention.
- Enable **reproducible computational experiments**: every run should be
  able to record parameters, units, numerical methods and results.
- Offer **validated numerical solvers** (starting with transient heat
  conduction) verified against analytical solutions and experimental data.
- Be **embeddable**: educational games such as *SoldamatGamification* and
  *Oficina de Materiais* can run MFL simulations behind the scenes, without
  players ever seeing or writing MFL.

## Planned features

| Feature | Status |
|---------|--------|
| Lexer | **Implemented** — `mfl --tokens` |
| Parser and AST | **Implemented** — `mfl --ast` |
| Bytecode compiler | **Implemented** — `mfl --bytecode` |
| Custom stack-based virtual machine | **Implemented** — `mfl file.mfl` |
| `double` precision as the default numeric type | **Implemented** (IEEE 754 binary64, locale-independent I/O) |
| Physical units and dimensional analysis (SI base + derived) | **Implemented**: checked before running where types are known (`mfl --types`), while running otherwise; exact unit conversion |
| Absolute temperatures (`degC`) vs. differences (`K`, `delta_degC`) | **Implemented** (v0.2) |
| Functions, `if`/`else`, `while`, `for`, blocks and scopes | **Implemented** (v0.2) |
| Arrays (homogeneous, quantities in any unit of one dimension) | **Implemented** (v0.2) |
| Gradual typing (optional annotations, run-time checks for the rest) | **Implemented** (v0.2) |
| Error messages with `file:line:col`, at compile time and run time | **Implemented** |
| Modules: `import "file.mfl"` and `import math` / `numerical` / `heat` | **Implemented** (v0.3) |
| `numerical` module: uniform meshes, sum, mean (dimension-checked) | **Implemented** (v0.3) |
| 1D transient heat conduction (FTCS), CSV/JSON export, CLI and C API | **Implemented** (v0.3) — [`docs/heat-conduction.md`](docs/heat-conduction.md) |
| `welding` module: analytical Rosenthal fields, EN 1011-1/-2, t8/5 and HAZ width | **Implemented** (v0.4) — [`docs/welding.md`](docs/welding.md) |
| Embeddable C API for programs and games | **Implemented** — [`docs/embedding.md`](docs/embedding.md) |
| WebAssembly target (runs in the browser, same results as the CLI) | **Implemented** — [`wasm/README.md`](wasm/README.md) |
| Bindings to C/C++/Fortran scientific libraries | Future |

MFL has **no dependency** on React, JavaScript, Unity, Unreal or any game or
graphics engine. The core is plain **C11** with only the C standard library.

## Proposed architecture

```
 source.mfl
     │
     ▼
 ┌────────┐   tokens   ┌────────┐   AST   ┌──────────┐ bytecode ┌──────┐
 │ Lexer  │──────────▶│ Parser │───────▶│ Compiler │─────────▶│  VM  │
 └────────┘            └────────┘         └──────────┘          └──┬───┘
                                    (unit / type checks)            │
                                                                    ▼
                                         ┌───────────────────────────────┐
                                         │ Runtime: values, quantities,  │
                                         │ units, GC, standard library   │
                                         └───────────────┬───────────────┘
                                                         │ native module API
                          ┌──────────────┬───────────────┼──────────────┐
                          ▼              ▼               ▼              ▼
                     modules/math  modules/units  modules/numerical  modules/welding
```

The host-facing **C API** (`include/mfl/mfl.h`) lets other programs and
games create an interpreter, set parameters with units, run a model and
read results in any compatible unit. Details:
[`docs/architecture.md`](docs/architecture.md) and
[`docs/embedding.md`](docs/embedding.md).

```c
MflState *S = mfl_state_new(NULL);
mfl_set_number(S, "voltage", 20, "V");
mfl_set_number(S, "current", 150, "A");
mfl_set_number(S, "speed", 5, "mm/s");
mfl_eval(S, model, strlen(model), "heat_input.mfl");
mfl_get_number(S, "energy", "kJ/mm", &q);    /* q = 0.48 */
```

## First validation case: welding

The first scientific domain used to validate MFL is **arc welding**,
specifically **heat transfer in MIG/MAG (GMAW)** processes. The validation
path is incremental:

1. Welding heat input (`Q = η·U·I / v`).
2. Transient heat conduction equation.
3. 1D numerical solver (finite differences).
4. Verification against analytical solutions.
5. Moving heat source in 2D and 3D (Rosenthal-type solutions).
6. Goldak double-ellipsoid heat source model.
7. Comparison with experimental data (thermocouples, macrographs).
8. Integration with *SoldamatGamification*.

See [`docs/validation-plan.md`](docs/validation-plan.md).

## Project structure

```
MFLanguage/
├── .github/workflows/   CI (build + smoke tests)
├── docs/                Language spec, architecture, units, numerics, validation
├── research/            Research log for IC/TCC (Portuguese): diary, ADRs, AI-use statement, references
├── src/
│   ├── lexer/           Tokenizer (implemented)
│   ├── parser/          Parser + AST (implemented)
│   ├── compiler/        Semantic checker, bytecode compiler (implemented)
│   ├── vm/              Bytecode virtual machine (implemented)
│   ├── api/             Public embedding API implementation
│   ├── modules/         Language binding of the C modules (math, numerical, heat)
│   ├── runtime/         Arena, unit registry and dimensions; values, stdlib glue (planned)
│   ├── main.c           `mfl` command-line driver
│   └── version.c
├── include/mfl/         Public C API headers
├── modules/             Scientific modules, independent of the core
│   ├── numerical/       Numerical library (vector ops, 1D heat solver, export) — plain C11
│   ├── math/  units/  welding/
├── examples/            basic/  thermal/  welding/  embedding/
├── tests/               language/  units/  numerical/
├── benchmarks/
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## Building

Requirements: a C11 compiler (GCC, Clang or MSVC) and CMake ≥ 3.16.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/mfl --version
```

Run a program:

```bash
./build/mfl examples/welding/heat_input.mfl       # prints: 480 J/mm
./build/mfl examples/thermal/steel_properties.mfl
./build/mfl examples/welding/heat_input_v02.mfl   # v0.2 demonstration
./build/mfl examples/thermal/bar_heated_end.mfl --csv out/bar_heated_end.csv   # v0.3
```

`mfl` exit codes: 0 success, 1 errors in the program (compile-time or
run-time, solver errors included) or bad usage, 3 internal failure, 4 the
export files could not be written. Binary packages for Windows, Linux and
macOS (executable, examples, header, libraries, instructions) are built by
CI as workflow artifacts.

Inspect the stages: `--tokens` (lexer), `--ast` (parser), `--types`
(inferred types and dimensions, functions included), `--bytecode`
(compiled code). The language is described in
[`docs/language-spec.md`](docs/language-spec.md).

## Roadmap

| Version | Scope |
|---------|-------|
| **v0.0.x** ✅ | Repository, documentation, build system, CI |
| **v0.1 — core** ✅ | Lexer, parser, AST, bytecode compiler, VM; numbers, strings, `let`, arithmetic, `print`; SI units with dimension checking and `.to()`; heat-input conformance test passing; C embedding API |
| **v0.2** ✅ | Functions, control flow, arrays, gradual type annotations, absolute temperatures, error messages with source locations at compile and run time |
| **v0.3** ✅ | `import`; `numerical` module; 1D transient heat conduction (explicit FD) verified against the analytical solution; CSV/JSON export; C API for simulations; packages; WebAssembly build |
| **v0.4** ✅ (this release) | Named arguments; `welding` module (EN 1011-1/-2, Rosenthal fields, t8/5, peak temperatures, HAZ width) with verification; in-memory sources and the `mfl-wasm` web package for applications |
| **v0.4.x** | Implicit schemes (backward Euler, Crank–Nicolson), convection boundaries, ODE integrators; experiment records (parameters, units, method, results) |
| **v0.5** | 2D/3D moving heat sources, Goldak model, experimental comparison |
| **later** | C/C++/Fortran library bindings, game integration |

## Contributing

Contributions are welcome — especially from students and researchers in
materials, welding, manufacturing and numerical methods.

1. Open an issue describing the bug, proposal or scientific use case.
2. Fork the repository and branch from the current development branch
   (`dev/mfl-core-v0.1` at the time of v0.2) or `main` (docs).
3. Keep the core in portable C11 with no external dependencies.
4. Add tests: every language feature needs a conformance test
   (`tests/language/*.mfl` + `*.expected`); every numerical method needs a
   verification case against a known solution.
5. Cite the source (book, paper, standard) for any physical model or
   material property.
6. Open a pull request; CI must pass.

## Current status

- ✅ Repository structure, design documents, CMake build (C11), CI on Linux (GCC, Clang), macOS (Clang) and Windows (MSVC)
- ✅ `mfl` CLI: run a program, or inspect `--tokens`, `--ast`, `--types`, `--bytecode`
- ✅ Lexer, parser and AST with error recovery
- ✅ Semantic checker: names and scopes, unit registry, static dimensional analysis, gradual typing
- ✅ Bytecode compiler and VM: functions (recursion, functions as values), `if`/`while`/`for`, arrays, run-time checks with positions, call depth limit
- ✅ Units: SI base and derived, prefixes, min/h/L/bar/percent, absolute temperatures in degC, exact conversions
- ✅ C embedding API (`include/mfl/mfl.h`, `docs/embedding.md`), usable from C and C++, with run-time errors and an instruction limit
- ✅ Conformance tests: `480 J/mm` (v0.1) and the v0.2 acceptance demonstration
- ✅ Modules: `import "file.mfl"` (each file once, cycles and missing files reported) and C modules `math`, `numerical`, `heat`, `welding`
- ✅ 1D transient heat conduction: FTCS with stability check, CSV/JSON export (atomic, with completion marker), same solver from MFL, the CLI and C
- ✅ CI on Linux (GCC, Clang), macOS, Windows with warnings as errors; ASan + UBSan; packages with a smoke test
- ⏳ `break`/`continue`, closures, a garbage collector for arrays
- ✅ Named arguments; `welding` module with analytical thermal models (Rosenthal, EN 1011-1/-2), verified (`docs/welding.md`)
- ✅ WebAssembly build and the `mfl-wasm` npm package (ESM/CJS, TypeScript types, state API)
- ⏳ Implicit schemes, numerical moving heat sources (2D/3D, Goldak) — **design only**

## License

[MIT](LICENSE)
