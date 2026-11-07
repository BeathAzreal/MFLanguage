/*
 * heat_natives.h - the `heat` module of the language (v0.3): functions
 * that build a 1D heat conduction case, run it and read its results.
 * Descriptors are in natives.c; the numerics in modules/numerical/heat1d.c.
 * See docs/heat-conduction.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_HEAT_NATIVES_H
#define MFL_HEAT_NATIVES_H

#include "modules/natives.h"

#define MFL_HEAT_NATIVE_FNS(X)                                                                   \
    X(bar) X(nodes) X(mesh) X(initial_temperature) X(initial_profile) X(left_temperature)        \
    X(right_temperature) X(left_insulated) X(right_insulated) X(time_step) X(end_time)           \
    X(sample_every) X(max_time_step) X(run) X(sample_count) X(times) X(positions) X(profile)     \
    X(final_profile) X(temperature) X(first_time_reaching) X(method) X(left_convection) X(right_convection)

#define MFL_HEAT_DECLARE_(name) int mfl_heat_##name(MflNativeEnv *env, const MflValue *a, MflValue *r);
MFL_HEAT_NATIVE_FNS(MFL_HEAT_DECLARE_)
#undef MFL_HEAT_DECLARE_

#endif /* MFL_HEAT_NATIVES_H */
