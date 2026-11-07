# 1D transient heat conduction — model, method and use (v0.3)

> **Implemented** (v0.3). Solver: `modules/numerical/heat1d.c` (plain C11,
> SI doubles, independent of the parser, compiler and VM). Export:
> `modules/numerical/heat1d_export.c`. Language binding: `import heat`
> (`src/modules/heat_natives.c`). C API: `mfl_heat1d_*` in
> `include/mfl/mfl.h`. The `mfl` command, the `heat` module and the C API
> all call the same solver.

## 1. Scope and limits of this milestone

This milestone ends with **1D heat conduction in a homogeneous bar**. The
example `examples/thermal/bar_heated_end.mfl` must be read as the
**verification of a 1D bar model**, not as a welding simulation: there is
no moving heat source, no heat input from an arc, no phase change, no
temperature-dependent properties, no convection or radiation losses and no
2D/3D geometry. Moving heat sources (Rosenthal) and the 2D/3D welding
simulation (Goldak) are reserved for a later milestone
(`docs/validation-plan.md`, stages 5–6).

## 2. Physical model

```
rho cp dT/dt = k d2T/dx2,     0 <= x <= L,  t >= 0,     alpha = k / (rho cp)
```

| Input | Meaning | Dimension | Example |
|-------|---------|-----------|---------|
| `length` | bar length L | m | `100 mm` |
| `conductivity` | k, constant | W/(m·K) | `45 W/(m*K)` |
| `density` | rho, constant | kg/m³ | `7850 kg/m^3` |
| `specific_heat` | cp, constant | J/(kg·K) | `460 J/(kg*K)` |
| initial condition | uniform T, or one T per node | absolute temperature | `20 degC` |
| left / right end | prescribed temperature, or insulated | absolute temperature | `500 degC` |
| `nodes` | mesh nodes, ends included (≥ 3) | — | `21` |
| `time_step`, `end_time` | Δt and final time | s | `0.5 s`, `10 min` |
| `sample_every` | steps between stored samples (default 1) | — | `20` |

The diffusivity α is computed internally from k, ρ and cp. Each end is
configured independently.

## 3. Method: explicit finite differences (FTCS)

Uniform mesh `x_j = j·Δx`, `j = 0 … N−1`, `Δx = L/(N−1)` (the last node is
exactly `L`). One step of length h, with Fourier number `Fo = α·h/Δx²`:

| Node | Update |
|------|--------|
| interior, `1 ≤ j ≤ N−2` | `T_j' = T_j + Fo·(T_{j−1} − 2T_j + T_{j+1})` |
| insulated left end | `T_0' = T_0 + 2Fo·(T_1 − T_0)` (mirror node `T_{−1} = T_1`, i.e. `dT/dx = 0`) |
| insulated right end | `T_{N−1}' = T_{N−1} + 2Fo·(T_{N−2} − T_{N−1})` |
| prescribed end | `T_0' = T_left` (`T_{N−1}' = T_right`) |

**Initial instant.** At `t = 0` a prescribed boundary temperature
**replaces** the initial value of its end node; the recorded initial state
(sample 0) already shows it. A uniform initial temperature or a profile
applies to all other nodes.

**Energy.** With both ends insulated the scheme conserves exactly (up to
rounding) the discrete energy `E = Δx·(T_0/2 + T_1 + … + T_{N−2} + T_{N−1}/2)`
(per unit area and per unit ρ·cp): the end nodes carry half a cell, and the
updates telescope to zero. Tested to 1e‑13 relative over 20 000 steps.

Accuracy: first order in time and second order in space; with Fo kept
constant while refining, the error falls as Δx² (§11).

## 4. Stability rule

FTCS is stable for **`Fo = α·Δt/Δx² ≤ 1/2`**. The same bound makes every
update coefficient non-negative, including the insulated-end update
(`1 − 2Fo ≥ 0`), so it holds for every combination of boundary conditions.

- **Equality is accepted**: `Δt = Δx²/(2α)` runs. Because Δx, α and a Δt
  given in other units are rounded, the check is
  `Δt ≤ Δx²/(2α) · (1 + 1e‑12)` (`MFL_H1_STABILITY_TOLERANCE`): Δt equal to
  the printed maximum always passes; `Δt = max·(1 + 1e‑9)` is rejected
  (both tested).
- At `Fo = 1/2` exactly the shortest wave of the mesh (alternating ±) is
  multiplied by `1 − 4Fo = −1` per step: it neither grows nor decays. The
  run is stable, but such a component does not damp out; use `Fo < 1/2`
  when the initial data are not smooth.
- An unstable configuration is rejected **before** any step, with the
  requested and the maximum time step:
  `heat.run: unstable time step: time_step = 2 s is above the stability limit of the explicit scheme, time_step <= dx^2 / (2 alpha) = 1.0030555555555556 s (Fourier number alpha*dt/dx^2 = 0.9969537524231514 > 1/2; dx = 0.005 m, alpha = 1.2461921905289394e-5 m^2/s)`.
- `heat.max_time_step()` / `mfl_heat1d_max_time_step()` return the limit
  for the current bar and mesh.

## 5. Time stepping, end time and sampling

- Steps of `time_step` are taken while they fit in `end_time`. If
  `end_time` falls **between steps**, one **shorter last step** of length
  `end_time − n·Δt` ends exactly at `end_time` (it is stable because it is
  shorter). A remainder below `1e‑9·Δt` is treated as rounding and dropped
  (so `1 s / 0.1 s` gives 10 steps, while `0.3 s / 0.1 s`, which is
  2.9999999999999996 in binary, gives 3 steps ending exactly at 0.3 s).
  Example: `Δt = 1 s`, `end_time = 10.5 s` → 10 steps of 1 s and one of
  0.5 s; the final state equals the 10-step state followed by one update
  with `Fo/2` (tested against that hand-written update).
- Sample times are computed as `k·Δt` (no accumulated rounding); the last
  sample time is exactly `end_time`.
- **Sampling**: the initial state, every `sample_every`-th step, and
  **always the final state**. With 10 steps and `sample_every = 4`, the
  samples are at steps 0, 4, 8 and 10. Number of samples:
  `⌊steps/n⌋ + 1`, plus 1 if `n` does not divide `steps`.

## 6. Units and temperatures

All inputs are quantities with units; they are checked for dimension and
converted to SI **before** the solver runs (`100 mm` and `0.1 m`, or
`7.85 g/cm^3` and `7850 kg/m^3`, give bit-identical results — tested).

Temperatures are **absolute**: `degC` or `K`. `20 degC` and `293.15 K`
give identical results. A temperature **difference** is rejected where an
absolute temperature is expected: a literal in `delta_degC` before running
(`argument 1 of 'heat.initial_temperature' ('temperature') must be an
absolute temperature (K or degC), found a temperature difference in
delta_degC`), a value in `delta_degC` when the call runs, and the unit
`"delta_degC"` in the C API (`MFL_ERR_UNIT`). Limitation: a difference
computed as `degC − degC` is displayed in K and cannot be told apart from
an absolute temperature in K; it is accepted as an absolute value.
Absolute temperatures must be above 0 K.

## 7. Input validation and limits

Every input is validated with a message that names the parameter:

| Check | Example message |
|-------|-----------------|
| dimension | `argument 2 of 'heat.bar' ('conductivity') must be Quantity[kg*m/(s^3*K)] (W/(m*K)), found Quantity[kg*m/s^3]` |
| finite | `heat.time_step: time_step (time step) must be finite, found inf s` |
| positive | `heat.bar: conductivity (thermal conductivity k) must be positive, found -45 W/(m*K)` |
| mesh | `nodes ... must be at least 3`; `heat.mesh: the mesh must be uniform ...`; first node at 0, last at L |
| profile | `the initial profile has 5 values, but the mesh has 7 nodes` |
| missing | `heat.run: missing parameter 'right' (boundary condition at x = L)` |
| stability | §4 |
| limits | nodes ≤ 10⁶; steps ≤ 10⁸ per run; stored values (samples × nodes) ≤ 2·10⁷ (`increase sample_every`); the host's memory limit |

## 8. Using it from MFL

```mfl
import numerical
import heat
import "materials/carbon_steel.mfl"            # k_steel, rho_steel, cp_steel

let length = 100 mm
heat.bar(length, k_steel, rho_steel, cp_steel)
heat.mesh(numerical.linspace(0 mm, length, 21))  # or heat.nodes(21)
heat.initial_temperature(20 degC)                # or heat.initial_profile([...])
heat.left_temperature(500 degC)
heat.right_insulated()
heat.time_step(0.5 s)
heat.end_time(10 min)
heat.sample_every(20)
heat.run()
let t = heat.first_time_reaching(50 mm, 150 degC)   # a time, or nil
```

| Function | Result |
|----------|--------|
| `heat.bar(L, k, rho, cp)`, `heat.nodes(n)`, `heat.mesh(x)`, `heat.initial_temperature(T)`, `heat.initial_profile(Ts)`, `heat.left_temperature(T)`, `heat.right_temperature(T)`, `heat.left_insulated()`, `heat.right_insulated()`, `heat.time_step(dt)`, `heat.end_time(t)`, `heat.sample_every(n)` | configure (nil) |
| `heat.max_time_step()` | `Quantity[s]` |
| `heat.run()` | runs; replaces the results of an earlier run |
| `heat.sample_count()`, `heat.times()`, `heat.positions()` | sizes, sample times (s), node positions (m) |
| `heat.profile(i)`, `heat.final_profile()` | `Array[Quantity[K]]` |
| `heat.temperature(x, t)` | temperature at a **node** `x` and a **sample time** `t` (no interpolation; other values are errors) |
| `heat.first_time_reaching(x, T)` | time of the **first sample at or above T** at node x, or `nil` if not reached within `end_time` |

`heat.mesh(x)` requires a uniform mesh from 0 to the bar length (tolerance
1e‑9·Δx) — use `numerical.linspace`. The configuration persists within a
run, so a program can change a parameter and call `heat.run()` again; the
`mfl` command exports the **last** run.

**Threshold times.** `first_time_reaching` returns the time of the first
stored sample whose temperature is ≥ the threshold, **not an interpolated
crossing time**: the true crossing lies between the previous sample and
that one, so its resolution is the sampling interval. The example prints
both samples. A threshold outside the simulated range (e.g. above the hot
end, or not reached by `end_time`) gives `nil`, and the example says so.

## 9. Using it from C

See `include/mfl/mfl.h` (section “1D transient heat conduction”) for the
functions, ownership and lifetime rules, and
`examples/embedding/heat_bar_host.c` (target `mfl_heat_host`), which runs
the CLI case (A) through `mfl_eval` of the `.mfl` file, (B) configured
directly in C, and (C) reads the CLI's CSV, and compares the final profiles.

**Tolerance of the CLI/C comparison: 1e‑9 K** (absolute). The three paths
use the same solver with the same doubles and the CSV stores round-trip
decimals, so the measured difference is **0 K**; the tolerance only allows
for a CSV produced by a different build. All targets are compiled with
`-ffp-contract=off` (MSVC does not contract by default), so results are
bit-identical across Linux, macOS (ARM64) and Windows.

Errors reach the host as `MFL_ERR_SIMULATION` (missing parameter,
unstable step, size limits), `MFL_ERR_ARGUMENT` (non-finite or
non-positive value, invalid call), `MFL_ERR_UNIT`, `MFL_ERR_MEMORY` or
`MFL_ERR_IO`, with the message in `mfl_error_message()`. A failed run keeps
the previous results; the host fixes the parameter and runs again in the
same state (tested, including memory limits from 64 B to 64 KiB).

## 10. Export

```
mfl examples/thermal/bar_heated_end.mfl --csv out/bar_heated_end.csv [--meta out/bar_heated_end.json]
```

(without `--meta`, the metadata goes next to the CSV with `.json`).

**CSV**: header `time_s,position_m,temperature_K`; one row per sample and
node, ordered **by time, then by position**; numbers are the shortest
decimal that reads back to the same double, always with `.` regardless of
the system locale (tested under a decimal-comma locale where available);
lines end in `\n`. Rows = samples × nodes.

**Metadata (JSON)**: format and MFL version; model, method and scope;
units; properties in SI and **as given** (`"100 mm"`, `"20 degC"`);
initial condition (uniform value, or **the full nodal profile** — so a
profile computed or imported from another `.mfl` file is recorded with
its data); boundaries; discretization (nodes, Δx, Δt, end time, steps,
last step, Fourier number, maximum stable step, end-time rule); sampling
(n, samples, rule); CSV file name, columns, order, rows, bytes and FNV‑1a
hash; the source files of the program (main file and every imported file)
with their FNV‑1a hashes, as a reproducible reference.

**Complete vs. interrupted export.** The export first removes an existing
metadata file, writes `<file>.partial` for both files, renames the CSV,
and renames the metadata **last**. Therefore:

- an export is complete **if and only if the metadata file exists**; it
  ends with `"export_complete": true` and its `csv.rows`, `csv.bytes` and
  `csv.fnv1a64` describe the CSV exactly;
- after any failure (missing directory, full disk, failed close or
  rename), `mfl` prints `mfl: export failed: … no metadata file was
  written, so the export is incomplete`, exits with **status 4**, removes
  the temporary files, and no metadata file exists. Tested: a write
  failure at every byte position, failing close and rename, a target that
  is a directory, a missing directory.

## 11. Verification

Registered in CTest; references are computed independently of the solver
and tolerances are declared with the tests.

**Analytical solution** (`tests/numerical/test_heat1d.c`): both ends at
T₀ = 300 K, `T(x,0) = T₀ + A·sin(πx/L)` with A = 50 K, L = 0.1 m, steel
α = 45/(7850·460) m²/s, t up to 60 s;
`T(x,t) = T₀ + A·sin(πx/L)·exp(−απ²t/L²)`. Three meshes, Δt reduced with
Δx² (Fo = 1/4) to keep stability:

| nodes | dx (mm) | dt (s) | max error, all nodes and samples (K) | x = L/4, t = 60 s (K) | x = L/2, t = 60 s (K) | x = L/2, sample nearest t = 30 s (K) | observed order |
|---|---|---|---|---|---|---|---|
| 9 | 12.5 | 3.13455 | 1.140e-01 | 7.927e-02 | 1.121e-01 | 8.501e-02 | – |
| 17 | 6.25 | 0.783637 | 2.834e-02 | 1.988e-02 | 2.812e-02 | 2.044e-02 | 2.009 |
| 33 | 3.125 | 0.195909 | 7.086e-03 | 5.002e-03 | 7.074e-03 | 5.123e-03 | 2.000 |

Tolerances: max error below 0.14, 0.035 and 0.0087 K; observed order
2 ± 0.1. Regenerate the table with `build/test_heat1d --table`. The same
case is also exported **through the CLI** (`tests/language/heat/sine_export.mfl`)
and every CSV row is compared with the formula by an independent checker
(`tests/numerical/check_heat_csv.c`, tolerance 0.035 K; measured
2.834e-2 K).

**Other cases**: insulated ends (uniform stays exactly uniform; energy
conserved; convergence to the mean at Fo = 0.4, neutral alternating mode
at Fo = 1/2), steady state between two prescribed ends (linear profile),
stability equality, end time between steps, sampling, every invalid input,
equivalent units, CLI exit codes, CSV structure, CLI ↔ C host equivalence.

## 12. Reproducing the example

```
cmake -S . -B build && cmake --build build
mkdir -p out
./build/mfl examples/thermal/bar_heated_end.mfl --csv out/bar_heated_end.csv
./build/mfl_heat_host examples/thermal/bar_heated_end.mfl out/bar_heated_end.csv
./build/test_heat1d --table
```

Output of the example (`tests/language/run/bar_heated_end.expected`):

```
stability limit: time_step <= 1.0030555555555556 s ; using 0.5 s
samples: 61 nodes: 21
center reaches 150 degC at the sample t = 90 s | previous sample: 80 s -> 146.86867502991004 degC
center does not reach 600 degC within 600 s | final: 431.77034745218145 degC
final: hot end 500 degC | center 431.77034745218145 degC | insulated end 403.50872401762297 degC
final mean of the nodal temperatures: 439.2295559229285 degC
```

A small exported case is kept in `examples/thermal/output/` (CSV and
metadata of `examples/thermal/small_bar.mfl`).

## 13. References

- Incropera, DeWitt, Bergman, Lavine — *Fundamentals of Heat and Mass
  Transfer* (1D transient conduction; explicit finite differences and the
  Fourier-number limit).
- LeVeque — *Finite Difference Methods for Ordinary and Partial
  Differential Equations* (FTCS stability and convergence).
- Carslaw & Jaeger — *Conduction of Heat in Solids* (separable solutions
  of the slab with fixed end temperatures).
