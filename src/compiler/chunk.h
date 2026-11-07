/*
 * chunk.h - MFL bytecode: values, instructions and the compiled chunk
 * (internal API).
 *
 * Numbers are stored as a magnitude in their DISPLAY unit plus that unit's
 * index in the chunk's unit table (MFL_NO_UNIT: coherent SI), so
 * `print(0.1 mm)` prints exactly "0.1 mm". Since v0.2 every value also
 * carries its dimension (and the absolute-temperature flag), because with
 * gradual typing some operations can only be checked at run time.
 *
 * Encoding: 1-byte opcode, then 0, 1 or 2 operands of 2 bytes each
 * (unsigned, little-endian).
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

typedef enum {
    MFL_VAL_NIL,
    MFL_VAL_BOOL,
    MFL_VAL_NUMBER,
    MFL_VAL_STRING,
    MFL_VAL_ARRAY,    /* v0.2 */
    MFL_VAL_FUNCTION, /* v0.2: as.function = function number */
    MFL_VAL_UNSET     /* a global whose declaration has not run yet */
} MflValueKind;

typedef struct MflArray MflArray;

typedef struct {
    MflValueKind  kind;
    int           unit;   /* NUMBER: index into chunk units, or MFL_NO_UNIT */
    MflDim        dim;    /* NUMBER */
    unsigned char affine; /* NUMBER: absolute temperature on an offset scale (degC) */
    union {
        double number; /* magnitude in `unit` (coherent SI if MFL_NO_UNIT) */
        int    boolean;
        struct {
            const char *chars; /* owned by the chunk; escapes decoded */
            size_t      length;
        } string;
        MflArray *array;
        int       function;
    } as;
} MflValue;

/* Arrays are created by the VM and live until the end of the run. */
struct MflArray {
    MflValue *items;
    size_t    length;
    size_t    capacity;
    MflArray *next_alloc; /* VM bookkeeping */
};

typedef struct {
    const char   *text;   /* as printed, e.g. "J/mm" */
    MflScale      scale;  /* exact factor to coherent SI */
    MflDim        dim;
    unsigned char affine; /* degC: SI value = magnitude * scale + offset */
    double        offset;
} MflUnitEntry;

/* Type to check at run time (CHECK_TYPE). */
typedef struct {
    MflTypeKind   kind; /* NUMBER BOOL STRING NIL ARRAY FUNCTION (ANY: anything) */
    MflDim        dim;
    unsigned char affine;
    int           elem; /* ARRAY: index of the element descriptor, -1 = any */
} MflTypeDesc;

typedef struct {
    const char *name;       /* owned by the chunk */
    int         arity;
    int         frame_size; /* local slots, parameters included */
    size_t      entry;      /* code offset */
    size_t      max_stack;  /* deepest stack the frame needs, locals included */
    int         line;
    int         column;
} MflFunction;

#define MFL_OPCODES(X)                                                                   \
    X(CONST, "CONST", 1)               /* k: push constants[k]                        */ \
    X(NIL, "NIL", 0)                                                                     \
    X(TRUE, "TRUE", 0)                                                                   \
    X(FALSE, "FALSE", 0)                                                                 \
    X(POP, "POP", 0)                                                                     \
    X(GET_GLOBAL, "GET_GLOBAL", 1)     /* g                                           */ \
    X(SET_GLOBAL, "SET_GLOBAL", 1)     /* g: globals[g] = pop                         */ \
    X(GET_LOCAL, "GET_LOCAL", 1)       /* l: frame slot                               */ \
    X(SET_LOCAL, "SET_LOCAL", 1)                                                         \
    X(ADD, "ADD", 0)                                                                     \
    X(SUB, "SUB", 0)                                                                     \
    X(MUL, "MUL", 0)                                                                     \
    X(DIV, "DIV", 0)                                                                     \
    X(MOD, "MOD", 0)                   /* C fmod: sign of the dividend                */ \
    X(POW, "POW", 0)                                                                     \
    X(NEG, "NEG", 0)                                                                     \
    X(NOT, "NOT", 0)                                                                     \
    X(EQ, "EQ", 0)                                                                       \
    X(NE, "NE", 0)                                                                       \
    X(LT, "LT", 0)                                                                       \
    X(LE, "LE", 0)                                                                       \
    X(GT, "GT", 0)                                                                       \
    X(GE, "GE", 0)                                                                       \
    X(JUMP, "JUMP", 1)                 /* off: ip += off                              */ \
    X(JUMP_IF_FALSE, "JUMP_IF_FALSE", 1) /* off: if top is false, ip += off (kept)    */ \
    X(JUMP_IF_TRUE, "JUMP_IF_TRUE", 1) /* off: if top is true, ip += off (kept)       */ \
    X(LOOP, "LOOP", 1)                 /* off: ip -= off                              */ \
    X(CONVERT, "CONVERT", 1)           /* u: express top in unit u                    */ \
    X(VALUE_IN, "VALUE_IN", 1)         /* u: top in unit u, as plain Number           */ \
    X(CALL, "CALL", 1)                 /* n: callee and n arguments -> result         */ \
    X(RETURN, "RETURN", 0)                                                               \
    X(PRINT, "PRINT", 1)               /* n: print n values, push nil                 */ \
    X(LEN, "LEN", 0)                                                                     \
    X(PUSH, "PUSH", 0)                 /* array, value -> nil                         */ \
    X(ARRAY, "ARRAY", 1)               /* n: n values -> array                        */ \
    X(INDEX, "INDEX", 0)               /* array, index -> element                     */ \
    X(SET_INDEX, "SET_INDEX", 0)       /* array, index, value ->                      */ \
    X(CHECK_TYPE, "CHECK_TYPE", 2)     /* t, c: top must match type t (context c)     */ \
    X(CHECK_LIKE_GLOBAL, "CHECK_LIKE_GLOBAL", 2) /* g, c: top must match globals[g]   */ \
    X(CHECK_LIKE_LOCAL, "CHECK_LIKE_LOCAL", 2)   /* l, c: top must match slot l       */ \
    X(RANGE_STEP, "RANGE_STEP", 0)                                                      \
    X(CALL_NATIVE, "CALL_NATIVE", 2)   /* f, n: module function f with n arguments    */ \
    X(HALT, "HALT", 0)

typedef enum {
#define MFL_OP_ENUM_(name, str, nops) MFL_OP_##name,
    MFL_OPCODES(MFL_OP_ENUM_)
#undef MFL_OP_ENUM_
    MFL_OP_COUNT_
} MflOpcode;

typedef struct {
    unsigned char *code;
    int           *lines;   /* source line per code byte */
    int           *columns; /* source column per code byte */
    int           *files;   /* source file per code byte (v0.3; index in the program's files) */
    size_t         files_cap;
    int            cur_file; /* file recorded by mfl_chunk_emit() */
    size_t         code_len;
    size_t         code_cap;
    size_t         lines_cap;
    size_t         columns_cap;

    MflValue      *constants;
    size_t         const_len;
    size_t         const_cap;

    MflUnitEntry  *units;
    size_t         unit_len;
    size_t         unit_cap;

    MflTypeDesc   *types;
    size_t         type_len;
    size_t         type_cap;

    MflFunction   *functions; /* user functions, by number */
    size_t         function_count;

    const char   **global_names;
    size_t         global_count;

    int            main_frame_size;
    size_t         main_max_stack;
    size_t         max_stack; /* main_max_stack (kept for v0.1 callers) */

    MflArena       strings; /* string constants, unit texts, names */
    size_t         limit;   /* byte budget for all allocations (0 = none) */
    size_t         used;
    int            out_of_memory;
} MflChunk;

void mfl_chunk_init(MflChunk *c, size_t memory_limit);
void mfl_chunk_free(MflChunk *c);

/* Appends; on failure sets out_of_memory and returns 0 / -1. */
int mfl_chunk_emit(MflChunk *c, unsigned char byte, int line, int column);
int mfl_chunk_add_constant(MflChunk *c, MflValue v); /* index or -1 */
/* dedup by text; offset and affine only for degC */
int mfl_chunk_add_unit(MflChunk *c, const char *text, MflScale scale);
int mfl_chunk_add_unit_ex(MflChunk *c, const char *text, MflScale scale, MflDim dim, int affine,
                          double offset);
int mfl_chunk_add_type(MflChunk *c, MflTypeDesc t); /* index or -1 */
char *mfl_chunk_strdup(MflChunk *c, const char *s, size_t n);
void *mfl_chunk_alloc(MflChunk *c, size_t bytes); /* zeroed, counts towards the limit */

const char *mfl_opcode_name(MflOpcode op);
int         mfl_opcode_operands(MflOpcode op); /* number of u16 operands */

/* Human-readable listing, one instruction per line. */
void mfl_chunk_disassemble(const MflChunk *c, MflWriteFn write, void *ctx);

/* "Number", "Quantity[m/s]", "Array[Number]", ... for a run-time value. */
void mfl_value_type_name(const MflValue *v, char *buf, size_t size);
void mfl_type_desc_name(const MflChunk *c, int desc, char *buf, size_t size);

#endif /* MFL_CHUNK_H */
