/*
 * vm.h - stack-based virtual machine for MFL bytecode (internal API).
 *
 * A checked and compiled v0.1 program cannot fail at run time: names,
 * types and dimensions were proven consistent before execution, and
 * arithmetic follows IEEE 754 (division by zero gives inf or nan). The
 * VM still validates the bytecode it executes (instruction bounds, stack
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

typedef enum {
    MFL_VM_OK,
    MFL_VM_OUT_OF_MEMORY,
    MFL_VM_INVALID_CODE
} MflVmStatus;

MflVmStatus mfl_vm_run(const MflChunk *chunk, MflWriteFn write, void *ctx, size_t memory_limit);

/* Same, but globals[0 .. init_count) start with init[] (host inputs), and
 * on success the final value of every global is copied to snapshot
 * (chunk->global_count entries) if it is not NULL. */
MflVmStatus mfl_vm_run_globals(const MflChunk *chunk, const MflValue *init, size_t init_count,
                               MflValue *snapshot, MflWriteFn write, void *ctx,
                               size_t memory_limit);

/* Writes a value the way print() shows it: "480 J/mm", "0.8", "true",
 * "nil", or the string's characters. */
void mfl_vm_format_value(const MflChunk *chunk, const MflValue *v, MflWriteFn write, void *ctx);

#endif /* MFL_VM_H */
