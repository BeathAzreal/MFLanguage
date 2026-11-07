/*
 * vm.h - stack-based virtual machine for MFL bytecode (internal API).
 *
 * Every operation checks its operands (kind, dimension, absolute
 * temperature) at run time. For fully typed programs these checks never
 * fail, because the checker has already proven them; with gradual typing
 * (MFL_TYPE_ANY) they are what keeps the program correct. A failed check is
 * a run-time error with the source position of the instruction.
 *
 * The VM also validates the bytecode itself (instruction bounds, stack
 * bounds, operand indices) and reports MFL_VM_INVALID_CODE instead of
 * misbehaving, so a damaged chunk cannot corrupt memory.
 *
 * All output goes through the write callback, so a host program (e.g. a
 * game) decides where print() writes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_VM_H
#define MFL_VM_H

#include <stddef.h>

#include "compiler/chunk.h"

#define MFL_VM_MAX_CALL_DEPTH 1000

typedef enum {
    MFL_VM_OK,
    MFL_VM_OUT_OF_MEMORY,
    MFL_VM_INVALID_CODE,
    MFL_VM_RUNTIME_ERROR /* see MflVmError */
} MflVmStatus;

typedef struct {
    size_t             memory_limit; /* bytes for stack, globals and arrays; 0 = none */
    unsigned long long max_steps;    /* instructions; 0 = no limit */
} MflVmLimits;

typedef struct {
    int  file;   /* index in the program's file table (v0.3) */
    int  line;   /* source position of the failing instruction (0 if unknown) */
    int  column;
    char message[400];
} MflVmError;

/* Runs the chunk. globals[0 .. init_count) start with init[] (host
 * inputs); the others start "unset". On success the final value of every
 * global is copied to snapshot (chunk->global_count entries) if it is not
 * NULL. Array values in the snapshot are only valid during the run. */
MflVmStatus mfl_vm_execute(const MflChunk *chunk, const MflValue *init, size_t init_count,
                           MflValue *snapshot, MflWriteFn write, void *ctx,
                           const MflVmLimits *limits, MflVmError *error);

/* v0.3: same as mfl_vm_execute(), with the session that holds the
 * simulations run by the program (heat.run(); see modules/natives.h).
 * session may be NULL: the heat module then reports a run-time error. */
struct MflNativeSession;
MflVmStatus mfl_vm_execute_ex(const MflChunk *chunk, const MflValue *init, size_t init_count,
                              MflValue *snapshot, MflWriteFn write, void *ctx,
                              const MflVmLimits *limits, struct MflNativeSession *session,
                              MflVmError *error);

/* v0.1 entry points (no limits besides memory; error details dropped). */
MflVmStatus mfl_vm_run(const MflChunk *chunk, MflWriteFn write, void *ctx, size_t memory_limit);
MflVmStatus mfl_vm_run_globals(const MflChunk *chunk, const MflValue *init, size_t init_count,
                               MflValue *snapshot, MflWriteFn write, void *ctx,
                               size_t memory_limit);

/* Writes a value the way print() shows it: "480 J/mm", "0.8", "true",
 * "nil", "[5 mm/s, 0.005 m/s]", "<fn heat_input>", or a string's characters. */
void mfl_vm_format_value(const MflChunk *chunk, const MflValue *v, MflWriteFn write, void *ctx);

#endif /* MFL_VM_H */
