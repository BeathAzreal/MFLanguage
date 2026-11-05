/*
 * mfl.h - Public C API of the MFL (Materials & Manufacturing Language).
 *
 * STATUS: pre-alpha scaffold. Only version reporting is implemented.
 * The embedding API sketched in docs/architecture.md (mfl_state_new,
 * mfl_eval, mfl_get_quantity, ...) is PLANNED and not yet declared here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_MFL_H
#define MFL_MFL_H

#ifdef __cplusplus
extern "C" {
#endif

#define MFL_VERSION_MAJOR 0
#define MFL_VERSION_MINOR 0
#define MFL_VERSION_PATCH 1
#define MFL_VERSION_STRING "0.0.1-dev"

/* Returns the library version string (static storage, never NULL). */
const char *mfl_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MFL_MFL_H */
