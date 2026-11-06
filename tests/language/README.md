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

Other folders: `tokens/` (`--tokens`), `ast/` (`--ast`), `check/`
(`--types`), `run/` (example programs and a `--bytecode` listing).
