/*
 * arena.h - region allocator (internal API).
 *
 * Many small allocations, freed all at once. Used for the AST and parser
 * diagnostics. An optional byte limit makes out-of-memory paths testable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_ARENA_H
#define MFL_ARENA_H

#include <stddef.h>

typedef struct MflArenaBlock MflArenaBlock;

typedef struct {
    MflArenaBlock *head;
    size_t         used_total; /* bytes handed out, for the limit */
    size_t         limit;      /* 0 = unlimited */
} MflArena;

void  mfl_arena_init(MflArena *a, size_t limit);
/* Zero-initialised, suitably aligned memory, or NULL when out of memory
 * or over the limit. size 0 is treated as 1. */
void *mfl_arena_alloc(MflArena *a, size_t size);
void  mfl_arena_free(MflArena *a);

#endif /* MFL_ARENA_H */
