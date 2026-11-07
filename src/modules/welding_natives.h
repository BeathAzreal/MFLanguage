/*
 * welding_natives.h - the `welding` module of the language (v0.4):
 * arc efficiency, heat input, Rosenthal fields, t8/5 and peak
 * temperatures. Descriptors in natives.c; numerics in
 * modules/numerical/welding.c; documentation in docs/welding.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_WELDING_NATIVES_H
#define MFL_WELDING_NATIVES_H

#include "modules/natives.h"

#define MFL_WELDING_NATIVE_FNS(X)                                                                \
    X(efficiency) X(heat_input) X(t85_thick) X(t85_thin) X(t85_en1011) X(t85_en1011_joint)       \
    X(en1011_transition) X(temperature_thick) X(temperature_thin) X(peak_thick) X(peak_thin)     \
    X(distance_for_peak_thick) X(distance_for_peak_thin)

#define MFL_WELDING_DECLARE_(name) int mfl_welding_##name(MflNativeEnv *env, const MflValue *a, MflValue *r);
MFL_WELDING_NATIVE_FNS(MFL_WELDING_DECLARE_)
#undef MFL_WELDING_DECLARE_

#endif /* MFL_WELDING_NATIVES_H */
