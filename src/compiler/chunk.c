/* chunk.c - bytecode container and disassembler. SPDX-License-Identifier: MIT */
#include "compiler/chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"

void mfl_chunk_init(MflChunk *c, size_t memory_limit)
{
    memset(c, 0, sizeof *c);
    c->limit = memory_limit;
    mfl_arena_init(&c->strings, memory_limit);
}

void mfl_chunk_free(MflChunk *c)
{
    free(c->code);
    free(c->lines);
    free(c->columns);
    free(c->files);
    free(c->constants);
    free(c->units);
    free(c->types);
    mfl_arena_free(&c->strings);
    memset(c, 0, sizeof *c);
}

static int grow(MflChunk *c, void **array, size_t *cap, size_t need, size_t elem)
{
    size_t new_cap;
    size_t delta;
    void *p;

    if (need <= *cap) {
        return 1;
    }
    new_cap = *cap == 0 ? 16 : *cap * 2;
    while (new_cap < need) {
        new_cap *= 2;
    }
    delta = (new_cap - *cap) * elem;
    if (c->limit != 0 && (delta > c->limit || c->used > c->limit - delta)) {
        c->out_of_memory = 1;
        return 0;
    }
    p = realloc(*array, new_cap * elem);
    if (p == NULL) {
        c->out_of_memory = 1;
        return 0;
    }
    *array = p;
    *cap = new_cap;
    c->used += delta;
    return 1;
}

int mfl_chunk_emit(MflChunk *c, unsigned char byte, int line, int column)
{
    if (!grow(c, (void **)&c->code, &c->code_cap, c->code_len + 1, 1) ||
        !grow(c, (void **)&c->lines, &c->lines_cap, c->code_len + 1, sizeof(int)) ||
        !grow(c, (void **)&c->columns, &c->columns_cap, c->code_len + 1, sizeof(int)) ||
        !grow(c, (void **)&c->files, &c->files_cap, c->code_len + 1, sizeof(int))) {
        return 0;
    }
    c->files[c->code_len] = c->cur_file;
    c->code[c->code_len] = byte;
    c->lines[c->code_len] = line;
    c->columns[c->code_len] = column;
    c->code_len++;
    return 1;
}

int mfl_chunk_add_constant(MflChunk *c, MflValue v)
{
    if (c->const_len >= MFL_CHUNK_MAX_INDEX) {
        return -1;
    }
    if (!grow(c, (void **)&c->constants, &c->const_cap, c->const_len + 1, sizeof(MflValue))) {
        return -1;
    }
    c->constants[c->const_len] = v;
    return (int)c->const_len++;
}

void *mfl_chunk_alloc(MflChunk *c, size_t bytes)
{
    void *p = mfl_arena_alloc(&c->strings, bytes);
    if (p == NULL) {
        c->out_of_memory = 1;
    }
    return p;
}

char *mfl_chunk_strdup(MflChunk *c, const char *s, size_t n)
{
    char *p = (char *)mfl_chunk_alloc(c, n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int mfl_chunk_add_unit_ex(MflChunk *c, const char *text, MflScale scale, MflDim dim, int affine,
                          double offset)
{
    size_t i;
    char *copy;
    for (i = 0; i < c->unit_len; i++) {
        if (strcmp(c->units[i].text, text) == 0) {
            return (int)i;
        }
    }
    if (c->unit_len >= MFL_CHUNK_MAX_INDEX) {
        return -1;
    }
    copy = mfl_chunk_strdup(c, text, strlen(text));
    if (copy == NULL ||
        !grow(c, (void **)&c->units, &c->unit_cap, c->unit_len + 1, sizeof(MflUnitEntry))) {
        return -1;
    }
    c->units[c->unit_len].text = copy;
    c->units[c->unit_len].scale = scale;
    c->units[c->unit_len].dim = dim;
    c->units[c->unit_len].affine = (unsigned char)(affine != 0);
    c->units[c->unit_len].offset = offset;
    return (int)c->unit_len++;
}

int mfl_chunk_add_unit(MflChunk *c, const char *text, MflScale scale)
{
    return mfl_chunk_add_unit_ex(c, text, scale, mfl_dim_none(), 0, 0.0);
}

int mfl_chunk_add_type(MflChunk *c, MflTypeDesc t)
{
    size_t i;
    for (i = 0; i < c->type_len; i++) {
        const MflTypeDesc *d = &c->types[i];
        if (d->kind == t.kind && mfl_dim_equal(d->dim, t.dim) && d->affine == t.affine &&
            d->elem == t.elem) {
            return (int)i;
        }
    }
    if (c->type_len >= MFL_CHUNK_MAX_INDEX ||
        !grow(c, (void **)&c->types, &c->type_cap, c->type_len + 1, sizeof(MflTypeDesc))) {
        return -1;
    }
    c->types[c->type_len] = t;
    return (int)c->type_len++;
}

const char *mfl_opcode_name(MflOpcode op)
{
    static const char *const names[] = {
#define MFL_OP_NAME_(name, str, nops) str,
        MFL_OPCODES(MFL_OP_NAME_)
#undef MFL_OP_NAME_
    };
    return (unsigned)op < (unsigned)MFL_OP_COUNT_ ? names[op] : "?";
}

int mfl_opcode_operands(MflOpcode op)
{
    static const int counts[] = {
#define MFL_OP_COUNT_ENTRY_(name, str, nops) nops,
        MFL_OPCODES(MFL_OP_COUNT_ENTRY_)
#undef MFL_OP_COUNT_ENTRY_
    };
    return (unsigned)op < (unsigned)MFL_OP_COUNT_ ? counts[op] : 0;
}

/* ------------------------------------------------------------------------ */
/* Type names                                                                */
/* ------------------------------------------------------------------------ */

static void number_type_name(MflDim dim, int affine, char *buf, size_t size)
{
    char d[96];
    if (affine) {
        snprintf(buf, size, "Quantity[degC]");
    } else if (mfl_dim_is_none(dim)) {
        snprintf(buf, size, "Number");
    } else {
        mfl_dim_format(dim, d, sizeof d);
        snprintf(buf, size, "Quantity[%s]", d);
    }
}

void mfl_value_type_name(const MflValue *v, char *buf, size_t size)
{
    switch (v->kind) {
    case MFL_VAL_NIL: snprintf(buf, size, "Nil"); break;
    case MFL_VAL_BOOL: snprintf(buf, size, "Bool"); break;
    case MFL_VAL_STRING: snprintf(buf, size, "String"); break;
    case MFL_VAL_FUNCTION: snprintf(buf, size, "Function"); break;
    case MFL_VAL_UNSET: snprintf(buf, size, "<unset>"); break;
    case MFL_VAL_NUMBER: number_type_name(v->dim, v->affine, buf, size); break;
    case MFL_VAL_ARRAY:
        if (v->as.array != NULL && v->as.array->length > 0) {
            char inner[128];
            mfl_value_type_name(&v->as.array->items[0], inner, sizeof inner);
            snprintf(buf, size, "Array[%s]", inner);
        } else {
            snprintf(buf, size, "Array");
        }
        break;
    }
}

void mfl_type_desc_name(const MflChunk *c, int desc, char *buf, size_t size)
{
    const MflTypeDesc *t;
    if (desc < 0 || (size_t)desc >= c->type_len) {
        snprintf(buf, size, "Any");
        return;
    }
    t = &c->types[desc];
    switch (t->kind) {
    case MFL_TYPE_NUMBER: number_type_name(t->dim, t->affine, buf, size); break;
    case MFL_TYPE_BOOL: snprintf(buf, size, "Bool"); break;
    case MFL_TYPE_STRING: snprintf(buf, size, "String"); break;
    case MFL_TYPE_NIL: snprintf(buf, size, "Nil"); break;
    case MFL_TYPE_FUNCTION: snprintf(buf, size, "Function"); break;
    case MFL_TYPE_ARRAY: {
        char inner[128];
        mfl_type_desc_name(c, t->elem, inner, sizeof inner);
        snprintf(buf, size, "Array[%s]", inner);
        break;
    }
    default: snprintf(buf, size, "Any"); break;
    }
}

/* ------------------------------------------------------------------------ */
/* Disassembler                                                              */
/* ------------------------------------------------------------------------ */

static void put(MflWriteFn w, void *ctx, const char *s) { w(ctx, s, strlen(s)); }

static void describe_value(const MflChunk *c, const MflValue *v, char *buf, size_t size)
{
    char num[MFL_NUMBER_BUF];
    switch (v->kind) {
    case MFL_VAL_NIL: snprintf(buf, size, "nil"); break;
    case MFL_VAL_BOOL: snprintf(buf, size, "%s", v->as.boolean ? "true" : "false"); break;
    case MFL_VAL_STRING: {
        int n = v->as.string.length > 40 ? 40 : (int)v->as.string.length;
        snprintf(buf, size, "\"%.*s\"", n, v->as.string.chars);
        break;
    }
    case MFL_VAL_FUNCTION:
        snprintf(buf, size, "<fn %s>",
                 (size_t)v->as.function < c->function_count ? c->functions[v->as.function].name : "?");
        break;
    case MFL_VAL_NUMBER:
        mfl_number_format(v->as.number, num, sizeof num);
        if (v->unit == MFL_NO_UNIT) {
            snprintf(buf, size, "%s", num);
        } else {
            snprintf(buf, size, "%s %s", num, c->units[v->unit].text);
        }
        break;
    default: snprintf(buf, size, "?"); break;
    }
}

void mfl_chunk_disassemble(const MflChunk *c, MflWriteFn w, void *ctx)
{
    size_t ip = 0;
    size_t f;
    char line[320];
    char extra[200];
    char num[MFL_NUMBER_BUF];

    snprintf(line, sizeof line,
             "; globals %lu, functions %lu, constants %lu, units %lu, types %lu\n"
             "; main: frame %d, max stack %lu\n",
             (unsigned long)c->global_count, (unsigned long)c->function_count,
             (unsigned long)c->const_len, (unsigned long)c->unit_len, (unsigned long)c->type_len,
             c->main_frame_size, (unsigned long)c->main_max_stack);
    put(w, ctx, line);
    while (ip < c->code_len) {
        MflOpcode op = (MflOpcode)c->code[ip];
        int nops = mfl_opcode_operands(op);
        unsigned a = 0;
        unsigned b = 0;
        for (f = 0; f < c->function_count; f++) {
            if (c->functions[f].entry == ip) {
                snprintf(line, sizeof line, "; fn %s: arity %d, frame %d, max stack %lu\n",
                         c->functions[f].name, c->functions[f].arity, c->functions[f].frame_size,
                         (unsigned long)c->functions[f].max_stack);
                put(w, ctx, line);
            }
        }
        extra[0] = '\0';
        if (nops >= 1 && ip + 2 < c->code_len) {
            a = (unsigned)c->code[ip + 1] | ((unsigned)c->code[ip + 2] << 8);
        }
        if (nops >= 2 && ip + 4 < c->code_len) {
            b = (unsigned)c->code[ip + 3] | ((unsigned)c->code[ip + 4] << 8);
        }
        if (op == MFL_OP_CONST && a < c->const_len) {
            describe_value(c, &c->constants[a], extra, sizeof extra);
        } else if ((op == MFL_OP_CONVERT || op == MFL_OP_VALUE_IN) && a < c->unit_len) {
            mfl_number_format(mfl_scale_to_double(c->units[a].scale), num, sizeof num);
            snprintf(extra, sizeof extra, "%s (x%s)", c->units[a].text, num);
        } else if (op == MFL_OP_JUMP || op == MFL_OP_JUMP_IF_FALSE || op == MFL_OP_JUMP_IF_TRUE) {
            snprintf(extra, sizeof extra, "-> %04lu", (unsigned long)(ip + 3 + a));
        } else if (op == MFL_OP_LOOP) {
            snprintf(extra, sizeof extra, "-> %04lu", (unsigned long)(ip + 3 - a));
        } else if ((op == MFL_OP_GET_GLOBAL || op == MFL_OP_SET_GLOBAL) && a < c->global_count) {
            snprintf(extra, sizeof extra, "%s", c->global_names[a]);
        } else if (op == MFL_OP_CHECK_TYPE) {
            mfl_type_desc_name(c, (int)a, extra, sizeof extra);
        }
        if (nops == 2) {
            snprintf(line, sizeof line, "%04lu %4d:%-3d %-17s %5u %5u%s%s\n", (unsigned long)ip,
                     c->lines[ip], c->columns[ip], mfl_opcode_name(op), a, b, extra[0] ? "  " : "",
                     extra);
        } else if (nops == 1) {
            snprintf(line, sizeof line, "%04lu %4d:%-3d %-17s %5u%s%s\n", (unsigned long)ip,
                     c->lines[ip], c->columns[ip], mfl_opcode_name(op), a, extra[0] ? "  " : "",
                     extra);
        } else {
            snprintf(line, sizeof line, "%04lu %4d:%-3d %s\n", (unsigned long)ip, c->lines[ip],
                     c->columns[ip], mfl_opcode_name(op));
        }
        put(w, ctx, line);
        ip += 1 + 2 * (size_t)nops;
    }
}
