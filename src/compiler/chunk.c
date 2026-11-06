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
    /* The string arena shares the budget through its own limit. */
    mfl_arena_init(&c->strings, memory_limit);
}

void mfl_chunk_free(MflChunk *c)
{
    free(c->code);
    free(c->lines);
    free(c->constants);
    free(c->units);
    mfl_arena_free(&c->strings);
    memset(c, 0, sizeof *c);
}

/* Grows a dynamic array to hold at least `need` elements. */
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

int mfl_chunk_emit(MflChunk *c, unsigned char byte, int line)
{
    if (!grow(c, (void **)&c->code, &c->code_cap, c->code_len + 1, 1) ||
        !grow(c, (void **)&c->lines, &c->lines_cap, c->code_len + 1, sizeof(int))) {
        return 0;
    }
    c->code[c->code_len] = byte;
    c->lines[c->code_len] = line;
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

char *mfl_chunk_strdup(MflChunk *c, const char *s, size_t n)
{
    char *p = (char *)mfl_arena_alloc(&c->strings, n + 1);
    if (p == NULL) {
        c->out_of_memory = 1;
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int mfl_chunk_add_unit(MflChunk *c, const char *text, MflScale scale)
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
    return (int)c->unit_len++;
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
    case MFL_VAL_NUMBER:
        mfl_number_format(v->as.number, num, sizeof num);
        if (v->unit == MFL_NO_UNIT) {
            snprintf(buf, size, "%s", num);
        } else {
            snprintf(buf, size, "%s %s", num, c->units[v->unit].text);
        }
        break;
    }
}

void mfl_chunk_disassemble(const MflChunk *c, MflWriteFn w, void *ctx)
{
    size_t ip = 0;
    char line[256];
    char extra[128];
    char num[MFL_NUMBER_BUF];

    snprintf(line, sizeof line, "; globals %lu, max stack %lu, constants %lu, units %lu\n",
             (unsigned long)c->global_count, (unsigned long)c->max_stack,
             (unsigned long)c->const_len, (unsigned long)c->unit_len);
    put(w, ctx, line);
    while (ip < c->code_len) {
        MflOpcode op = (MflOpcode)c->code[ip];
        int nops = mfl_opcode_operands(op);
        unsigned arg = 0;
        extra[0] = '\0';
        if (nops == 1 && ip + 2 < c->code_len) {
            arg = (unsigned)c->code[ip + 1] | ((unsigned)c->code[ip + 2] << 8);
        }
        if (op == MFL_OP_CONST && arg < c->const_len) {
            describe_value(c, &c->constants[arg], extra, sizeof extra);
        } else if ((op == MFL_OP_DEFAULT_UNIT || op == MFL_OP_CONVERT || op == MFL_OP_VALUE_IN) &&
                   arg < c->unit_len) {
            mfl_number_format(mfl_scale_to_double(c->units[arg].scale), num, sizeof num);
            snprintf(extra, sizeof extra, "%s (x%s)", c->units[arg].text, num);
        } else if (op == MFL_OP_JUMP_IF_FALSE || op == MFL_OP_JUMP_IF_TRUE) {
            snprintf(extra, sizeof extra, "-> %04lu", (unsigned long)(ip + 3 + arg));
        }
        if (nops == 1) {
            snprintf(line, sizeof line, "%04lu %4d %-14s %5u%s%s\n", (unsigned long)ip,
                     c->lines[ip], mfl_opcode_name(op), arg, extra[0] ? "  " : "", extra);
        } else {
            snprintf(line, sizeof line, "%04lu %4d %s\n", (unsigned long)ip, c->lines[ip],
                     mfl_opcode_name(op));
        }
        put(w, ctx, line);
        ip += 1 + 2 * (size_t)nops;
    }
}
