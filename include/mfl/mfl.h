/*
 * mfl.h - Public C API of MFL (Materials & Manufacturing Language).
 *
 * Embeds the MFL interpreter in another program, for example an
 * educational game that runs a physical model behind the scenes:
 *
 *     MflState *S = mfl_state_new(NULL);
 *     mfl_set_number(S, "voltage", 20, "V");
 *     mfl_set_number(S, "current", 150, "A");
 *     mfl_set_number(S, "speed", 5, "mm/s");
 *     if (mfl_eval(S, model, strlen(model), "heat_input.mfl") == MFL_OK) {
 *         double q;
 *         mfl_get_number(S, "energy", "kJ/mm", &q);      // q == 0.48
 *     } else {
 *         fputs(mfl_error_message(S), stderr);          // file:line:col: error: ...
 *     }
 *     mfl_state_free(S);
 *
 * Rules: opaque handles, no global state (states are independent, so
 * different threads may use different states), no exit() or abort(),
 * every failure is a status code plus a message, strings are UTF-8,
 * print() output goes to a host callback.
 *
 * STATUS: pre-alpha (v0.3). Inputs and outputs are numbers and quantities
 * (scripts themselves may use functions, arrays, ...). Not yet available:
 * a host-supplied allocator (only a memory limit), strings/booleans/arrays
 * through the API, calling MFL functions from C.
 *
 * After any error (including MFL_ERR_RUNTIME) the state stays usable: the
 * host may change inputs and run again.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_MFL_H
#define MFL_MFL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFL_VERSION_MAJOR 0
#define MFL_VERSION_MINOR 3
#define MFL_VERSION_PATCH 0
#define MFL_VERSION_STRING "0.3.0-dev"

/* Returns the library version string (static storage, never NULL). */
const char *mfl_version(void);

/* ------------------------------------------------------------------------ */
/* Types                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct MflState MflState;   /* inputs, results of the last run, errors */
typedef struct MflScript MflScript; /* a compiled program, runnable many times */

typedef enum {
    MFL_OK = 0,
    MFL_ERR_ARGUMENT,       /* NULL pointer, invalid name, ...                    */
    MFL_ERR_MEMORY,         /* out of memory or over the configured limit         */
    MFL_ERR_PROGRAM,        /* the MFL source has errors (syntax, names, units,
                               dimensions); see mfl_error_message()               */
    MFL_ERR_UNIT,           /* unknown unit text, or dimension does not match     */
    MFL_ERR_NOT_FOUND,      /* no such variable in the last run                   */
    MFL_ERR_TYPE,           /* the variable is not a number                       */
    MFL_ERR_INPUTS_CHANGED, /* a script's inputs were removed or changed dimension
                               since it was compiled; compile it again            */
    MFL_ERR_NOT_RUN,        /* mfl_get_number() before any successful run         */
    MFL_ERR_INTERNAL,       /* should not happen; please report it                */
    MFL_ERR_RUNTIME         /* the program failed while running (v0.2): index out
                               of range, a type or dimension only known at run
                               time, call depth or instruction limit; the message
                               is "name:line:col: runtime error: text"            */
} MflStatus;

/* Receives print() output. `text` is not NUL-terminated. */
typedef void (*MflOutputFn)(void *user, const char *text, size_t length);

typedef struct {
    MflOutputFn output;       /* NULL: write to stdout */
    void       *user;         /* passed to output */
    size_t      memory_limit; /* bytes per compilation stage and per run; 0 = no limit */
} MflConfig;

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

/* Creates a state. config may be NULL (stdout, no memory limit); it is
 * copied. Returns NULL only when memory runs out. */
MflState *mfl_state_new(const MflConfig *config);

/* Frees the state. Scripts compiled with it must be freed separately
 * (before or after). NULL is allowed. */
void mfl_state_free(MflState *S);

/* Message of the last failed call on S ("" if the last call succeeded).
 * Program errors use one line per error: "name:line:col: error: text".
 * Valid until the next call on S. Never NULL for a non-NULL S. */
const char *mfl_error_message(const MflState *S);

/* Short name of a status, e.g. "MFL_ERR_UNIT". */
const char *mfl_status_name(MflStatus status);

/* ------------------------------------------------------------------------ */
/* Inputs and results                                                        */
/* ------------------------------------------------------------------------ */

/* Defines (or replaces) a host input visible to scripts as an immutable
 * variable. unit is an MFL unit expression such as "V", "mm/s",
 * "W/(m*K)"; NULL or "" means a plain dimensionless number. The script's
 * dimensional analysis uses the input's dimension. */
MflStatus mfl_set_number(MflState *S, const char *name, double value, const char *unit);

/* Removes all inputs. */
void mfl_clear_inputs(MflState *S);

/* Allows or forbids `import "file.mfl"` in scripts compiled on S (v0.3).
 * Default: forbidden (0), so a host that runs scripts from untrusted
 * sources never lets them open files. When allowed, paths are resolved
 * relative to the directory of the script's source_name, with fopen(),
 * exactly like the `mfl` command. Module imports (`import numerical`) are
 * always allowed. */
void mfl_set_file_imports(MflState *S, int enabled);

/* Limits the number of VM instructions per run (0 = no limit, the
 * default). A run that exceeds it fails with MFL_ERR_RUNTIME, so an
 * endless loop in a model cannot freeze the host (v0.2). */
void mfl_set_instruction_limit(MflState *S, unsigned long long max_instructions);

/* Reads a variable (an input or a top-level 'let'/'var') as it was at the
 * end of the last successful run on S, expressed in `unit`. The unit must
 * have the variable's dimension (else MFL_ERR_UNIT). unit NULL or "" gives
 * the value in coherent SI units (for a dimensionless number: the number). */
MflStatus mfl_get_number(MflState *S, const char *name, const char *unit, double *out);

/* ------------------------------------------------------------------------ */
/* Running programs                                                          */
/* ------------------------------------------------------------------------ */

/* Checks, compiles and runs source[0 .. length) with the current inputs.
 * source_name is used in error messages (NULL: "<script>"). */
MflStatus mfl_eval(MflState *S, const char *source, size_t length, const char *source_name);

/* Checks and compiles once against the current inputs (names and
 * dimensions; values may change later). Returns NULL on failure, with
 * the reason in mfl_error_message(S). The script copies the source. */
MflScript *mfl_script_compile(MflState *S, const char *source, size_t length,
                              const char *source_name);

/* Runs a compiled script with the current input values. */
MflStatus mfl_script_run(MflState *S, MflScript *script);

/* Frees a script. NULL is allowed. */
void mfl_script_free(MflScript *script);

#ifdef __cplusplus
}
#endif

#endif /* MFL_MFL_H */
