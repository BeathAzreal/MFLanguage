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

## 3. Unit registry for v0.1

> **Implemented** (`src/runtime/units.c`), with the differences noted
> below: prefixes also apply to `L`; `degC` and `%` are not available yet.

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
| degC | affine, see §5 — **not available in v0.1** (error: use K) |
| % | 0.01 (dimensionless) — **not available in v0.1** (`%` is the remainder operator) |

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
without `.to()` shows as `N`. Use `.to(J/mm)` to choose the unit.

## 5. Temperature *(open question)*

Absolute (`K`) and affine (`degC`) temperatures behave differently:
`20 degC + 10 degC` is ambiguous (point + point). Draft proposal:

- `degC` literals create an **absolute** temperature stored in K
  (`20 degC → 293.15 K`).
- A separate unit `delta_degC` (= 1 K, no offset) is used for differences.
- Subtracting two absolute temperatures yields a temperature difference.
- Adding two absolute temperatures is a `DimensionError`-class warning.

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

## 7. Planned tests (`tests/units/`)

- Each derived unit reduces to the expected base dimension.
- Each prefix scales correctly.
- `1 kJ/mm == 1000 J/mm`, `1 m/min == 16.666… mm/s`.
- Adding incompatible dimensions raises `DimensionError` with the correct
  message.
- Heat input conformance test (`480 J/mm`).
