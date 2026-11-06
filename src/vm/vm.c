/*
 * vm.c - bytecode interpreter. See vm.h and compiler/chunk.h.
 *
 * Number values carry a magnitude in their display unit. Binary
 * arithmetic on two values with the same unit works on magnitudes and
 * keeps the unit (1 mm + 2 mm = 3 mm); otherwise both are converted to
 * coherent SI first and the result has no unit (the compiler then adds
 * the SI display unit with DEFAULT_UNIT).
 *
 * (Tried and rejected: multiplying magnitudes first and combining exact
 * unit scales. It made a few "round" cases exact but was correctly
 * rounded less often on 4500 random products: 59% vs 64%.)
 *
 * SPDX-License-Identifier: MIT
 */
#include "vm/vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"
#include "runtime/units.h"

/* Magnitude in coherent SI. */
static double si(const MflChunk *c, const MflValue *v)
{
    return v->unit == MFL_NO_UNIT ? v->as.number
                                  : mfl_scale_convert(v->as.number, c->units[v->unit].scale,
                                                      mfl_scale_one());
}

static void put(MflWriteFn w, void *ctx, const char *s) { w(ctx, s, strlen(s)); }

void mfl_vm_format_value(const MflChunk *c, const MflValue *v, MflWriteFn w, void *ctx)
{
    char num[MFL_NUMBER_BUF];
    switch (v->kind) {
    case MFL_VAL_NIL:
        put(w, ctx, "nil");
        break;
    case MFL_VAL_BOOL:
        put(w, ctx, v->as.boolean ? "true" : "false");
        break;
    case MFL_VAL_STRING:
        w(ctx, v->as.string.chars, v->as.string.length);
        break;
    case MFL_VAL_NUMBER:
        mfl_number_format(v->as.number, num, sizeof num);
        put(w, ctx, num);
        if (v->unit != MFL_NO_UNIT) {
            put(w, ctx, " ");
            put(w, ctx, c->units[v->unit].text);
        }
        break;
    }
}

static int values_equal(const MflChunk *c, const MflValue *a, const MflValue *b)
{
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case MFL_VAL_NIL:
        return 1;
    case MFL_VAL_BOOL:
        return a->as.boolean == b->as.boolean;
    case MFL_VAL_STRING:
        return a->as.string.length == b->as.string.length &&
               memcmp(a->as.string.chars, b->as.string.chars, a->as.string.length) == 0;
    case MFL_VAL_NUMBER:
        if (a->unit == b->unit) {
            return a->as.number == b->as.number;
        }
        return si(c, a) == si(c, b);
    }
    return 0;
}

static MflValue number(double x, int unit)
{
    MflValue v;
    v.kind = MFL_VAL_NUMBER;
    v.unit = unit;
    v.as.number = x;
    return v;
}

static MflValue boolean(int b)
{
    MflValue v;
    v.kind = MFL_VAL_BOOL;
    v.unit = MFL_NO_UNIT;
    v.as.boolean = b != 0;
    return v;
}

MflVmStatus mfl_vm_run(const MflChunk *c, MflWriteFn write, void *ctx, size_t memory_limit)
{
    return mfl_vm_run_globals(c, NULL, 0, NULL, write, ctx, memory_limit);
}

MflVmStatus mfl_vm_run_globals(const MflChunk *c, const MflValue *init, size_t init_count,
                               MflValue *snapshot, MflWriteFn write, void *ctx,
                               size_t memory_limit)
{
    size_t stack_cap = c->max_stack + 1;
    size_t bytes = (stack_cap + c->global_count) * sizeof(MflValue);
    MflValue *stack;
    MflValue *globals;
    size_t sp = 0; /* number of values on the stack */
    size_t ip = 0;
    size_t i;
    MflVmStatus status = MFL_VM_INVALID_CODE;

    if (memory_limit != 0 && bytes > memory_limit) {
        return MFL_VM_OUT_OF_MEMORY;
    }
    stack = (MflValue *)malloc(bytes);
    if (stack == NULL) {
        return MFL_VM_OUT_OF_MEMORY;
    }
    globals = stack + stack_cap;
    for (i = 0; i < c->global_count; i++) {
        globals[i].kind = MFL_VAL_NIL;
        globals[i].unit = MFL_NO_UNIT;
        globals[i].as.number = 0.0;
    }
    if (init_count > c->global_count) {
        free(stack);
        return MFL_VM_INVALID_CODE;
    }
    for (i = 0; i < init_count; i++) {
        if (init[i].kind == MFL_VAL_NUMBER && init[i].unit != MFL_NO_UNIT &&
            (init[i].unit < 0 || (size_t)init[i].unit >= c->unit_len)) {
            free(stack);
            return MFL_VM_INVALID_CODE;
        }
        globals[i] = init[i];
    }

#define NEED(n)               \
    do {                      \
        if (sp < (size_t)(n)) \
            goto done;        \
    } while (0)
#define ROOM()                \
    do {                      \
        if (sp >= stack_cap)  \
            goto done;        \
    } while (0)
#define TOP (stack[sp - 1])
#define NUMBERS2()                                                               \
    do {                                                                         \
        NEED(2);                                                                 \
        if (stack[sp - 1].kind != MFL_VAL_NUMBER || stack[sp - 2].kind != MFL_VAL_NUMBER) \
            goto done;                                                           \
    } while (0)

    for (;;) {
        MflOpcode op;
        unsigned arg = 0;

        if (ip >= c->code_len) {
            goto done;
        }
        op = (MflOpcode)c->code[ip++];
        if ((unsigned)op >= (unsigned)MFL_OP_COUNT_) {
            goto done;
        }
        if (mfl_opcode_operands(op) == 1) {
            if (ip + 2 > c->code_len) {
                goto done;
            }
            arg = (unsigned)c->code[ip] | ((unsigned)c->code[ip + 1] << 8);
            ip += 2;
        }

        switch (op) {
        case MFL_OP_CONST:
            ROOM();
            if (arg >= c->const_len) {
                goto done;
            }
            stack[sp++] = c->constants[arg];
            break;
        case MFL_OP_NIL:
            ROOM();
            stack[sp].kind = MFL_VAL_NIL;
            stack[sp].unit = MFL_NO_UNIT;
            stack[sp].as.number = 0.0;
            sp++;
            break;
        case MFL_OP_TRUE:
        case MFL_OP_FALSE:
            ROOM();
            stack[sp++] = boolean(op == MFL_OP_TRUE);
            break;
        case MFL_OP_POP:
            NEED(1);
            sp--;
            break;
        case MFL_OP_GET_GLOBAL:
            ROOM();
            if (arg >= c->global_count) {
                goto done;
            }
            stack[sp++] = globals[arg];
            break;
        case MFL_OP_SET_GLOBAL:
            NEED(1);
            if (arg >= c->global_count) {
                goto done;
            }
            globals[arg] = stack[--sp];
            break;

        case MFL_OP_ADD:
        case MFL_OP_SUB:
        case MFL_OP_MOD: {
            MflValue *a;
            MflValue *b;
            double x;
            double y;
            int unit;
            NUMBERS2();
            a = &stack[sp - 2];
            b = &stack[sp - 1];
            if (a->unit == b->unit) {
                x = a->as.number;
                y = b->as.number;
                unit = a->unit;
            } else {
                x = si(c, a);
                y = si(c, b);
                unit = MFL_NO_UNIT;
            }
            *a = number(op == MFL_OP_ADD ? x + y : op == MFL_OP_SUB ? x - y : fmod(x, y), unit);
            sp--;
            break;
        }
        case MFL_OP_MUL:
        case MFL_OP_DIV:
        case MFL_OP_POW: {
            double x;
            double y;
            NUMBERS2();
            x = si(c, &stack[sp - 2]);
            y = si(c, &stack[sp - 1]);
            stack[sp - 2] =
                number(op == MFL_OP_MUL ? x * y : op == MFL_OP_DIV ? x / y : pow(x, y), MFL_NO_UNIT);
            sp--;
            break;
        }
        case MFL_OP_NEG:
            NEED(1);
            if (TOP.kind != MFL_VAL_NUMBER) {
                goto done;
            }
            TOP.as.number = -TOP.as.number;
            break;
        case MFL_OP_NOT:
            NEED(1);
            if (TOP.kind != MFL_VAL_BOOL) {
                goto done;
            }
            TOP.as.boolean = !TOP.as.boolean;
            break;

        case MFL_OP_EQ:
        case MFL_OP_NE: {
            int eq;
            NEED(2);
            eq = values_equal(c, &stack[sp - 2], &stack[sp - 1]);
            stack[sp - 2] = boolean(op == MFL_OP_EQ ? eq : !eq);
            sp--;
            break;
        }
        case MFL_OP_LT:
        case MFL_OP_LE:
        case MFL_OP_GT:
        case MFL_OP_GE: {
            MflValue *a;
            MflValue *b;
            double x;
            double y;
            int r;
            NUMBERS2();
            a = &stack[sp - 2];
            b = &stack[sp - 1];
            if (a->unit == b->unit) {
                x = a->as.number;
                y = b->as.number;
            } else {
                x = si(c, a);
                y = si(c, b);
            }
            r = op == MFL_OP_LT ? x < y : op == MFL_OP_LE ? x <= y : op == MFL_OP_GT ? x > y : x >= y;
            stack[sp - 2] = boolean(r);
            sp--;
            break;
        }

        case MFL_OP_JUMP_IF_FALSE:
        case MFL_OP_JUMP_IF_TRUE:
            NEED(1);
            if (TOP.kind != MFL_VAL_BOOL || ip + arg > c->code_len) {
                goto done;
            }
            if (TOP.as.boolean == (op == MFL_OP_JUMP_IF_TRUE)) {
                ip += arg;
            }
            break;

        case MFL_OP_DEFAULT_UNIT:
            NEED(1);
            if (arg >= c->unit_len || TOP.kind != MFL_VAL_NUMBER) {
                goto done;
            }
            if (TOP.unit == MFL_NO_UNIT) {
                TOP.unit = (int)arg; /* SI display units have scale 1 */
            }
            break;
        case MFL_OP_CONVERT:
        case MFL_OP_VALUE_IN:
            NEED(1);
            if (arg >= c->unit_len || TOP.kind != MFL_VAL_NUMBER) {
                goto done;
            }
            if (TOP.unit != (int)arg) {
                MflScale from = TOP.unit == MFL_NO_UNIT ? mfl_scale_one() : c->units[TOP.unit].scale;
                TOP.as.number = mfl_scale_convert(TOP.as.number, from, c->units[arg].scale);
            }
            TOP.unit = op == MFL_OP_CONVERT ? (int)arg : MFL_NO_UNIT;
            break;

        case MFL_OP_PRINT: {
            size_t n = arg;
            size_t k;
            NEED(n);
            for (k = 0; k < n; k++) {
                if (k > 0) {
                    write(ctx, " ", 1);
                }
                mfl_vm_format_value(c, &stack[sp - n + k], write, ctx);
            }
            write(ctx, "\n", 1);
            sp -= n;
            ROOM(); /* print() returns nil */
            stack[sp].kind = MFL_VAL_NIL;
            stack[sp].unit = MFL_NO_UNIT;
            stack[sp].as.number = 0.0;
            sp++;
            break;
        }

        case MFL_OP_HALT:
            status = MFL_VM_OK;
            if (snapshot != NULL) {
                memcpy(snapshot, globals, c->global_count * sizeof *globals);
            }
            goto done;

        default:
            goto done;
        }
    }

done:
#undef NEED
#undef ROOM
#undef TOP
#undef NUMBERS2
    free(stack);
    return status;
}
