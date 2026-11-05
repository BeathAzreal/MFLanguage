# Numerical Methods — Draft 0

> **Status:** plan. No solver is implemented yet. The `numerical` module
> will live in `modules/numerical/`, outside the language core.

## 1. Principles

- Default precision: IEEE-754 binary64.
- Every method ships with a **verification test** against an analytical or
  manufactured solution, including an observed order-of-convergence check.
- Every solver call should be able to emit an **experiment record**: method,
  discretisation (Δx, Δt), tolerances, parameters with units, and results.
- Units are checked at the API boundary (e.g. diffusivity must be
  `L²·T⁻¹`); inner loops work on plain SI doubles for speed.

## 2. Governing equation (first target)

Transient heat conduction with constant properties:

```
ρ c_p ∂T/∂t = k ∇²T + q̇
α = k / (ρ c_p)
```

## 3. Planned methods

| Area | Method | Order | Target version |
|------|--------|-------|----------------|
| ODE | Explicit Euler | 1 | v0.3 |
| ODE | Classical RK4 | 4 | v0.3 |
| ODE | Adaptive RK45 (Dormand–Prince) | 4(5) | v0.3+ |
| 1D heat | FTCS (explicit) | O(Δt, Δx²) | v0.3 |
| 1D heat | Backward Euler (implicit) + Thomas algorithm | O(Δt, Δx²) | v0.3 |
| 1D heat | Crank–Nicolson + Thomas algorithm | O(Δt², Δx²) | v0.3 |
| 2D/3D heat | ADI / implicit with iterative solvers (CG, Gauss–Seidel) | 2 | v0.5 |
| Root finding | Bisection, Newton, Brent | — | v0.3 |
| Quadrature | Trapezoid, Simpson, Gauss–Legendre | 2/4/2n | v0.3 |
| Linear algebra | Tridiagonal (Thomas), dense LU | — | v0.3 |

### 3.1 Stability

FTCS in 1D is stable only for the Fourier number
`Fo = α Δt / Δx² ≤ 1/2`. The solver must **check and report** this limit
rather than silently diverge.

### 3.2 Boundary conditions

Dirichlet (prescribed temperature), Neumann (prescribed flux, including
insulated), Robin (convection `-k ∂T/∂n = h (T - T∞)`), and optionally
radiation (linearised).

## 4. Verification strategy

| Test | Reference solution |
|------|--------------------|
| Semi-infinite solid, step surface temperature | `T = T_s + (T_i − T_s)·erf(x / (2√(αt)))` |
| Finite slab, Dirichlet ends | Fourier series solution |
| Steady state with source | Analytical parabola |
| Method of manufactured solutions | Chosen `T(x,t)` with derived source |
| Convergence | Error in L2/L∞ halved grid → expected order ± tolerance |

## 5. Performance

Benchmarks (`benchmarks/`) will compare: MFL-level loop vs. native module
call vs. equivalent C code, for a 1D Crank–Nicolson solve on 10³–10⁶ nodes.

## 6. References

- Incropera, DeWitt, Bergman, Lavine — *Fundamentals of Heat and Mass Transfer*.
- Carslaw & Jaeger — *Conduction of Heat in Solids*.
- LeVeque — *Finite Difference Methods for Ordinary and Partial Differential Equations*.
- Press et al. — *Numerical Recipes*.
- Roache — *Verification and Validation in Computational Science and Engineering*.
