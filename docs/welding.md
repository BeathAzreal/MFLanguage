# Welding thermal models (v0.4)

> **Implemented** (v0.4). Numerics: `modules/numerical/welding.c` (plain
> C11, SI doubles). Language: `import welding`
> (`src/modules/welding_natives.c`). Verification:
> `tests/numerical/test_welding.c`. Example:
> `examples/welding/thermal_cycle.mfl`.

These are **analytical** models of the heat flow around a moving arc. They
give heat input, cooling times and peak temperatures of the heat-affected
zone (HAZ) with checked units and cited formulas. They are the bridge
between the heat-input calculation (stage 1 of the validation plan) and
the numerical 2D/3D moving-source simulation (stage 5, a later milestone).

## 1. What the models assume, and where they hold

Rosenthal's solutions (1946) treat the arc as a point source (thick plate,
3-D heat flow) or a line source through the thickness (thin plate, 2-D
heat flow), moving at constant speed on an infinite plate, in a
quasi-steady state, with **constant properties, no latent heat and no
heat lost through the surfaces**.

- **Valid:** the heat-affected zone and the far field, for comparing
  welding conditions, estimating t8/5 and the extent of the HAZ.
- **Not valid:** at and near the arc, where the field is singular (it
  grows without bound towards the source). The models do not predict the
  shape of the weld pool, penetration, the bead or the arc physics, and
  their fused-zone widths are estimates only.
- **Material properties** must be representative of the temperature range
  of interest. For t8/5 of steels, typical values for 500–800 °C are about
  k ≈ 25–35 W/(m·K) and ρc ≈ 4.5–5.5 MJ/(m³·K). Room-temperature values
  change the results noticeably.
- **EN 1011-2 formulas** are empirical fits for steels, which include the
  effect of real properties and losses. Prefer them for steel t8/5.
  Rosenthal's formulas work for any material and also give the
  temperature field.

## 2. Functions (`import welding`)

All functions take named or positional arguments. Temperatures are
absolute (`degC` or `K`). A `delta_degC` value is rejected where an absolute
temperature is expected.

| Function | Result | Formula |
|----------|--------|---------|
| `efficiency(process)` | Number | thermal efficiency factor *k* of EN 1011-1 Table 1 (ISO/TR 17671-1): SAW 1.0; SMAW/MMA, GMAW/MIG/MAG, FCAW 0.8; GTAW/TIG, PAW/plasma 0.6 |
| `heat_input(voltage, current, speed, efficiency)` | J/m (use `.to(kJ/mm)`) | Q = k·U·I / v |
| `t85_thick(heat_input, conductivity, preheat)` | s | t8/5 = Q/(2πk)·[1/(500−T0) − 1/(800−T0)] |
| `t85_thin(heat_input, thickness, conductivity, density, specific_heat, preheat)` | s | t8/5 = Q²/(4πk ρc d²)·[1/(500−T0)² − 1/(800−T0)²] |
| `t85_en1011(heat_input, thickness, preheat)` | s | EN 1011-2 Annex D, 2-D or 3-D by the transition thickness, F2 = F3 = 1 |
| `t85_en1011_joint(heat_input, thickness, preheat, f2, f3)` | s | same, with the joint shape factors of the standard |
| `en1011_transition(heat_input, preheat)` | m | thickness where the 2-D and 3-D formulas agree |
| `temperature_thick(power, speed, conductivity, density, specific_heat, preheat, x, y, z)` | K | T − T0 = q/(2πkR)·exp(−v(x+R)/(2a)) |
| `temperature_thin(power, speed, thickness, conductivity, density, specific_heat, preheat, x, y)` | K | T − T0 = q/(2πkd)·exp(−vx/(2a))·K0(vr/(2a)) |
| `peak_thick(power, speed, conductivity, density, specific_heat, preheat, distance)` | K | maximum of the 3-D field at lateral distance y on the surface |
| `peak_thin(power, speed, thickness, conductivity, density, specific_heat, preheat, distance)` | K | maximum of the 2-D field at lateral distance y |
| `distance_for_peak_thick(..., temperature)` / `distance_for_peak_thin(..., temperature)` | m | lateral distance at which the peak equals a temperature: half-width of the fused zone (melting point) or outer edge of the HAZ (A1 = 727 °C for steels) |

Notation:
- `power` q is the **net** arc power, k·U·I;
- `heat_input` Q = q/v;
- a = k/(ρc) is the diffusivity;
- coordinates move with the arc, which travels in +x and sits at the
  origin; y is across the weld and z is the depth.

The peak temperature is the **exact** maximum of the Rosenthal field along
a line parallel to the weld, found by bisection on the derivative. It is
not Adams' approximate formula.

## 3. Example

```mfl
import welding
let k_arc = welding.efficiency("MAG")                     # 0.8
let Q = welding.heat_input(voltage: 25 V, current: 250 A, speed: 5 mm/s,
                           efficiency: k_arc)             # 1 kJ/mm
print(welding.t85_en1011(heat_input: Q, thickness: 10 mm, preheat: 20 degC))
```

`examples/welding/thermal_cycle.mfl` (a MAG pass on 10 mm steel) prints:

```
heat input: 1 kJ/mm | net power: 5 kW
plate 10 mm is below the transition thickness 14.658605275561131 mm -> 2-D heat flow (thin plate)
t8/5 EN 1011-2 (steel): 11.363566321499013 s
t8/5 Rosenthal thick:   4.250933309078401 s
t8/5 Rosenthal thin:    14.23768467296822 s
peak at 5 mm from the centre line: 938.4618351258315 degC
peak at 8 mm from the centre line: 609.7810967374959 degC
peak at 12 mm from the centre line: 417.5479924559809 degC
peak at 20 mm from the centre line: 259.96959670898786 degC
fused zone half-width: 2.9080258782795396 mm | HAZ width: 3.708824690602919 mm
```

The three t8/5 values differ as expected:
- the 10 mm plate is in the 2-D regime, so the thick-plate formula
  underestimates t8/5;
- Rosenthal's thin-plate formula ignores surface losses, so it gives a
  longer time than the empirical EN 1011-2 fit.

## 4. Verification

`tests/numerical/test_welding.c`, with references independent of the code
(CTest `numerical_welding`; `build/test_welding --table` prints the
tables):

- **Bessel functions** e^x K0 and e^x K1 (Abramowitz & Stegun polynomials)
  against the integral K_n(x) = ∫₀^∞ e^(−x cosh t) cosh(nt) dt, evaluated
  separately: relative error below 3e-7 at x = 0.1 … 20.
- **t8/5 closed forms** against the cooling times measured on the fields
  themselves (bisection for the 800 and 500 °C crossings behind the arc).
  Steel, q = 5 kW, v = 5 mm/s:

  | plate | closed form (s) | from the field (s) | relative difference |
  |---|---|---|---|
  | thick | 4.25093 | 4.25093 | 2.7e-12 |
  | thin, d = 3 mm | 158.956 | 158.956 | 8.5e-07 |
  | thin, d = 6 mm | 39.739 | 39.7384 | 1.4e-05 |
  | thin, d = 9 mm | 17.6618 | 17.6606 | 6.7e-05 |
  | thin, d = 12 mm | 9.93474 | 9.93271 | 2.0e-04 |

  The thin-plate closed form uses the far-field limit of K0, so it
  deviates slightly as the plate gets thicker. The test tolerance is 5e-4.
- **Peak temperatures** against the far-field results (Rykalin): thick
  Tp − T0 = 2Q/(πe ρc y²), thin Tp − T0 = Q/(√(2πe) ρc d y), within 1 %
  at y = 0.6 m. Near the weld the exact peak is lower than these limits:

  | y (mm) | thick peak (°C) | far field | thin (5 mm) peak (°C) | far field |
  |---|---|---|---|---|
  | 2 | 7120.4 | 11730.0 | 4028.7 | 4859.4 |
  | 6 | 1173.4 | 1321.1 | 1577.7 | 1633.1 |
  | 18 | 162.1 | 164.6 | 555.4 | 557.7 |
  | 54 | 36.0 | 36.1 | 199.1 | 199.2 |
  | 162 | 21.8 | 21.8 | 79.7 | 79.7 |

  The peak is also checked to be a maximum of the field, and to decrease
  with the distance. `distance_for_peak` inverts it to within 1e-9.
- **EN 1011-2** against values of the Annex D formulas computed
  separately, including shape factors and continuity at the transition
  thickness.

## 5. Use in a game or simulator

Through the web package (`wasm/README.md`) a host such as
*SoldamatGamification* keeps a model loaded and evaluates it at the end of
each pass:

```ts
const s = mfl.createState();
s.addSource("models/weld.mfl", weldModel);       // uses `import welding`
s.setNumber("U", pass.volts, "V"); s.setNumber("I", pass.amps, "A");
s.setNumber("v", pass.speed, "mm/s"); s.setNumber("d", plate.thickness, "mm");
const weld = s.compile("models/weld.mfl");
weld.run();
report.t85 = s.getNumber("t85", "s");
report.hazWidth = s.getNumber("haz_width", "mm");
```

Recommended first step: show these values **alongside** the game's own
qualitative indices ("shadow mode") before using them for scoring. The
relation between wire feed speed and current is empirical (it depends on
the wire, the gas and the machine) and is not part of this module.

## 6. Not included (later milestones)

- The numerical moving-source simulation in 2D/3D, Goldak's
  double-ellipsoid source, and latent heat and temperature-dependent
  properties.
- Hardness or microstructure prediction (carbon equivalent and t8/5
  correlations such as Yurioka's). These are empirical and need a choice of
  standard and data.
- Comparison with measured thermal cycles (validation, stage 7).

## 7. References

- D. Rosenthal, *The theory of moving sources of heat and its application
  to metal treatments*, Trans. ASME 68 (1946) 849–866.
- N. N. Rykalin, *Berechnung der Wärmevorgänge beim Schweißen*, VEB
  Verlag Technik (1957).
- EN 1011-1:2009, *Welding — Recommendations for welding of metallic
  materials — Part 1: General guidance for arc welding* (Table 1, thermal
  efficiency factors).
- EN 1011-2:2001, *Part 2: Arc welding of ferritic steels*, Annex D
  (cooling time t8/5).
- M. Abramowitz, I. A. Stegun, *Handbook of Mathematical Functions*,
  §9.8 (polynomial approximations of I0, I1, K0, K1).
- S. Kou, *Welding Metallurgy*, 2nd ed., Wiley (2003), chapter 2.
