# Physical Units and Dimensional Analysis — Draft 0

> **Status:** design only. No unit support is implemented yet.

## 1. Model

Every quantity has a **dimension**, a vector of integer exponents over the
seven SI base dimensions:

| Index | Dimension | Symbol | SI base unit |
|-------|-----------|--------|--------------|
| 0 | length | L | m |
| 1 | mass | M | kg |
| 2 | time | T | s |
| 3 | electric current | I | A |
| 4 | thermodynamic temperature | Θ | K |
| 5 | amount of substance | N | mol |
| 6 | luminous intensity | J | cd |

A unit is `(scale, offset, dimension)` relative to coherent SI. For all
units except affine temperatures, `offset = 0`.

## 2. Rules

| Operation | Rule |
|-----------|------|
| `a + b`, `a - b` | `dim(a) == dim(b)`, result in SI |
| comparisons | `dim(a) == dim(b)` |
| `a * b` | `dim = dim(a) + dim(b)` |
| `a / b` | `dim = dim(a) - dim(b)` |
| `a ^ n` | `n` dimensionless and constant; `dim = n · dim(a)` |
| `sin, exp, log, …` | argument must be dimensionless (radians count as dimensionless) |
| `.to(u)` | `dim(a) == dim(u)` else `DimensionError` |

## 3. Unit registry (v0.1, extended in v0.2)

> **Implemented** (`src/runtime/units.c`). v0.2 adds `degC`, `delta_degC`
> and `percent`. Differences from the first draft: prefixes also apply to
> `L`; the percent unit is spelled `percent` (`%` stays the remainder
> operator).

**Base:** `m, kg, s, A, K, mol, cd`, plus `g`.

**Derived (SI, coherent):**

| Symbol | Name | Definition |
|--------|------|------------|
| N | newton | kg·m·s⁻² |
| Pa | pascal | N·m⁻² |
| J | joule | N·m |
| W | watt | J·s⁻¹ |
| C | coulomb | A·s |
| V | volt | W·A⁻¹ |
| Ω (`ohm`) | ohm | V·A⁻¹ |
| Hz | hertz | s⁻¹ |

**Prefixes:** `p n u m c d k M G` (write `u` for micro: `µ` is not allowed
in v0.1 code), applicable to every SI symbol above except `kg` (use `g`:
`mg`, `ug`), and to `L` (`mL`). Not to `min`, `h`, `bar`. One prefix only
(`mmm` is unknown). Ambiguities are resolved by preferring an exact
registered symbol first (`min` = minute, not milli-inch; `cd` = candela).
Symbols are case-sensitive (`Pa` pascal, `pA` picoampere).

**Non-SI accepted in welding/materials:**

| Symbol | Definition |
|--------|------------|
| min | 60 s |
| h | 3600 s |
| L | 10⁻³ m³ |
| bar | 10⁵ Pa |
| degC | absolute temperature on the Celsius scale: T[K] = T[degC] + 273.15 (§5); no prefixes; only on its own |
| delta_degC | temperature difference, = 1 K (§5) |
| percent | 0.01 (dimensionless); `%` is the remainder operator |
| degF, delta_degF | reserved: reported as "not supported" (use K or degC) |

Common welding units are expressible directly: `kJ/mm`, `J/mm`, `mm/s`,
`m/min`, `L/min` (shielding gas flow), `W/(m*K)` (conductivity),
`J/(kg*K)` (specific heat), `kg/m^3` (density), `m^2/s` (diffusivity).

## 4. Display units

> **Implemented** (v0.1). Values are stored as a magnitude **in their
> display unit**, so `print(0.1 mm)` prints exactly `0.1 mm`.

Results keep a *display unit* chosen as follows:

1. If created by a literal or `.to(u)`, the display unit is `u` (written in
   normalised form: `W/(m*K)`, `kg/m^3`, `m^-2`).
2. `+`, `-`, `%` and unary `-` on operands **with the same display unit**
   keep it: `1 mm + 2 mm` prints `3 mm`. *(Addition to the original draft.)*
3. Otherwise the result is shown in coherent SI: a registered SI symbol
   when the dimension matches exactly one (`W`, `V`, `Pa`, `J`, `N`, …),
   else the base form (`m^2`, `m/s`, `kg/m^3`).

Note: J/m and N have the same dimension, so an energy per length printed
without `.to()` shows as `N`. Use `.to(J/mm)` to choose the unit. A
temperature difference without a unit of its own prints in `K`;
`delta_degC` is shown only after `.to(delta_degC)`.

## 5. Temperature

> **Implemented** (v0.2). Tests: `tests/language/conformance_temperature.mfl`,
> `tests/language/errors/rt_affine.mfl`, `errors/compile_v02.mfl`.

A temperature can mean two different things, and MFL keeps them apart:

| Kind | Units | Meaning |
|------|-------|---------|
| **Absolute temperature** | `degC` | a point on the Celsius scale; stored with its offset: 20 degC = 293.15 K |
| **Temperature difference** | `delta_degC`, `K` | an interval; 1 delta_degC = 1 K, no offset |

Kelvin values have no offset, so `K` serves both as an absolute
temperature and as a difference. A value in `degC` is *absolute*; its type
is `Quantity[degC]` (see the language spec §4).

| Operation | Result | Example |
|-----------|--------|---------|
| absolute − absolute | difference (printed in K) | `80 degC - 20 degC` → `60 K` |
| absolute ± difference | absolute (keeps degC) | `20 degC + 5 K` → `25 degC`; `100 degC - 30 K` → `70 degC` |
| difference + absolute | absolute | `5 K + 20 degC` → `25 degC` |
| absolute + absolute | **error** | `cannot add two absolute temperatures (degC + degC)` |
| difference − absolute | **error** | |
| `*`, `/`, `^`, `%`, unary `-` on an absolute | **error** (`… convert it with .to(K) first`) | `20 degC * 2` |
| comparison | absolute values compared in K | `27 degC > 300 K` → `true` |
| `.to(K)` / `.value_in(K)` from degC | absolute conversion (adds 273.15) | `(20 degC).to(K)` → `293.15 K` |
| `.to(degC)` from K | absolute conversion (subtracts 273.15) | `(273.15 K).to(degC)` → `0 degC` |
| `.to(delta_degC)` from a difference | no offset | `(80 degC - 20 degC).to(delta_degC)` → `60 delta_degC` |

`degC` cannot appear inside compound units (`degC/s` is an error: use `K/s`
or `delta_degC/s`) and takes no prefix. Through the embedding API, inputs
and outputs use K: an absolute temperature in degC cannot be passed in.

Because 273.15 is not exact in binary, conversions with the offset are
correctly rounded IEEE operations, not exact decimals:
`(300 K).value_in(degC)` is `26.850000000000023`.

## 6. Precision

> **Implemented** (v0.1).

Magnitudes are binary64. A unit's factor to SI is kept **exactly** as
`num / den × 10^k` (integers `num`, `den`; prefixes and decimal units only
change `k`). Converting between units applies the integer part and then
the power of ten as one exact operation, so pure prefix conversions
(`mm → m`, `g/cm^3 → kg/m^3`, `kJ/mm → J/mm`) are correctly rounded:
`(1 g/cm^3).to(kg/m^3)` prints exactly `1000 kg/m^3`.

Measured (exact rational reference, 4000 random sums of up to five
quantities in mixed units, converted to a random target): worst relative
error 5.0e-16 (2.3 ulp).

## 7. Tests

- Registry, prefixes, ambiguities, exact scales, dimension formatting:
  `tests/unit/test_checker.c`.
- Display rules and conversions: `tests/language/conformance_display.mfl`.
- Temperatures (absolute and difference, tested separately):
  `tests/language/conformance_temperature.mfl`.
- Conversion accuracy against exact rational arithmetic (worst 2.3 ulp)
  and the heat-input conformance test (`480 J/mm`).
