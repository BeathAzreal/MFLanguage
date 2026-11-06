/*
 * vm.c - bytecode interpreter. See vm.h and compiler/chunk.h.
 *
 * Numbers carry a magnitude in their display unit. + - % and comparisons on
 * two values with the same unit work on magnitudes and keep the unit
 * (1 mm + 2 mm = 3 mm); otherwise both are converted to coherent SI first
 * and the result has no unit (printed with the SI name of its dimension).
 *
 * (Tried and rejected: multiplying magnitudes first and combining exact
 * unit scales. It made a few "round" cases exact but was correctly
 * rounded less often on 4500 random products: 59% vs 64%; ADR-025.)
 *
 * Absolute temperatures (degC, `affine`) follow docs/units.md section 5:
 * degC - degC is a difference (K); degC +- difference stays absolute;
 * every other arithmetic on them is an error.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vm/vm.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/natives.h"
#include "runtime/number.h"
#include "runtime/units.h"

typedef struct {
    int    fn;        /* function number, -1 for the main program */
    size_t base;      /* stack index of local slot 0 */
    size_t frame_size;
    size_t return_ip;
} Frame;

typedef struct {
    const MflChunk *c;
    MflValue       *stack;
    size_t          cap;
    size_t          sp;
    MflValue       *globals;
    Frame          *frames;
    int             frame_count;
    int             frame_cap;
    MflArray       *arrays;
    size_t          used;
    size_t          limit;
    MflVmError     *err;
    size_t          op_ip; /* start of the current instruction */
    int             oom;   /* set by mfl_vm_alloc_array() */
} Vm;

/* ------------------------------------------------------------------------ */
/* Values                                                                    */
/* ------------------------------------------------------------------------ */

static MflValue nil_value(void)
{
    MflValue v;
    memset(&v, 0, sizeof v);
    v.kind = MFL_VAL_NIL;
    v.unit = MFL_NO_UNIT;
    return v;
}

static MflValue number(double x, int unit, MflDim dim, int affine)
{
    MflValue v = nil_value();
    v.kind = MFL_VAL_NUMBER;
    v.unit = unit;
    v.dim = dim;
    v.affine = (unsigned char)(affine != 0);
    v.as.number = x;
    return v;
}

static MflValue boolean(int b)
{
    MflValue v = nil_value();
    v.kind = MFL_VAL_BOOL;
    v.as.boolean = b != 0;
    return v;
}

static MflScale scale_of(const MflChunk *c, int unit)
{
    return unit == MFL_NO_UNIT ? mfl_scale_one() : c->units[unit].scale;
}

/* Coherent SI magnitude of a difference (no offset). */
static double si_diff(const MflChunk *c, const MflValue *v)
{
    return v->unit == MFL_NO_UNIT ? v->as.number
                                  : mfl_scale_convert(v->as.number, c->units[v->unit].scale,
                                                      mfl_scale_one());
}

/* Coherent SI magnitude, absolute (degC adds its offset). */
static double si_abs(const MflChunk *c, const MflValue *v)
{
    double r = si_diff(c, v);
    if (v->unit != MFL_NO_UNIT && c->units[v->unit].affine) {
        r += c->units[v->unit].offset;
    }
    return r;
}

/* Magnitude of v in unit u. */
static double in_unit(const MflChunk *c, const MflValue *v, int u)
{
    const MflUnitEntry *to = &c->units[u];
    int from_affine = v->unit != MFL_NO_UNIT && c->units[v->unit].affine;
    if (v->unit == u) {
        return v->as.number;
    }
    if (!from_affine && !to->affine) {
        return mfl_scale_convert(v->as.number, scale_of(c, v->unit), to->scale);
    }
    return mfl_scale_convert(si_abs(c, v) - (to->affine ? to->offset : 0.0), mfl_scale_one(),
                             to->scale);
}

static int same_type(const MflValue *a, const MflValue *b)
{
    if (a->kind != b->kind) {
        return 0;
    }
    if (a->kind == MFL_VAL_NUMBER) {
        return mfl_dim_equal(a->dim, b->dim) && a->affine == b->affine;
    }
    return 1;
}

static int matches_desc(const MflChunk *c, const MflValue *v, int desc, int depth)
{
    const MflTypeDesc *t;
    size_t i;
    if (desc < 0 || (size_t)desc >= c->type_len) {
        return 1;
    }
    t = &c->types[desc];
    switch (t->kind) {
    case MFL_TYPE_NUMBER:
        return v->kind == MFL_VAL_NUMBER && mfl_dim_equal(v->dim, t->dim) && v->affine == t->affine;
    case MFL_TYPE_BOOL: return v->kind == MFL_VAL_BOOL;
    case MFL_TYPE_STRING: return v->kind == MFL_VAL_STRING;
    case MFL_TYPE_NIL: return v->kind == MFL_VAL_NIL;
    case MFL_TYPE_FUNCTION: return v->kind == MFL_VAL_FUNCTION;
    case MFL_TYPE_ARRAY:
        if (v->kind != MFL_VAL_ARRAY) {
            return 0;
        }
        if (t->elem < 0 || depth > 16) {
            return 1;
        }
        for (i = 0; i < v->as.array->length; i++) {
            if (!matches_desc(c, &v->as.array->items[i], t->elem, depth + 1)) {
                return 0;
            }
        }
        return 1;
    default:
        return 1;
    }
}

static void put(MflWriteFn w, void *ctx, const char *s) { w(ctx, s, strlen(s)); }

static void format_value(const MflChunk *c, const MflValue *v, MflWriteFn w, void *ctx, int depth)
{
    char num[MFL_NUMBER_BUF];
    char unit[128];
    size_t i;
    switch (v->kind) {
    case MFL_VAL_NIL:
    case MFL_VAL_UNSET:
        put(w, ctx, "nil");
        break;
    case MFL_VAL_BOOL:
        put(w, ctx, v->as.boolean ? "true" : "false");
        break;
    case MFL_VAL_STRING:
        if (depth > 0) { /* inside an array: quoted, so commas stay unambiguous */
            put(w, ctx, "\"");
        }
        w(ctx, v->as.string.chars, v->as.string.length);
        if (depth > 0) {
            put(w, ctx, "\"");
        }
        break;
    case MFL_VAL_FUNCTION:
        put(w, ctx, "<fn ");
        put(w, ctx, (size_t)v->as.function < c->function_count ? c->functions[v->as.function].name
                                                                 : "?");
        put(w, ctx, ">");
        break;
    case MFL_VAL_NUMBER:
        mfl_number_format(v->as.number, num, sizeof num);
        put(w, ctx, num);
        if (v->unit != MFL_NO_UNIT) {
            put(w, ctx, " ");
            put(w, ctx, c->units[v->unit].text);
        } else if (!mfl_dim_is_none(v->dim)) {
            mfl_dim_display_name(v->dim, unit, sizeof unit);
            put(w, ctx, " ");
            put(w, ctx, unit);
        }
        break;
    case MFL_VAL_ARRAY:
        if (depth > 8) {
            put(w, ctx, "[...]");
            break;
        }
        put(w, ctx, "[");
        for (i = 0; i < v->as.array->length; i++) {
            if (i > 0) {
                put(w, ctx, ", ");
            }
            format_value(c, &v->as.array->items[i], w, ctx, depth + 1);
        }
        put(w, ctx, "]");
        break;
    }
}

void mfl_vm_format_value(const MflChunk *c, const MflValue *v, MflWriteFn w, void *ctx)
{
    format_value(c, v, w, ctx, 0);
}

/* ------------------------------------------------------------------------ */
/* Errors and memory                                                         */
/* ------------------------------------------------------------------------ */

static MflVmStatus runtime_error(Vm *vm, const char *fmt, ...)
{
    va_list ap;
    if (vm->err != NULL) {
        vm->err->file = vm->op_ip < vm->c->code_len && vm->c->files != NULL ? vm->c->files[vm->op_ip] : 0;
        vm->err->line = vm->op_ip < vm->c->code_len ? vm->c->lines[vm->op_ip] : 0;
        vm->err->column = vm->op_ip < vm->c->code_len ? vm->c->columns[vm->op_ip] : 0;
        va_start(ap, fmt);
        vsnprintf(vm->err->message, sizeof vm->err->message, fmt, ap);
        va_end(ap);
    }
    return MFL_VM_RUNTIME_ERROR;
}

static int charge(Vm *vm, size_t bytes)
{
    if (vm->limit != 0 && (bytes > vm->limit || vm->used > vm->limit - bytes)) {
        return 0;
    }
    vm->used += bytes;
    return 1;
}

static int ensure_stack(Vm *vm, size_t need)
{
    size_t cap;
    MflValue *p;
    if (need <= vm->cap) {
        return 1;
    }
    cap = vm->cap ? vm->cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    if (!charge(vm, (cap - vm->cap) * sizeof(MflValue))) {
        return 0;
    }
    p = (MflValue *)realloc(vm->stack, cap * sizeof(MflValue));
    if (p == NULL) {
        return 0;
    }
    vm->stack = p;
    vm->cap = cap;
    return 1;
}

static MflArray *new_array(Vm *vm, size_t capacity)
{
    MflArray *a;
    if (!charge(vm, sizeof *a + capacity * sizeof(MflValue))) {
        return NULL;
    }
    a = (MflArray *)calloc(1, sizeof *a);
    if (a == NULL) {
        return NULL;
    }
    if (capacity > 0) {
        a->items = (MflValue *)malloc(capacity * sizeof(MflValue));
        if (a->items == NULL) {
            free(a);
            return NULL;
        }
    }
    a->capacity = capacity;
    a->next_alloc = vm->arrays;
    vm->arrays = a;
    return a;
}

MflArray *mfl_vm_alloc_array(void *vmp, size_t length)
{
    Vm *vm = (Vm *)vmp;
    MflArray *a;
    if (length > ((size_t)-1) / sizeof(MflValue) / 2) {
        return NULL;
    }
    a = new_array(vm, length);
    if (a != NULL) {
        a->length = length;
    }
    return a;
}

static int array_reserve(Vm *vm, MflArray *a, size_t need)
{
    size_t cap;
    MflValue *p;
    if (need <= a->capacity) {
        return 1;
    }
    cap = a->capacity ? a->capacity * 2 : 4;
    while (cap < need) {
        cap *= 2;
    }
    if (!charge(vm, (cap - a->capacity) * sizeof(MflValue))) {
        return 0;
    }
    p = (MflValue *)realloc(a->items, cap * sizeof(MflValue));
    if (p == NULL) {
        return 0;
    }
    a->items = p;
    a->capacity = cap;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Interpreter                                                               */
/* ------------------------------------------------------------------------ */

static const char *op_symbol(MflOpcode op)
{
    switch (op) {
    case MFL_OP_ADD: return "+";
    case MFL_OP_SUB: return "-";
    case MFL_OP_MUL: return "*";
    case MFL_OP_DIV: return "/";
    case MFL_OP_MOD: return "%";
    case MFL_OP_POW: return "^";
    case MFL_OP_LT: return "<";
    case MFL_OP_LE: return "<=";
    case MFL_OP_GT: return ">";
    case MFL_OP_GE: return ">=";
    case MFL_OP_EQ: return "==";
    case MFL_OP_NE: return "!=";
    default: return "?";
    }
}

#define TYPE_NAME(v, buf) mfl_value_type_name((v), (buf), sizeof(buf))

/* Index operand: a plain number with an integer value within the array. */
static MflVmStatus check_index(Vm *vm, const MflValue *arr, const MflValue *ix, size_t *out,
                               int allow_end)
{
    char ts[160];
    double d;
    size_t len;
    if (arr->kind != MFL_VAL_ARRAY) {
        TYPE_NAME(arr, ts);
        return runtime_error(vm, "cannot index a %s (only arrays can be indexed)", ts);
    }
    if (ix->kind != MFL_VAL_NUMBER || !mfl_dim_is_none(ix->dim)) {
        TYPE_NAME(ix, ts);
        return runtime_error(vm, "array index must be a plain number (0, 1, 2, ...), found %s", ts);
    }
    d = ix->as.number;
    len = arr->as.array->length;
    if (d != floor(d) || isnan(d)) {
        char num[MFL_NUMBER_BUF];
        mfl_number_format(d, num, sizeof num);
        return runtime_error(vm, "array index must be a whole number, found %s", num);
    }
    if (d < 0 || d >= (double)len + (allow_end ? 1 : 0)) {
        char num[MFL_NUMBER_BUF];
        mfl_number_format(d, num, sizeof num);
        if (len == 0) {
            return runtime_error(vm, "index %s is out of range: the array is empty", num);
        }
        return runtime_error(vm,
                             "index %s is out of range for an array of length %lu (valid indices: "
                             "0 to %lu)",
                             num, (unsigned long)len, (unsigned long)(len - 1));
    }
    *out = (size_t)d;
    return MFL_VM_OK;
}

MflVmStatus mfl_vm_execute(const MflChunk *c, const MflValue *init, size_t init_count,
                           MflValue *snapshot, MflWriteFn write, void *ctx,
                           const MflVmLimits *limits, MflVmError *error)
{
    return mfl_vm_execute_ex(c, init, init_count, snapshot, write, ctx, limits, NULL, error);
}

MflVmStatus mfl_vm_execute_ex(const MflChunk *c, const MflValue *init, size_t init_count,
                              MflValue *snapshot, MflWriteFn write, void *ctx,
                              const MflVmLimits *limits, struct MflNativeSession *session,
                              MflVmError *error)
{
    Vm vm;
    size_t ip = 0;
    size_t i;
    unsigned long long steps = 0;
    unsigned long long max_steps = limits != NULL ? limits->max_steps : 0;
    MflVmStatus status = MFL_VM_INVALID_CODE;
    char ta[160];
    char tb[160];

    memset(&vm, 0, sizeof vm);
    vm.c = c;
    vm.limit = limits != NULL ? limits->memory_limit : 0;
    vm.err = error;
    if (error != NULL) {
        memset(error, 0, sizeof *error);
    }
    if (init_count > c->global_count || c->main_frame_size < 0) {
        return MFL_VM_INVALID_CODE;
    }
    if (!charge(&vm, (c->global_count + 1) * sizeof(MflValue) + 4 * sizeof(Frame))) {
        return MFL_VM_OUT_OF_MEMORY;
    }
    vm.globals = (MflValue *)malloc((c->global_count + 1) * sizeof(MflValue));
    vm.frames = (Frame *)malloc(4 * sizeof(Frame));
    vm.frame_cap = 4;
    if (vm.globals == NULL || vm.frames == NULL ||
        !ensure_stack(&vm, (size_t)c->main_frame_size + c->main_max_stack + 1)) {
        status = MFL_VM_OUT_OF_MEMORY;
        goto done;
    }
    for (i = 0; i < c->global_count; i++) {
        vm.globals[i] = nil_value();
        vm.globals[i].kind = MFL_VAL_UNSET;
    }
    for (i = 0; i < init_count; i++) {
        if (init[i].kind == MFL_VAL_NUMBER && init[i].unit != MFL_NO_UNIT &&
            (init[i].unit < 0 || (size_t)init[i].unit >= c->unit_len)) {
            goto done;
        }
        vm.globals[i] = init[i];
    }
    for (i = 0; i < (size_t)c->main_frame_size; i++) {
        vm.stack[i] = nil_value();
    }
    vm.sp = (size_t)c->main_frame_size;
    vm.frames[0].fn = -1;
    vm.frames[0].base = 0;
    vm.frames[0].frame_size = (size_t)c->main_frame_size;
    vm.frames[0].return_ip = 0;
    vm.frame_count = 1;

#define FRAME (vm.frames[vm.frame_count - 1])
/* n operands above the current frame's local slots */
#define NEED(n)                                                     \
    do {                                                            \
        if (vm.sp < FRAME.base + FRAME.frame_size + (size_t)(n))    \
            goto done;                                              \
    } while (0)
#define ROOM()                       \
    do {                             \
        if (vm.sp >= vm.cap && !ensure_stack(&vm, vm.sp + 1)) { \
            status = MFL_VM_OUT_OF_MEMORY; \
            goto done;               \
        }                            \
    } while (0)
#define TOP (vm.stack[vm.sp - 1])
#define FAIL(...)                                  \
    do {                                           \
        status = runtime_error(&vm, __VA_ARGS__);  \
        goto done;                                 \
    } while (0)

    for (;;) {
        MflOpcode op;
        unsigned a = 0;
        unsigned b = 0;
        int nops;

        if (ip >= c->code_len) {
            goto done;
        }
        vm.op_ip = ip;
        if (max_steps != 0 && ++steps > max_steps) {
            FAIL("instruction limit of %llu exceeded (an endless loop?)", max_steps);
        }
        op = (MflOpcode)c->code[ip++];
        if ((unsigned)op >= (unsigned)MFL_OP_COUNT_) {
            goto done;
        }
        nops = mfl_opcode_operands(op);
        if (ip + 2 * (size_t)nops > c->code_len) {
            goto done;
        }
        if (nops >= 1) {
            a = (unsigned)c->code[ip] | ((unsigned)c->code[ip + 1] << 8);
        }
        if (nops >= 2) {
            b = (unsigned)c->code[ip + 2] | ((unsigned)c->code[ip + 3] << 8);
        }
        ip += 2 * (size_t)nops;

        switch (op) {
        case MFL_OP_CONST:
            ROOM();
            if (a >= c->const_len) {
                goto done;
            }
            vm.stack[vm.sp++] = c->constants[a];
            break;
        case MFL_OP_NIL:
            ROOM();
            vm.stack[vm.sp++] = nil_value();
            break;
        case MFL_OP_TRUE:
        case MFL_OP_FALSE:
            ROOM();
            vm.stack[vm.sp++] = boolean(op == MFL_OP_TRUE);
            break;
        case MFL_OP_POP:
            NEED(1);
            vm.sp--;
            break;
        case MFL_OP_GET_GLOBAL:
            ROOM();
            if (a >= c->global_count) {
                goto done;
            }
            if (vm.globals[a].kind == MFL_VAL_UNSET) {
                FAIL("'%s' is used before its declaration has run (it is declared later in "
                     "the program)",
                     c->global_names != NULL ? c->global_names[a] : "?");
            }
            vm.stack[vm.sp++] = vm.globals[a];
            break;
        case MFL_OP_SET_GLOBAL:
            NEED(1);
            if (a >= c->global_count) {
                goto done;
            }
            vm.globals[a] = vm.stack[--vm.sp];
            break;
        case MFL_OP_GET_LOCAL:
            ROOM();
            if (a >= FRAME.frame_size) {
                goto done;
            }
            vm.stack[vm.sp] = vm.stack[FRAME.base + a];
            vm.sp++;
            break;
        case MFL_OP_SET_LOCAL:
            NEED(1);
            if (a >= FRAME.frame_size) {
                goto done;
            }
            vm.stack[FRAME.base + a] = vm.stack[--vm.sp];
            break;

        case MFL_OP_ADD:
        case MFL_OP_SUB:
        case MFL_OP_MOD:
        case MFL_OP_LT:
        case MFL_OP_LE:
        case MFL_OP_GT:
        case MFL_OP_GE: {
            MflValue *x;
            MflValue *y;
            double p;
            double q;
            int compare = op != MFL_OP_ADD && op != MFL_OP_SUB && op != MFL_OP_MOD;
            NEED(2);
            x = &vm.stack[vm.sp - 2];
            y = &vm.stack[vm.sp - 1];
            if (x->kind != MFL_VAL_NUMBER || y->kind != MFL_VAL_NUMBER) {
                TYPE_NAME(x, ta);
                TYPE_NAME(y, tb);
                FAIL("operator '%s' needs numbers, found %s and %s", op_symbol(op), ta, tb);
            }
            if (!mfl_dim_equal(x->dim, y->dim)) {
                TYPE_NAME(x, ta);
                TYPE_NAME(y, tb);
                FAIL("%s %s and %s: dimensions differ",
                     op == MFL_OP_ADD   ? "cannot add"
                     : op == MFL_OP_SUB ? "cannot subtract"
                     : op == MFL_OP_MOD ? "cannot take the remainder of"
                                        : "cannot compare",
                     ta, tb);
            }
            if (compare) {
                int r;
                if (x->unit == y->unit) {
                    p = x->as.number;
                    q = y->as.number;
                } else {
                    p = si_abs(c, x);
                    q = si_abs(c, y);
                }
                r = op == MFL_OP_LT ? p < q : op == MFL_OP_LE ? p <= q : op == MFL_OP_GT ? p > q : p >= q;
                *x = boolean(r);
                vm.sp--;
                break;
            }
            if (x->affine || y->affine) {
                if (op == MFL_OP_MOD) {
                    FAIL("cannot take the remainder of an absolute temperature in degC; convert it "
                         "with .to(K) first");
                }
                if (x->affine && y->affine) {
                    if (op == MFL_OP_ADD) {
                        FAIL("cannot add two absolute temperatures (degC + degC); add a "
                             "difference in K or delta_degC instead");
                    }
                    /* degC - degC: a difference in K */
                    p = x->unit == y->unit
                            ? mfl_scale_convert(x->as.number - y->as.number, scale_of(c, x->unit),
                                                mfl_scale_one())
                            : si_abs(c, x) - si_abs(c, y);
                    *x = number(p, MFL_NO_UNIT, x->dim, 0);
                } else if (x->affine) {
                    /* degC +- difference */
                    q = mfl_scale_convert(si_diff(c, y), mfl_scale_one(), scale_of(c, x->unit));
                    x->as.number = op == MFL_OP_ADD ? x->as.number + q : x->as.number - q;
                } else {
                    if (op == MFL_OP_SUB) {
                        FAIL("cannot subtract an absolute temperature (degC) from a difference");
                    }
                    /* difference + degC */
                    p = mfl_scale_convert(si_diff(c, x), mfl_scale_one(), scale_of(c, y->unit));
                    *x = number(y->as.number + p, y->unit, y->dim, 1);
                }
                vm.sp--;
                break;
            }
            if (x->unit == y->unit) {
                p = x->as.number;
                q = y->as.number;
                *x = number(op == MFL_OP_ADD ? p + q : op == MFL_OP_SUB ? p - q : fmod(p, q), x->unit,
                            x->dim, 0);
            } else {
                p = si_diff(c, x);
                q = si_diff(c, y);
                *x = number(op == MFL_OP_ADD ? p + q : op == MFL_OP_SUB ? p - q : fmod(p, q),
                            MFL_NO_UNIT, x->dim, 0);
            }
            vm.sp--;
            break;
        }
        case MFL_OP_MUL:
        case MFL_OP_DIV:
        case MFL_OP_POW: {
            MflValue *x;
            MflValue *y;
            MflDim dim;
            NEED(2);
            x = &vm.stack[vm.sp - 2];
            y = &vm.stack[vm.sp - 1];
            if (x->kind != MFL_VAL_NUMBER || y->kind != MFL_VAL_NUMBER) {
                TYPE_NAME(x, ta);
                TYPE_NAME(y, tb);
                FAIL("operator '%s' needs numbers, found %s and %s", op_symbol(op), ta, tb);
            }
            if (x->affine || y->affine) {
                FAIL("cannot %s an absolute temperature in degC; convert it with .to(K) first, or "
                     "use a difference (K or delta_degC)",
                     op == MFL_OP_MUL ? "multiply" : op == MFL_OP_DIV ? "divide" : "raise to a power");
            }
            if (op == MFL_OP_POW) {
                double e = y->as.number;
                if (!mfl_dim_is_none(y->dim)) {
                    TYPE_NAME(y, tb);
                    FAIL("exponent must be dimensionless, found %s", tb);
                }
                if (mfl_dim_is_none(x->dim)) {
                    dim = x->dim;
                } else if (e != floor(e) || fabs(e) > 127 ||
                           !mfl_dim_pow(x->dim, (long)e, &dim)) {
                    char num[MFL_NUMBER_BUF];
                    TYPE_NAME(x, ta);
                    mfl_number_format(e, num, sizeof num);
                    FAIL("the exponent of a %s must be a whole number (small enough to keep the "
                         "dimension valid), found %s",
                         ta, num);
                }
                *x = number(pow(si_diff(c, x), si_diff(c, y)), MFL_NO_UNIT, dim, 0);
            } else {
                if (!(op == MFL_OP_MUL ? mfl_dim_mul(x->dim, y->dim, &dim)
                                       : mfl_dim_div(x->dim, y->dim, &dim))) {
                    FAIL("dimension exponent out of range");
                }
                *x = number(op == MFL_OP_MUL ? si_diff(c, x) * si_diff(c, y)
                                             : si_diff(c, x) / si_diff(c, y),
                            MFL_NO_UNIT, dim, 0);
            }
            vm.sp--;
            break;
        }
        case MFL_OP_RANGE_STEP: {
            double next;
            NEED(1);
            if (TOP.kind != MFL_VAL_NUMBER) {
                goto done;
            }
            next = TOP.as.number + 1.0;
            if (!(next > TOP.as.number)) {
                FAIL("numeric for loop cannot advance its counter by 1");
            }
            TOP.as.number = next;
            break;
        }
        case MFL_OP_NEG:
            NEED(1);
            if (TOP.kind != MFL_VAL_NUMBER) {
                TYPE_NAME(&TOP, ta);
                FAIL("operator '-' needs a number, found %s", ta);
            }
            if (TOP.affine) {
                FAIL("cannot negate an absolute temperature in degC; convert it with .to(K) first");
            }
            TOP.as.number = -TOP.as.number;
            break;
        case MFL_OP_NOT:
            NEED(1);
            if (TOP.kind != MFL_VAL_BOOL) {
                TYPE_NAME(&TOP, ta);
                FAIL("'not' needs a Bool, found %s", ta);
            }
            TOP.as.boolean = !TOP.as.boolean;
            break;

        case MFL_OP_EQ:
        case MFL_OP_NE: {
            MflValue *x;
            MflValue *y;
            int eq = 0;
            NEED(2);
            x = &vm.stack[vm.sp - 2];
            y = &vm.stack[vm.sp - 1];
            if (x->kind == MFL_VAL_ARRAY || y->kind == MFL_VAL_ARRAY) {
                FAIL("arrays cannot be compared with '%s' (compare their elements)", op_symbol(op));
            }
            if (x->kind == MFL_VAL_NIL || y->kind == MFL_VAL_NIL) {
                eq = x->kind == y->kind;
            } else if (x->kind != y->kind) {
                TYPE_NAME(x, ta);
                TYPE_NAME(y, tb);
                FAIL("cannot compare %s and %s", ta, tb);
            } else {
                switch (x->kind) {
                case MFL_VAL_BOOL: eq = x->as.boolean == y->as.boolean; break;
                case MFL_VAL_STRING:
                    eq = x->as.string.length == y->as.string.length &&
                         memcmp(x->as.string.chars, y->as.string.chars, x->as.string.length) == 0;
                    break;
                case MFL_VAL_FUNCTION: eq = x->as.function == y->as.function; break;
                case MFL_VAL_NUMBER:
                    if (!mfl_dim_equal(x->dim, y->dim)) {
                        TYPE_NAME(x, ta);
                        TYPE_NAME(y, tb);
                        FAIL("cannot compare %s and %s: dimensions differ", ta, tb);
                    }
                    eq = x->unit == y->unit ? x->as.number == y->as.number
                                            : si_abs(c, x) == si_abs(c, y);
                    break;
                default: eq = 0; break;
                }
            }
            *x = boolean(op == MFL_OP_EQ ? eq : !eq);
            vm.sp--;
            break;
        }

        case MFL_OP_JUMP:
            if (ip + a > c->code_len) {
                goto done;
            }
            ip += a;
            break;
        case MFL_OP_JUMP_IF_FALSE:
        case MFL_OP_JUMP_IF_TRUE:
            NEED(1);
            if (ip + a > c->code_len) {
                goto done;
            }
            if (TOP.kind != MFL_VAL_BOOL) {
                TYPE_NAME(&TOP, ta);
                FAIL("expected a Bool (a condition, or an operand of 'and'/'or'), found %s", ta);
            }
            if (TOP.as.boolean == (op == MFL_OP_JUMP_IF_TRUE)) {
                ip += a;
            }
            break;
        case MFL_OP_LOOP:
            if (a > ip) {
                goto done;
            }
            ip -= a;
            break;

        case MFL_OP_CONVERT:
        case MFL_OP_VALUE_IN:
            NEED(1);
            if (a >= c->unit_len) {
                goto done;
            }
            if (TOP.kind != MFL_VAL_NUMBER) {
                TYPE_NAME(&TOP, ta);
                FAIL("'.%s' needs a number or quantity, found %s",
                     op == MFL_OP_CONVERT ? "to" : "value_in", ta);
            }
            if (!mfl_dim_equal(TOP.dim, c->units[a].dim)) {
                TYPE_NAME(&TOP, ta);
                FAIL("cannot convert %s to %s: dimensions differ", ta, c->units[a].text);
            }
            TOP.as.number = in_unit(c, &TOP, (int)a);
            if (op == MFL_OP_CONVERT) {
                TOP.unit = (int)a;
                TOP.affine = c->units[a].affine;
            } else {
                TOP.unit = MFL_NO_UNIT;
                TOP.dim = mfl_dim_none();
                TOP.affine = 0;
            }
            break;

        case MFL_OP_CALL: {
            MflValue *callee;
            const MflFunction *fn;
            size_t base;
            size_t k;
            NEED((size_t)a + 1);
            callee = &vm.stack[vm.sp - a - 1];
            if (callee->kind != MFL_VAL_FUNCTION) {
                TYPE_NAME(callee, ta);
                FAIL("cannot call a value of type %s", ta);
            }
            if ((size_t)callee->as.function >= c->function_count) {
                goto done;
            }
            fn = &c->functions[callee->as.function];
            if ((int)a != fn->arity) {
                FAIL("'%s' takes %d argument%s, but %u %s given", fn->name, fn->arity,
                     fn->arity == 1 ? "" : "s", a, a == 1 ? "was" : "were");
            }
            if (vm.frame_count >= MFL_VM_MAX_CALL_DEPTH) {
                FAIL("call depth limit of %d exceeded (endless recursion?)", MFL_VM_MAX_CALL_DEPTH);
            }
            if (vm.frame_count == vm.frame_cap) {
                int cap = vm.frame_cap * 2;
                Frame *p;
                if (!charge(&vm, (size_t)(cap - vm.frame_cap) * sizeof(Frame)) ||
                    (p = (Frame *)realloc(vm.frames, (size_t)cap * sizeof(Frame))) == NULL) {
                    status = MFL_VM_OUT_OF_MEMORY;
                    goto done;
                }
                vm.frames = p;
                vm.frame_cap = cap;
            }
            if (fn->frame_size < fn->arity || fn->entry >= c->code_len) {
                goto done;
            }
            base = vm.sp - a;
            if (!ensure_stack(&vm, base + (size_t)fn->frame_size + fn->max_stack + 1)) {
                status = MFL_VM_OUT_OF_MEMORY;
                goto done;
            }
            for (k = a; k < (size_t)fn->frame_size; k++) {
                vm.stack[base + k] = nil_value();
            }
            vm.sp = base + (size_t)fn->frame_size;
            vm.frames[vm.frame_count].fn = vm.stack[base - 1].as.function;
            vm.frames[vm.frame_count].base = base;
            vm.frames[vm.frame_count].frame_size = (size_t)fn->frame_size;
            vm.frames[vm.frame_count].return_ip = ip;
            vm.frame_count++;
            ip = fn->entry;
            break;
        }
        case MFL_OP_RETURN: {
            MflValue result;
            size_t base;
            if (vm.frame_count <= 1) {
                goto done;
            }
            NEED(1);
            result = vm.stack[vm.sp - 1];
            base = FRAME.base;
            ip = FRAME.return_ip;
            vm.frame_count--;
            vm.sp = base - 1; /* drop the callee too */
            vm.stack[vm.sp++] = result;
            break;
        }

        case MFL_OP_PRINT: {
            size_t n = a;
            size_t k;
            NEED(n);
            for (k = 0; k < n; k++) {
                if (k > 0) {
                    write(ctx, " ", 1);
                }
                format_value(c, &vm.stack[vm.sp - n + k], write, ctx, 0);
            }
            write(ctx, "\n", 1);
            vm.sp -= n;
            ROOM();
            vm.stack[vm.sp++] = nil_value();
            break;
        }
        case MFL_OP_LEN:
            NEED(1);
            if (TOP.kind != MFL_VAL_ARRAY) {
                TYPE_NAME(&TOP, ta);
                FAIL("'len' needs an array, found %s", ta);
            }
            TOP = number((double)TOP.as.array->length, MFL_NO_UNIT, mfl_dim_none(), 0);
            break;
        case MFL_OP_PUSH: {
            MflValue *arr;
            MflValue *val;
            MflArray *A;
            NEED(2);
            arr = &vm.stack[vm.sp - 2];
            val = &vm.stack[vm.sp - 1];
            if (arr->kind != MFL_VAL_ARRAY) {
                TYPE_NAME(arr, ta);
                FAIL("'push' needs an array as its first argument, found %s", ta);
            }
            A = arr->as.array;
            if (A->length > 0 && !same_type(&A->items[0], val)) {
                TYPE_NAME(val, ta);
                TYPE_NAME(&A->items[0], tb);
                FAIL("cannot push %s into an array of %s (elements must have the same type and "
                     "dimension)",
                     ta, tb);
            }
            if (!array_reserve(&vm, A, A->length + 1)) {
                status = MFL_VM_OUT_OF_MEMORY;
                goto done;
            }
            A->items[A->length++] = *val;
            vm.sp -= 2;
            vm.stack[vm.sp++] = nil_value();
            break;
        }
        case MFL_OP_ARRAY: {
            MflArray *A;
            size_t n = a;
            size_t k;
            NEED(n);
            for (k = 1; k < n; k++) {
                if (!same_type(&vm.stack[vm.sp - n], &vm.stack[vm.sp - n + k])) {
                    TYPE_NAME(&vm.stack[vm.sp - n + k], ta);
                    TYPE_NAME(&vm.stack[vm.sp - n], tb);
                    FAIL("array elements must all have the same type and dimension: element %lu "
                         "is %s, but the first element is %s",
                         (unsigned long)(k + 1), ta, tb);
                }
            }
            A = new_array(&vm, n);
            if (A == NULL) {
                status = MFL_VM_OUT_OF_MEMORY;
                goto done;
            }
            if (n > 0) {
                memcpy(A->items, &vm.stack[vm.sp - n], n * sizeof(MflValue));
            }
            A->length = n;
            vm.sp -= n;
            ROOM();
            vm.stack[vm.sp] = nil_value();
            vm.stack[vm.sp].kind = MFL_VAL_ARRAY;
            vm.stack[vm.sp].as.array = A;
            vm.sp++;
            break;
        }
        case MFL_OP_INDEX: {
            size_t k = 0;
            MflVmStatus st;
            NEED(2);
            st = check_index(&vm, &vm.stack[vm.sp - 2], &vm.stack[vm.sp - 1], &k, 0);
            if (st != MFL_VM_OK) {
                status = st;
                goto done;
            }
            vm.stack[vm.sp - 2] = vm.stack[vm.sp - 2].as.array->items[k];
            vm.sp--;
            break;
        }
        case MFL_OP_SET_INDEX: {
            size_t k = 0;
            MflVmStatus st;
            MflArray *A;
            const MflValue *val;
            NEED(3);
            st = check_index(&vm, &vm.stack[vm.sp - 3], &vm.stack[vm.sp - 2], &k, 0);
            if (st != MFL_VM_OK) {
                status = st;
                goto done;
            }
            A = vm.stack[vm.sp - 3].as.array;
            val = &vm.stack[vm.sp - 1];
            {
                const MflValue *ref = &A->items[(k == 0 && A->length > 1) ? 1 : 0];
                if (!same_type(ref, val)) {
                    TYPE_NAME(val, ta);
                    TYPE_NAME(ref, tb);
                    FAIL("cannot store %s in an array of %s (elements must have the same type and "
                         "dimension)",
                         ta, tb);
                }
            }
            A->items[k] = *val;
            vm.sp -= 3;
            break;
        }

        case MFL_OP_CHECK_TYPE:
            NEED(1);
            if (a >= c->type_len || b >= c->const_len) {
                goto done;
            }
            if (!matches_desc(c, &TOP, (int)a, 0)) {
                const MflValue *ctxv = &c->constants[b];
                TYPE_NAME(&TOP, ta);
                mfl_type_desc_name(c, (int)a, tb, sizeof tb);
                FAIL("%.*s must be %s, found %s",
                     ctxv->kind == MFL_VAL_STRING ? (int)ctxv->as.string.length : 5,
                     ctxv->kind == MFL_VAL_STRING ? ctxv->as.string.chars : "value", tb, ta);
            }
            break;
        case MFL_OP_CHECK_LIKE_GLOBAL:
        case MFL_OP_CHECK_LIKE_LOCAL: {
            const MflValue *cur;
            NEED(1);
            if (b >= c->const_len) {
                goto done;
            }
            if (op == MFL_OP_CHECK_LIKE_GLOBAL) {
                if (a >= c->global_count) {
                    goto done;
                }
                cur = &vm.globals[a];
            } else {
                if (a >= FRAME.frame_size) {
                    goto done;
                }
                cur = &vm.stack[FRAME.base + a];
            }
            if (cur->kind != MFL_VAL_NIL && cur->kind != MFL_VAL_UNSET && !same_type(cur, &TOP)) {
                const MflValue *ctxv = &c->constants[b];
                TYPE_NAME(&TOP, ta);
                TYPE_NAME(cur, tb);
                FAIL("cannot assign %s to %.*s, which holds a %s (a variable keeps its type and "
                     "dimension)",
                     ta, ctxv->kind == MFL_VAL_STRING ? (int)ctxv->as.string.length : 1,
                     ctxv->kind == MFL_VAL_STRING ? ctxv->as.string.chars : "?", tb);
            }
            break;
        }

        case MFL_OP_CALL_NATIVE: {
            MflNativeEnv env;
            MflValue result;
            char message[sizeof error->message];
            int rc;
            NEED(b);
            memset(&env, 0, sizeof env);
            env.chunk = c;
            env.vm = &vm;
            env.session = (MflNativeSession *)session;
            message[0] = '\0';
            env.message = message;
            env.message_size = sizeof message;
            result = nil_value();
            rc = mfl_native_call(&env, (int)a, &vm.stack[vm.sp - b], (int)b, &result);
            if (rc == 2) {
                status = MFL_VM_OUT_OF_MEMORY;
                goto done;
            }
            if (rc != 0) {
                FAIL("%s", message);
            }
            vm.sp -= b;
            ROOM();
            vm.stack[vm.sp++] = result;
            break;
        }

        case MFL_OP_HALT:
            status = MFL_VM_OK;
            if (snapshot != NULL) {
                memcpy(snapshot, vm.globals, c->global_count * sizeof *vm.globals);
            }
            goto done;

        default:
            goto done;
        }
    }

done:
#undef FRAME
#undef NEED
#undef ROOM
#undef TOP
#undef FAIL
    while (vm.arrays != NULL) {
        MflArray *next = vm.arrays->next_alloc;
        free(vm.arrays->items);
        free(vm.arrays);
        vm.arrays = next;
    }
    free(vm.stack);
    free(vm.globals);
    free(vm.frames);
    return status;
}

MflVmStatus mfl_vm_run(const MflChunk *c, MflWriteFn write, void *ctx, size_t memory_limit)
{
    return mfl_vm_run_globals(c, NULL, 0, NULL, write, ctx, memory_limit);
}

MflVmStatus mfl_vm_run_globals(const MflChunk *c, const MflValue *init, size_t init_count,
                               MflValue *snapshot, MflWriteFn write, void *ctx,
                               size_t memory_limit)
{
    MflVmLimits limits;
    limits.memory_limit = memory_limit;
    limits.max_steps = 0;
    return mfl_vm_execute(c, init, init_count, snapshot, write, ctx, &limits, NULL);
}
