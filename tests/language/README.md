# Language conformance tests

Each test is a pair of files:

| File | Purpose |
|------|---------|
| `<name>.mfl` | MFL source program |
| `<name>.expected` | Exact expected standard output |

**Status:** every `conformance_*.mfl` file in this folder is registered in
CTest automatically (see `CMakeLists.txt`): the test runs `mfl <name>.mfl`
and requires stdout to equal `<name>.expected` exactly and stderr to be
empty.

## Current tests

- `conformance_heat_input` — welding heat input `Q = eta * U * I / v`.
  Expected output: `480 J/mm` (file unchanged since it was written on
  2026-11-05, before the interpreter existed). Exercises literals with
  units, unit multiplication/division, dimensionless scalars and unit
  conversion (`W·s/mm → J/mm`).
- `conformance_display` — display-unit rules and conversions
  (`docs/units.md` §4, §6).
- `conformance_logic` — number formatting, IEEE edge cases, comparisons,
  short-circuit logic, strings and variables (spec §9).

- v0.2: `conformance_functions`, `conformance_control`,
  `conformance_arrays`, `conformance_types` (annotations and gradual
  typing), `conformance_temperature` (absolute vs. difference).

Other folders:

| Folder | Content |
|--------|---------|
| `tokens/`, `ast/`, `check/` | `--tokens`, `--ast`, `--types` golden outputs |
| `run/` | example programs' output and a `--bytecode` listing |
| `errors/` | v0.2 programs that must fail: `compile_v02.mfl` (22 compile-time errors) and one file per run-time error (`rt_*.mfl`), each with `.stdout` and `.stderr`; all registered automatically |
| `acceptance/` | v0.2 acceptance cases: power passed as speed, return value incompatible with the signature, index out of range |
