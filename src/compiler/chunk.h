/*
 * chunk.h - MFL bytecode: values, instructions and the compiled chunk
 * (internal API).
 *
 * Quantities are stored as a magnitude in their DISPLAY unit plus that
 * unit's index in the chunk's unit table, so `print(0.1 mm)` prints
 * exactly "0.1 mm". Arithmetic converts to coherent SI (magnitude * unit
 * scale) whenever the operands' units differ. Dimensions are not stored
 * at run time: the checker has already proven them consistent.
 *
 * Encoding: 1-byte opcode, operands are 2-byte little-endian unsigned
 * integers (u16) unless noted.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_CHUNK_H
#define MFL_CHUNK_H

#include <stddef.h>

#include "parser/ast.h"
#include "runtime/arena.h"

#define MFL_NO_UNIT (-1)
#define MFL_CHUNK_MAX_INDEX 65535

typedef enum { MFL_VAL_NIL, MFL_VAL_BOOL, MFL_VAL_NUMBER, MFL_VAL_STRING } MflValueKind;

typedef struct {
    MflValueKind kind;
    int          unit; /* NUMBER: index into chunk units, or MFL_NO_UNIT */
    union {
        double number; /* magnitude in `unit` (coherent SI if MFL_NO_UNIT) */
        int    boolean;
        struct {
            const char *chars; /* owned by the chunk; escapes decoded */
            size_t      length;
        } string;
    } as;
} MflValue;

typedef struct {
    const char *text;  /* as printed, e.g. "J/mm" */
    MflScale    scale; /* exact factor to coherent SI */
} MflUnitEntry;

#define MFL_OPCODES(X)                                                       \
    X(CONST, "CONST", 1)               /* k: push constants[k]            */ \
    X(NIL, "NIL", 0)                                                         \
    X(TRUE, "TRUE", 0)                                                       \
    X(FALSE, "FALSE", 0)                                                     \
    X(POP, "POP", 0)                                                         \
    X(GET_GLOBAL, "GET_GLOBAL", 1)     /* g: push globals[g]              */ \
    X(SET_GLOBAL, "SET_GLOBAL", 1)     /* g: globals[g] = pop             */ \
    X(ADD, "ADD", 0)                                                         \
    X(SUB, "SUB", 0)                                                         \
    X(MUL, "MUL", 0)                                                         \
    X(DIV, "DIV", 0)                                                         \
    X(MOD, "MOD", 0)                   /* C fmod: sign of the dividend    */ \
    X(POW, "POW", 0)                                                         \
    X(NEG, "NEG", 0)                                                         \
    X(NOT, "NOT", 0)                                                         \
    X(EQ, "EQ", 0)                                                           \
    X(NE, "NE", 0)                                                           \
    X(LT, "LT", 0)                                                           \
    X(LE, "LE", 0)                                                           \
    X(GT, "GT", 0)                                                           \
    X(GE, "GE", 0)                                                           \
    X(JUMP_IF_FALSE, "JUMP_IF_FALSE", 1) /* off: if top is false, ip += off (top kept) */ \
    X(JUMP_IF_TRUE, "JUMP_IF_TRUE", 1)   /* off: if top is true, ip += off (top kept)  */ \
    X(DEFAULT_UNIT, "DEFAULT_UNIT", 1) /* u: a number without unit gets u */ \
    X(CONVERT, "CONVERT", 1)           /* u: express top in unit u        */ \
    X(VALUE_IN, "VALUE_IN", 1)         /* u: top in unit u, as plain Number */ \
    X(PRINT, "PRINT", 1)               /* n: print n values, push nil     */ \
    X(HALT, "HALT", 0)

typedef enum {
#define MFL_OP_ENUM_(name, str, nops) MFL_OP_##name,
    MFL_OPCODES(MFL_OP_ENUM_)
#undef MFL_OP_ENUM_
    MFL_OP_COUNT_
} MflOpcode;

typedef struct {
    unsigned char *code;
    int           *lines; /* source line per code byte */
    size_t         code_len;
    size_t         code_cap;
    size_t         lines_cap;

    MflValue      *constants;
    size_t         const_len;
    size_t         const_cap;

    MflUnitEntry  *units;
    size_t         unit_len;
    size_t         unit_cap;

    size_t         global_count;
    size_t         max_stack; /* deepest value stack the code needs */

    MflArena       strings; /* string constants and unit texts */
    size_t         limit;   /* byte budget for all allocations (0 = none) */
    size_t         used;
    int            out_of_memory;
} MflChunk;

void mfl_chunk_init(MflChunk *c, size_t memory_limit);
void mfl_chunk_free(MflChunk *c);

/* Appends; on failure sets out_of_memory and returns 0 / -1. */
int mfl_chunk_emit(MflChunk *c, unsigned char byte, int line);
int mfl_chunk_add_constant(MflChunk *c, MflValue v);          /* index or -1 */
int mfl_chunk_add_unit(MflChunk *c, const char *text, MflScale scale); /* dedup by text */
char *mfl_chunk_strdup(MflChunk *c, const char *s, size_t n);

const char *mfl_opcode_name(MflOpcode op);
int         mfl_opcode_operands(MflOpcode op); /* number of u16 operands */

/* Human-readable listing, one instruction per line. */
void mfl_chunk_disassemble(const MflChunk *c, MflWriteFn write, void *ctx);

#endif /* MFL_CHUNK_H */
