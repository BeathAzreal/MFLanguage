# Scientific Validation Plan — Draft 0

> **Status:** stage 1 (heat input, v0.1/v0.2) and the 1D part of stages 3–4
> (explicit solver verified against the analytical slab solution, v0.3,
> `heat-conduction.md` §11) are done. The other stages are plans.

MFL's first scientific domain is **arc welding**, specifically heat transfer
in **MIG/MAG (GMAW)**. Validation follows the usual distinction:

- **Verification** — are we solving the equations correctly? (code vs.
  analytical/manufactured solutions)
- **Validation** — are we solving the right equations? (model vs.
  experimental measurements)

## Stage 1 — Welding heat input

```
Q = η · U · I / v
```

| Symbol | Meaning | Typical unit |
|--------|---------|--------------|
| η | arc thermal efficiency (GMAW ≈ 0.7–0.85) | — |
| U | arc voltage | V |
| I | welding current | A |
| v | travel speed | mm/s |
| Q | net heat input per unit length | J/mm or kJ/mm |

**Acceptance:** conformance test `tests/language/conformance_heat_input`
prints exactly `480 J/mm`; additional cases compared with hand calculation
(relative error < 1e-12).

## Stage 2 — Transient heat conduction equation

Express `ρ c_p ∂T/∂t = k ∇²T + q̇` with unit-checked material properties
(e.g. structural steel: k ≈ 45 W/(m·K), ρ ≈ 7850 kg/m³, c_p ≈ 460 J/(kg·K)).
**Acceptance:** dimensional consistency enforced by the language; wrong
units produce `DimensionError`.

## Stage 3 — 1D numerical solver

FTCS, implicit Euler and Crank–Nicolson (see `numerical-methods.md`).
**Acceptance:** runs with Dirichlet/Neumann/Robin BCs; stability limit
reported for FTCS.

## Stage 4 — Verification against analytical solutions

Semi-infinite solid (erf solution), finite slab (Fourier series),
manufactured solutions. **Acceptance:** observed order of convergence within
±0.1 of theoretical; L∞ error below documented tolerance.

## Stage 5 — Moving heat source in 2D and 3D

Rosenthal quasi-steady solutions for thin plates (2D line source) and thick
plates (3D point source) as analytical references; numerical moving-source
solver on structured grids. **Acceptance:** agreement with Rosenthal far from
the source (where its singularity is irrelevant).

> **Status (v0.4).** The analytical references are done: the `welding`
> module computes both Rosenthal fields, t8/5 (closed forms and EN 1011-2),
> exact peak temperatures and fused-zone/HAZ widths, verified in
> `tests/numerical/test_welding.c` (`docs/welding.md` §4). The numerical
> moving-source solver is not implemented yet.

## Stage 6 — Goldak double-ellipsoid model

Volumetric heat source with front/rear ellipsoid parameters
(`a_f, a_r, b, c, f_f, f_r`, with `f_f + f_r = 2`). **Acceptance:** integral
of the source equals `η U I` (energy conservation check); parametric study
reproducing trends reported in the literature.

## Stage 7 — Comparison with experimental data

- Thermal cycles from thermocouples (peak temperature, t₈/₅ cooling time).
- Fusion zone and HAZ dimensions from macrographs.
- Data sets stored under `examples/welding/data/` with provenance
  (source, date, equipment, uncertainty, licence).

**Acceptance:** documented error metrics (e.g. peak temperature, t₈/₅,
bead width) with stated uncertainty; no tuning on the same data used for
validation without saying so.

## Stage 8 — Integration with SoldamatGamification

Expose the validated models through the C embedding API so the game can run
them behind the scenes. **Acceptance:** game passes parameters and reads
results without players writing MFL; same inputs produce identical outputs
in the game and in the CLI.

## Reproducibility requirements

Every validation run must record:

1. MFL version and git commit.
2. Input parameters **with units**.
3. Numerical method, grid/time step, tolerances.
4. Material property sources.
5. Results and error metrics.
6. Platform/compiler information.

## References

- Goldak, Chakravarti, Bibby (1984), *A new finite element model for welding
  heat sources*, Metallurgical Transactions B 15, 299–305.
- Rosenthal (1946), *The theory of moving sources of heat and its
  application to metal treatments*, Trans. ASME 68, 849–866.
- Kou — *Welding Metallurgy*.
- Lancaster — *The Physics of Welding*.
- ISO/TR 17671 / EN 1011-1 — heat input definitions and thermal efficiency
  factors for arc processes.
