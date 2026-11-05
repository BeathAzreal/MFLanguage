# MFL — Materials & Manufacturing Language

[![CI](https://github.com/BeathAzreal/MFLanguage/actions/workflows/ci.yml/badge.svg)](https://github.com/BeathAzreal/MFLanguage/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**MFL** is a lightweight, cross-platform programming language for scientific
computing, materials engineering and physical simulation. It is designed so
that students, researchers and engineers can write mathematical models, test
hypotheses and run reproducible simulations — for undergraduate research
(IC), final-year projects (TCC), graduate work and industrial applications.

> ⚠️ **Early stage.** MFL is in the initial design and scaffolding phase.
> Most features described below are **planned, not implemented**. Today the
> repository contains the project structure, design documents, a CMake build
> and a CLI stub that only reports its version. See
> [Current status](#current-status).

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

*This is the proposed syntax and the first conformance test of the language.
It is **not executable yet**.*

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
| Lexer, parser and AST | Planned (v0.1) |
| Bytecode compiler | Planned (v0.1) |
| Custom stack-based virtual machine | Planned (v0.1) |
| `double` precision as the default numeric type | Planned (v0.1) |
| Physical units and dimensional analysis (SI base + derived) | Planned (v0.1 core, v0.2 full) |
| Gradual typing (dynamic by default, optional annotations) | Planned (v0.2+) |
| Scientific modules (`math`, `units`, `numerical`, `welding`) independent of the core | Planned |
| Embeddable C API for programs and games | Planned |
| WebAssembly target | Future |
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
games create an interpreter, set parameters, run a model and read results.
Details: [`docs/architecture.md`](docs/architecture.md).

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
├── src/
│   ├── lexer/           Tokenizer (planned)
│   ├── parser/          Parser + AST (planned)
│   ├── compiler/        AST → bytecode (planned)
│   ├── vm/              Bytecode virtual machine (planned)
│   ├── runtime/         Values, units, memory, stdlib glue (planned)
│   ├── main.c           `mfl` CLI driver (stub)
│   └── version.c
├── include/mfl/         Public C API headers
├── modules/             Scientific modules, independent of the core
│   ├── math/  units/  numerical/  welding/
├── examples/            basic/  thermal/  welding/
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

At this stage the `mfl` binary only prints its version/help; running a
script exits with an explicit "not implemented yet" error.

## Roadmap

| Version | Scope |
|---------|-------|
| **v0.0.x** (current) | Repository, documentation, build system, CI |
| **v0.1 — core** | Lexer, parser, AST, bytecode compiler, VM; numbers, strings, `let`, arithmetic, `print`; SI units with dimension checking and `.to()`; heat-input conformance test passing |
| **v0.2** | Functions, control flow, arrays/vectors, gradual type annotations, full unit system, error messages with source locations |
| **v0.3** | `numerical` module: ODE integrators, 1D finite-difference heat solver, verification suite |
| **v0.4** | C embedding API stabilised; experiment records (parameters, units, method, results) |
| **v0.5** | 2D/3D moving heat sources, Goldak model, experimental comparison |
| **later** | WebAssembly build, C/C++/Fortran library bindings, game integration |

## Contributing

Contributions are welcome — especially from students and researchers in
materials, welding, manufacturing and numerical methods.

1. Open an issue describing the bug, proposal or scientific use case.
2. Fork the repository and branch from `dev/mfl-core-v0.1` (core work) or
   `main` (docs).
3. Keep the core in portable C11 with no external dependencies.
4. Add tests: every language feature needs a conformance test
   (`tests/language/*.mfl` + `*.expected`); every numerical method needs a
   verification case against a known solution.
5. Cite the source (book, paper, standard) for any physical model or
   material property.
6. Open a pull request; CI must pass.

## Current status

- ✅ Repository structure and design documents
- ✅ CMake build (C11) and CI workflow (Linux/macOS/Windows)
- ✅ `mfl` CLI stub (`--version`, `--help`)
- ⏳ Lexer, parser, compiler, VM — **not started**
- ⏳ Units and dimensional analysis — **design only**
- ⏳ Numerical and welding modules — **design only**
- ⏳ The example programs in `examples/` **cannot be executed yet**

## License

[MIT](LICENSE)
