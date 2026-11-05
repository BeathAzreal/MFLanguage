# Language conformance tests

Each test is a pair of files:

| File | Purpose |
|------|---------|
| `<name>.mfl` | MFL source program |
| `<name>.expected` | Exact expected standard output |

**Status:** these tests are *not yet registered in CTest*, because the
interpreter cannot execute scripts. They will be enabled (via a runner that
executes `mfl <name>.mfl` and diffs stdout with `<name>.expected`) as soon as
the v0.1 core is able to run them.

## Current tests

- `conformance_heat_input` — welding heat input `Q = eta * U * I / v`.
  Expected output: `480 J/mm`. Exercises literals with units, unit
  multiplication/division, dimensionless scalars and unit conversion
  (`W·s/mm → J/mm`).
