/*
 * heat1d.h - transient heat conduction in a homogeneous 1D bar with
 * constant properties, FTCS or backward Euler on a uniform
 * mesh. See docs/heat-conduction.md for the model, the schemes and
 * its limits.
 *
 *     rho cp dT/dt = k d2T/dx2,   0 <= x <= L,   alpha = k / (rho cp)
 *
 * Everything here is in coherent SI (m, s, K, W/(m*K), kg/m^3,
 * J/(kg*K)) and in plain doubles: units and dimensions are checked by the
 * callers (the `heat` module of the language and the embedding API), which
 * convert the user's values before calling. Temperatures are absolute
 * (K, > 0).
 *
 * This file knows nothing about the parser, the compiler or the VM. Every
 * function reports errors as a status plus a message that names the
 * parameter at fault.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NUMERICAL_HEAT1D_H
#define MFL_NUMERICAL_HEAT1D_H

#include <stddef.h>

/* Limits (configurations beyond them are rejected before allocating). */
#define MFL_H1_MAX_NODES 1000000u
#define MFL_H1_MAX_STEPS 100000000u      /* time steps per run */
#define MFL_H1_MAX_VALUES 20000000u      /* stored temperatures: samples x nodes */
#define MFL_H1_STABILITY_TOLERANCE 1e-12 /* relative tolerance of Fo <= 1/2 */
#define MFL_H1_STEP_TOLERANCE 1e-9       /* end_time remainders below this x dt are dropped */

typedef enum { MFL_H1_LEFT = 0, MFL_H1_RIGHT = 1 } MflH1Side;

typedef enum {
    MFL_H1_BC_UNSET = 0,
    MFL_H1_BC_TEMPERATURE, /* prescribed temperature (Dirichlet) */
    MFL_H1_BC_INSULATED,   /* zero heat flux (Neumann, dT/dx = 0) */
    MFL_H1_BC_CONVECTION   /* outward flux h (T - T_ambient), Robin */
} MflH1Boundary;

typedef enum { MFL_H1_FTCS = 0, MFL_H1_BACKWARD_EULER } MflH1Method;

typedef enum { MFL_H1_INIT_UNSET = 0, MFL_H1_INIT_UNIFORM, MFL_H1_INIT_PROFILE } MflH1Initial;

/* Parameters (bit numbers of MflH1Config.set and indices of `given`). */
typedef enum {
    MFL_H1_P_LENGTH,
    MFL_H1_P_CONDUCTIVITY,
    MFL_H1_P_DENSITY,
    MFL_H1_P_SPECIFIC_HEAT,
    MFL_H1_P_NODES,
    MFL_H1_P_INITIAL,
    MFL_H1_P_LEFT,
    MFL_H1_P_RIGHT,
    MFL_H1_P_TIME_STEP,
    MFL_H1_P_END_TIME,
    MFL_H1_P_SAMPLE_EVERY,
    MFL_H1_P_COUNT_
} MflH1Param;

typedef enum {
    MFL_H1_OK = 0,
    MFL_H1_INVALID,  /* a value is out of its domain, or a parameter is missing */
    MFL_H1_UNSTABLE, /* time_step above the stability limit of the scheme */
    MFL_H1_LIMIT,    /* nodes, steps or stored values above the limits */
    MFL_H1_NO_MEMORY /* allocation failed or over the memory limit */
} MflH1Status;

#define MFL_H1_GIVEN 64

typedef struct {
    double        length;        /* m */
    double        conductivity;  /* W/(m*K) */
    double        density;       /* kg/m^3 */
    double        specific_heat; /* J/(kg*K) */
    size_t        nodes;         /* 0 = not set (taken from the profile) */
    MflH1Initial  initial;
    double        initial_uniform; /* K */
    double       *initial_profile; /* K, owned (malloc), initial_count values */
    size_t        initial_count;
    MflH1Boundary boundary[2];
    double        boundary_temperature[2]; /* K */
    double        boundary_coefficient[2]; /* W/(m^2*K), convection only */
    MflH1Method   method; /* zero/default is FTCS */
    double        time_step;    /* s */
    double        end_time;     /* s */
    size_t        sample_every; /* steps between samples; 0 = not set (1) */
    unsigned      set;          /* bit p: parameter p was given */
    char          given[MFL_H1_P_COUNT_][MFL_H1_GIVEN]; /* as the user wrote it ("100 mm") */
    size_t        memory_limit; /* bytes for the results; 0 = no limit */
} MflH1Config;

typedef struct {
    MflH1Config config;   /* copy of the configuration that produced the results */
    size_t      nodes;
    size_t      samples;
    size_t      steps;    /* time steps taken */
    double      dx;       /* m */
    double      alpha;    /* m^2/s */
    double      fourier;  /* alpha dt / dx^2 of a full step */
    double      max_time_step; /* s: FTCS reference positivity limit, including convection */
    double      last_step;     /* s: length of the last step (= dt unless shortened) */
    double     *times;     /* samples values, s */
    size_t     *sample_step; /* samples values: time step index of each sample */
    double     *positions; /* nodes values, m */
    double     *T;         /* samples x nodes, K; sample s at T + s * nodes */
} MflH1Result;

/* Configuration. */
void mfl_h1_config_init(MflH1Config *c);
void mfl_h1_config_free(MflH1Config *c);
/* Deep copy (the profile is duplicated). Returns MFL_H1_NO_MEMORY on failure. */
MflH1Status mfl_h1_config_copy(MflH1Config *dst, const MflH1Config *src);

/* Name used in messages and metadata ("conductivity", "time_step", ...). */
const char *mfl_h1_param_name(MflH1Param p);

/* Setters. Values in SI; `given` (may be NULL) is the text recorded in the
 * metadata. Each validates its value and writes the reason to msg. */
MflH1Status mfl_h1_set_positive(MflH1Config *c, MflH1Param p, double value, const char *given,
                                char *msg, size_t msg_size); /* length, k, rho, cp, dt, end_time */
MflH1Status mfl_h1_set_nodes(MflH1Config *c, double nodes, char *msg, size_t msg_size);
MflH1Status mfl_h1_set_sample_every(MflH1Config *c, double steps, char *msg, size_t msg_size);
MflH1Status mfl_h1_set_initial_uniform(MflH1Config *c, double kelvin, const char *given,
                                       char *msg, size_t msg_size);
MflH1Status mfl_h1_set_initial_profile(MflH1Config *c, const double *kelvin, size_t count,
                                       char *msg, size_t msg_size);
MflH1Status mfl_h1_set_boundary(MflH1Config *c, MflH1Side side, MflH1Boundary kind,
                                double kelvin, const char *given, char *msg, size_t msg_size);
MflH1Status mfl_h1_set_convection(MflH1Config *c, MflH1Side side, double coefficient,
                                  double ambient_kelvin, char *msg, size_t msg_size);
MflH1Status mfl_h1_set_method(MflH1Config *c, MflH1Method method, char *msg, size_t msg_size);

/* Derived quantities of a configuration whose bar and mesh are set:
 * dx = L / (nodes - 1), alpha = k / (rho cp). Returns the FTCS reference
 * limit, dx^2/(2 alpha), further divided by (1 + h dx/k) at Robin ends,
 * even when backward Euler is selected. */
MflH1Status mfl_h1_max_time_step(const MflH1Config *c, double *dt_max, char *msg, size_t msg_size);

/* Validates the configuration and runs the simulation. On success the
 * result owns its buffers (free with mfl_h1_result_free). On failure the
 * result is empty and msg says which parameter is at fault. */
MflH1Status mfl_h1_run(const MflH1Config *c, MflH1Result *out, char *msg, size_t msg_size);

void mfl_h1_result_free(MflH1Result *r);

/* First sample at which the temperature of `node` is >= threshold (K):
 * returns 1 and sets *sample; 0 if it never is within the simulated time. */
int mfl_h1_first_sample_reaching(const MflH1Result *r, size_t node, double threshold,
                                 size_t *sample);

/* Discrete energy per unit cross-section area and heat capacity,
 * E = dx * (T0/2 + T1 + ... + T(N-2) + T(N-1)/2) (K*m): the quantity the
 * scheme conserves exactly (up to rounding) when both ends are insulated. */
double mfl_h1_discrete_energy(const double *T, size_t nodes, double dx);

#endif /* MFL_NUMERICAL_HEAT1D_H */
