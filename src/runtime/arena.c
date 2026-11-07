/* arena.c - region allocator. SPDX-License-Identifier: MIT */
#include "runtime/arena.h"

#include <stdlib.h>
#include <string.h>

/* Strictest alignment among the types the AST stores. */
typedef union {
    long double ld;
    long long   ll;
    void       *p;
    void      (*fp)(void);
} MflMaxAlign;

#define MFL_ALIGN (sizeof(MflMaxAlign))
#define MFL_ARENA_BLOCK_SIZE 8192

struct MflArenaBlock {
    MflArenaBlock *next;
    size_t         size; /* usable bytes in data */
    size_t         used;
    MflMaxAlign    data[1]; /* flexible storage, aligned */
};

void mfl_arena_init(MflArena *a, size_t limit)
{
    a->head = NULL;
    a->used_total = 0;
    a->limit = limit;
}

void *mfl_arena_alloc(MflArena *a, size_t size)
{
    MflArenaBlock *b = a->head;
    void *p;

    if (size == 0) {
        size = 1;
    }
    if (size > (size_t)-1 - MFL_ALIGN) {
        return NULL;
    }
    size = (size + MFL_ALIGN - 1) / MFL_ALIGN * MFL_ALIGN;
    if (a->limit != 0 && (size > a->limit || a->used_total > a->limit - size)) {
        return NULL;
    }
    if (b == NULL || b->size - b->used < size) {
        size_t cap = size > MFL_ARENA_BLOCK_SIZE ? size : MFL_ARENA_BLOCK_SIZE;
        b = (MflArenaBlock *)malloc(offsetof(MflArenaBlock, data) + cap);
        if (b == NULL) {
            return NULL;
        }
        b->next = a->head;
        b->size = cap;
        b->used = 0;
        a->head = b;
    }
    p = (char *)b->data + b->used;
    b->used += size;
    a->used_total += size;
    memset(p, 0, size);
    return p;
}

void mfl_arena_free(MflArena *a)
{
    MflArenaBlock *b = a->head;
    while (b != NULL) {
        MflArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->used_total = 0;
}
