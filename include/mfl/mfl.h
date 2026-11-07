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
#define MFL_VERSION_MINOR 4
#define MFL_VERSION_PATCH 0
#define MFL_VERSION_STRING "0.4.0-dev"

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
    MFL_ERR_RUNTIME,        /* the program failed while running (v0.2): index out
                               of range, a type or dimension only known at run
                               time, call depth or instruction limit; the message
                               is "name:line:col: runtime error: text"            */
    MFL_ERR_SIMULATION,     /* (v0.3) a simulation cannot run: a parameter is
                               missing, the time step is above the stability
                               limit, or a size limit is exceeded; the message
                               names the parameter                                */
    MFL_ERR_IO              /* (v0.3) a file could not be written (export)        */
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

/* In-memory sources (v0.4): a host that ships its .mfl files inside the
 * program (a game, a web page) adds them here instead of on disk. Paths
 * use '/' and are normalised ("models/./steel.mfl" is "models/steel.mfl").
 * `import "file.mfl"` looks for an added source first (always allowed),
 * then for a file when mfl_set_file_imports(S, 1). The text is copied;
 * adding the same path again replaces it. */
MflStatus mfl_add_source(MflState *S, const char *path, const char *text, size_t length);

/* Removes every added source. */
void mfl_clear_sources(MflState *S);

/* Checks and compiles the added source `path` (v0.4); like
 * mfl_script_compile() with that text and that name. NULL on failure:
 * MFL_ERR_NOT_FOUND when no such source was added. */
MflScript *mfl_script_compile_source(MflState *S, const char *path);

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

/* ------------------------------------------------------------------------ */
/* 1D transient heat conduction (v0.3)                                       */
/* ------------------------------------------------------------------------ */
/*
 * The same solver as the `heat` module of the language and the `mfl`
 * command (docs/heat-conduction.md): a homogeneous bar with constant
 * properties, explicit finite differences on a uniform mesh.
 *
 *     MflHeat1D *h = mfl_heat1d_new(S);
 *     mfl_heat1d_set(h, "length", 100, "mm");
 *     mfl_heat1d_set(h, "conductivity", 45, "W/(m*K)");
 *     mfl_heat1d_set(h, "density", 7850, "kg/m^3");
 *     mfl_heat1d_set(h, "specific_heat", 460, "J/(kg*K)");
 *     mfl_heat1d_set_nodes(h, 21);
 *     mfl_heat1d_set_initial_temperature(h, 20, "degC");
 *     mfl_heat1d_set_boundary_temperature(h, MFL_SIDE_LEFT, 500, "degC");
 *     mfl_heat1d_set_boundary_insulated(h, MFL_SIDE_RIGHT);
 *     mfl_heat1d_set(h, "time_step", 0.5, "s");
 *     mfl_heat1d_set(h, "end_time", 10, "min");
 *     if (mfl_heat1d_run(h) == MFL_OK) { ... read results ... }
 *     mfl_heat1d_free(h);
 *
 * Units: every value comes with an MFL unit expression of the right
 * dimension (MFL_ERR_UNIT otherwise, naming the parameter). Temperatures
 * are ABSOLUTE: "K" or "degC" (and prefixed K such as "mK"); a temperature
 * difference ("delta_degC") is rejected with MFL_ERR_UNIT. Values must be
 * finite and positive (temperatures above 0 K): MFL_ERR_ARGUMENT
 * otherwise. Results can be read in any unit of the right dimension
 * ("degC" for temperatures: absolute conversion).
 *
 * Ownership and lifetime:
 *   - an MflHeat1D created by mfl_heat1d_new() belongs to the host, who
 *     frees it with mfl_heat1d_free() BEFORE freeing its state; errors are
 *     reported through mfl_error_message() of that state;
 *   - the MflHeat1D returned by mfl_state_heat1d() belongs to the state:
 *     never free it; it is valid until the next mfl_eval() /
 *     mfl_script_run() on the state or mfl_state_free();
 *   - a pointer from mfl_heat1d_profile() points into the MflHeat1D and is
 *     valid until the next successful mfl_heat1d_run() on it or until it
 *     is freed (for state-owned results: as above). Setters do not
 *     invalidate results: the results keep the configuration that
 *     produced them.
 *
 * Errors: a failed run (MFL_ERR_SIMULATION, MFL_ERR_MEMORY) keeps the
 * previous results and the configuration; fix the parameter and run
 * again. The state stays usable for other simulations and scripts.
 */

typedef struct MflHeat1D MflHeat1D;

typedef enum { MFL_SIDE_LEFT = 0, MFL_SIDE_RIGHT = 1 } MflSide; /* x = 0, x = L */

/* Creates an empty configuration bound to S (errors, memory limit).
 * Returns NULL if S is NULL or memory runs out. */
MflHeat1D *mfl_heat1d_new(MflState *S);

/* Frees a host-owned simulation (NULL allowed). Must not be called on the
 * pointer returned by mfl_state_heat1d(). */
void mfl_heat1d_free(MflHeat1D *h);

/* Sets a scalar parameter: "length", "conductivity", "density",
 * "specific_heat", "time_step" or "end_time". */
MflStatus mfl_heat1d_set(MflHeat1D *h, const char *parameter, double value, const char *unit);

/* Number of mesh nodes, both ends included (>= 3). Optional when an
 * initial profile is given (its length is then the node count). */
MflStatus mfl_heat1d_set_nodes(MflHeat1D *h, size_t nodes);

/* Samples are taken at t = 0, every `steps` time steps and at the end
 * (default 1: every step). */
MflStatus mfl_heat1d_set_sample_every(MflHeat1D *h, size_t steps);

MflStatus mfl_heat1d_set_initial_temperature(MflHeat1D *h, double temperature, const char *unit);

/* One absolute temperature per node, in node order (copied). */
MflStatus mfl_heat1d_set_initial_profile(MflHeat1D *h, const double *temperatures, size_t count,
                                         const char *unit);

/* Prescribed temperature at one end (applied from t = 0). */
MflStatus mfl_heat1d_set_boundary_temperature(MflHeat1D *h, MflSide side, double temperature,
                                              const char *unit);

/* Insulated end (zero heat flux). */
MflStatus mfl_heat1d_set_boundary_insulated(MflHeat1D *h, MflSide side);

/* Largest stable time step for the current bar and mesh, dx^2 / (2 alpha). */
MflStatus mfl_heat1d_max_time_step(MflHeat1D *h, const char *unit, double *out);

/* Validates the configuration and runs the simulation. */
MflStatus mfl_heat1d_run(MflHeat1D *h);

/* Size of the results (0 before a successful run). */
size_t mfl_heat1d_sample_count(const MflHeat1D *h);
size_t mfl_heat1d_node_count(const MflHeat1D *h);

/* Time of a sample, position of a node, temperature at (sample, node). */
MflStatus mfl_heat1d_time(const MflHeat1D *h, size_t sample, const char *unit, double *out);
MflStatus mfl_heat1d_position(const MflHeat1D *h, size_t node, const char *unit, double *out);
MflStatus mfl_heat1d_temperature(const MflHeat1D *h, size_t sample, size_t node, const char *unit,
                                 double *out);

/* Temperatures in K of one sample (node_count values), or NULL if the
 * sample does not exist. See the lifetime rules above. */
const double *mfl_heat1d_profile(const MflHeat1D *h, size_t sample);

/* Time (in time_unit) of the first sample at which `node` is at or above
 * `temperature`; *reached = 0 (and *time unchanged) if it never is within
 * the simulated time. */
MflStatus mfl_heat1d_first_time_reaching(const MflHeat1D *h, size_t node, double temperature,
                                         const char *unit, const char *time_unit, double *time,
                                         int *reached);

/* Writes the CSV and the JSON metadata (docs/heat-conduction.md, Export):
 * the metadata file is written last and marks a complete export. On
 * failure returns MFL_ERR_IO and leaves no metadata file. */
MflStatus mfl_heat1d_export(const MflHeat1D *h, const char *csv_path, const char *metadata_path);

/* The simulation run by the last successful script on S (its last
 * heat.run()), or NULL if that script ran none. Owned by S. */
const MflHeat1D *mfl_state_heat1d(MflState *S);

#ifdef __cplusplus
}
#endif

#endif /* MFL_MFL_H */
