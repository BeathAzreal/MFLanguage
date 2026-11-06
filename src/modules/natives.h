/*
 * natives.h - modules implemented in C and imported by name (v0.3):
 *
 *     import math         math.sin(x), math.cos, math.exp, math.log, math.sqrt, math.pi
 *     import numerical    numerical.linspace(a, b, n), numerical.sum(v), numerical.mean(v)
 *     import heat         1D transient heat conduction (docs/heat-conduction.md)
 *
 * This file is the binding layer between the language and the numerical
 * code in modules/numerical/: every native function has a descriptor
 * (parameter kinds and dimensions, result kind) that the checker uses
 * before running and that is checked again on the values at run time
 * (gradual typing), then converts the values to plain SI doubles and calls
 * the numerical code. The numerical code itself knows nothing about the
 * parser, the compiler or the VM.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NATIVES_H
#define MFL_NATIVES_H

#include <stddef.h>

#include "compiler/chunk.h"
#include "parser/ast.h"

/* Parameter kinds. */
typedef enum {
    MFL_NP_NUMBER,        /* plain Number (dimensionless) */
    MFL_NP_COUNT,         /* plain Number; whole and >= min_count (checked at run time) */
    MFL_NP_QUANTITY,      /* any number or quantity (no absolute temperature) */
    MFL_NP_ANY_NUMBER,    /* any number or quantity, absolute temperatures included */
    MFL_NP_SAME_AS_FIRST, /* same type as argument 1 (dimension and absolute flag) */
    MFL_NP_DIM,           /* a quantity of dimension `dim` (not an absolute temperature) */
    MFL_NP_TEMPERATURE,   /* absolute temperature: degC, or K (not delta_degC) */
    MFL_NP_NUMBERS,       /* array of numbers (any one dimension) */
    MFL_NP_DIM_ARRAY,     /* array of quantities of dimension `dim` */
    MFL_NP_TEMPERATURES   /* array of absolute temperatures */
} MflNativeParamKind;

typedef struct {
    const char        *name;
    MflNativeParamKind kind;
    MflDim             dim;       /* DIM, DIM_ARRAY */
    const char        *unit;      /* DIM, DIM_ARRAY: unit shown in messages, e.g. "W/(m*K)" */
    int                min_count; /* COUNT */
} MflNativeParam;

/* Result kinds. */
typedef enum {
    MFL_NR_NIL,
    MFL_NR_NUMBER,         /* plain Number */
    MFL_NR_DIM,            /* quantity of dimension result_dim */
    MFL_NR_ARRAY_OF_FIRST, /* Array of the type of argument 1 */
    MFL_NR_ELEM_OF_FIRST,  /* the element type of argument 1 (an array) */
    MFL_NR_SQRT_FIRST,     /* dimension of argument 1 halved */
    MFL_NR_DIM_ARRAY,      /* Array[Quantity[result_dim]] */
    MFL_NR_ANY,            /* known only at run time (e.g. a time or nil) */
    MFL_NR_CONSTANT        /* module constant (math.pi): `constant`, no call */
} MflNativeResultKind;

#define MFL_NF_NO_AFFINE 1u /* elements of argument 1 may not be absolute temperatures */
#define MFL_NF_EVEN_DIM 2u  /* argument 1 needs even dimension exponents (sqrt) */

#define MFL_NATIVE_MAX_ARGS 4

typedef struct MflNativeEnv MflNativeEnv;
typedef struct MflNativeSession MflNativeSession;

/* 0: ok (*result set); 1: run-time error (env->message set); 2: out of memory. */
typedef int (*MflNativeImpl)(MflNativeEnv *env, const MflValue *args, MflValue *result);

typedef struct {
    const char         *module;
    const char         *name;
    int                 arity;
    MflNativeParam      params[MFL_NATIVE_MAX_ARGS];
    MflNativeResultKind result;
    MflDim              result_dim;
    unsigned            flags;
    double              constant;
    MflNativeImpl       impl;
} MflNative;

struct MflNativeEnv {
    const MflChunk   *chunk;
    void             *vm;      /* for mfl_vm_alloc_array() */
    MflNativeSession *session; /* simulations of this run (heat); NULL if unavailable */
    const MflNative  *native;
    char             *message;
    size_t            message_size;
};

/* Number of native functions and constants; descriptor by number. */
int              mfl_native_count(void);
const MflNative *mfl_native_get(int index);

/* Native number of module.name, or -1. */
int mfl_native_find(MflText module, MflText name);

/* 1 if `name` is a module that can be imported. */
int mfl_native_module_exists(MflText name);

/* "math, numerical, heat" */
const char *mfl_native_module_list(void);

/* Comma-separated names in a module ("linspace, sum, mean"). */
void mfl_native_module_members(MflText module, char *buf, size_t size);

/* Validates the arguments against the descriptor (run time) and calls the
 * function. Same return codes as MflNativeImpl. */
int mfl_native_call(MflNativeEnv *env, int index, const MflValue *args, int argc,
                    MflValue *result);

/* Helpers for implementations. */
double   mfl_native_si(const MflNativeEnv *env, const MflValue *v);     /* absolute SI */
MflValue mfl_native_number(double si, MflDim dim);                      /* in SI, no unit */
int      mfl_native_fail(MflNativeEnv *env, const char *fmt, ...);      /* returns 1 */
int      mfl_native_value_text(const MflNativeEnv *env, const MflValue *v, char *buf, size_t size);

/* Implemented by the VM: a new array of `length` elements (contents
 * uninitialised) owned by the run, or NULL when memory runs out. */
MflArray *mfl_vm_alloc_array(void *vm, size_t length);

#endif /* MFL_NATIVES_H */
