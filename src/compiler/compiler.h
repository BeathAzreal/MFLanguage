/*
 * compiler.h - compiles a checked MFL program to bytecode (internal API).
 *
 * Input: a program that passed mfl_parse() and mfl_check() (types and
 * unit scales are read from the AST). The only errors left at this stage
 * are numeric literals that do not fit a double, size limits and
 * running out of memory.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_COMPILER_H
#define MFL_COMPILER_H

#include <stddef.h>

#include "compiler/chunk.h"
#include "compiler/checker.h"
#include "runtime/diagnostic.h"

typedef struct {
    MflChunk       chunk;
    MflArena       arena; /* diagnostics */
    MflDiagnostic *diagnostics;
    int            diagnostic_count;
    int            out_of_memory;
} MflCompileResult;

/* Returns 1 on success. Always call mfl_compile_free() afterwards. */
int mfl_compile(const MflProgram *program, const MflCheckResult *checked, size_t memory_limit,
                MflCompileResult *out);

void mfl_compile_free(MflCompileResult *result);

#endif /* MFL_COMPILER_H */
