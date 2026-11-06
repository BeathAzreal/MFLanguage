/*
 * ast.h - MFL abstract syntax tree (internal API).
 *
 * All nodes live in an MflArena owned by the parse result. Names and
 * literal texts point into the SOURCE buffer, which must outlive the AST.
 * Literals are kept as source text: numbers are converted to double in a
 * later, locale-independent stage.
 *
 * Unit expressions in quantity literals (`5 mm/s`) reuse the ordinary
 * expression nodes: IDENT for a unit symbol, BINARY '*' '/' '^' for
 * products, quotients and integer powers. Deciding which identifiers are
 * units is the job of the semantic checker, not of the parser.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_AST_H
#define MFL_AST_H

#include <stddef.h>

#include "lexer/lexer.h"
#include "runtime/units.h"

typedef enum {
    MFL_EXPR_ERROR,    /* placeholder produced after a syntax error */
    MFL_EXPR_NUMBER,   /* as.text: digits as written, e.g. "1.5e-3" */
    MFL_EXPR_STRING,   /* as.text: including quotes, escapes not decoded */
    MFL_EXPR_BOOL,     /* as.boolean */
    MFL_EXPR_NIL,
    MFL_EXPR_IDENT,    /* as.text: the name */
    MFL_EXPR_UNARY,    /* as.unary: '-' or 'not' */
    MFL_EXPR_BINARY,   /* as.binary */
    MFL_EXPR_CALL,     /* as.call: callee(args...) */
    MFL_EXPR_MEMBER,   /* as.member: object.name */
    MFL_EXPR_INDEX,    /* as.index: object[index] */
    MFL_EXPR_QUANTITY, /* as.quantity: number followed by a unit expression */
    MFL_EXPR_ARRAY     /* as.array: [e1, e2, ...] (v0.2) */
} MflExprKind;

typedef struct MflExpr MflExpr;
typedef struct MflStmt MflStmt;
typedef struct MflFnSig MflFnSig; /* defined in compiler/checker.h */

typedef struct {
    const char *start;
    size_t      length;
} MflText;

/* Static type, filled in by the semantic checker (src/compiler/checker). */
typedef enum {
    MFL_TYPE_UNCHECKED = 0, /* the checker has not visited the node */
    MFL_TYPE_ERROR,         /* an error was reported; suppresses cascades */
    MFL_TYPE_NUMBER,        /* real number; dim says which quantity (none = plain Number) */
    MFL_TYPE_BOOL,
    MFL_TYPE_STRING,
    MFL_TYPE_NIL,
    MFL_TYPE_UNIT,          /* a unit expression (quantity literal unit, .to argument) */
    MFL_TYPE_ANY,           /* not known before running: checked at run time (v0.2) */
    MFL_TYPE_ARRAY,         /* elem: element type (v0.2) */
    MFL_TYPE_FUNCTION       /* fn: signature (v0.2) */
} MflTypeKind;

typedef struct MflType MflType;

struct MflType {
    MflTypeKind     kind;
    MflDim          dim;    /* NUMBER and UNIT */
    unsigned char   affine; /* NUMBER/UNIT: absolute temperature on an offset scale (degC) */
    const MflType  *elem;   /* ARRAY */
    const MflFnSig *fn;     /* FUNCTION */
};

/* Where a name refers to, decided by the checker. */
typedef enum {
    MFL_REF_NONE = 0,
    MFL_REF_GLOBAL,   /* ref_index: global slot */
    MFL_REF_LOCAL,    /* ref_index: slot in the current function frame */
    MFL_REF_FUNCTION, /* ref_index: function number */
    MFL_REF_BUILTIN,  /* ref_index: MflBuiltin */
    MFL_REF_NATIVE,   /* callee of a module call (numerical.sum): ref_index: native number (v0.3) */
    MFL_REF_CONSTANT  /* module constant (math.pi): ref_index: native number (v0.3) */
} MflRefKind;

typedef enum { MFL_BUILTIN_PRINT, MFL_BUILTIN_LEN, MFL_BUILTIN_PUSH } MflBuiltin;

/* Type annotation as written (v0.2): Number, Bool, String, Quantity[u], Array[T]. */
typedef enum {
    MFL_TE_NUMBER,
    MFL_TE_BOOL,
    MFL_TE_STRING,
    MFL_TE_QUANTITY,
    MFL_TE_ARRAY
} MflTypeExprKind;

typedef struct MflTypeExpr MflTypeExpr;

struct MflTypeExpr {
    MflTypeExprKind kind;
    int             line;
    int             column;
    MflExpr        *unit; /* QUANTITY */
    MflTypeExpr    *elem; /* ARRAY */
};

struct MflExpr {
    MflExprKind kind;
    int         line;   /* position of the token that defines the node */
    int         column; /* (operator token for unary/binary) */
    MflExpr    *next;   /* next argument / array element */
    MflType     type;   /* set by the checker */
    MflScale    unit_scale; /* UNIT-typed nodes: exact factor to coherent SI */
    MflRefKind  ref_kind;   /* IDENT (and callee IDENT): set by the checker */
    int         ref_index;
    union {
        MflText text;
        int     boolean;
        struct {
            MflTokenKind op;
            MflExpr     *operand;
        } unary;
        struct {
            MflTokenKind op;
            MflExpr     *left;
            MflExpr     *right;
        } binary;
        struct {
            MflExpr *callee;
            MflExpr *args; /* linked through ->next */
            int      argc;
        } call;
        struct {
            MflExpr *object;
            MflText  name;
        } member;
        struct {
            MflExpr *object;
            MflExpr *index;
        } index;
        struct {
            MflExpr *value; /* MFL_EXPR_NUMBER */
            MflExpr *unit;
        } quantity;
        struct {
            MflExpr *elems; /* linked through ->next */
            int      count;
        } array;
    } as;
};

typedef enum {
    MFL_STMT_LET,    /* let name (: annotation)? = value                       */
    MFL_STMT_VAR,    /* var name (: annotation)? = value                       */
    MFL_STMT_ASSIGN, /* target = value         (target: IDENT or INDEX)        */
    MFL_STMT_EXPR,   /* value                                                  */
    MFL_STMT_FN,     /* fn name(params) (-> ret_type)? body          (v0.2)    */
    MFL_STMT_RETURN, /* return value?                                (v0.2)    */
    MFL_STMT_IF,     /* if value body (else else_body)?   else_body: BLOCK or IF */
    MFL_STMT_WHILE,  /* while value body                                       */
    MFL_STMT_FOR,    /* for name in value (.. range_end)? body                 */
    MFL_STMT_BLOCK,  /* { body }                                               */
    MFL_STMT_IMPORT  /* import name | import "path.mfl"              (v0.3)    */
} MflStmtKind;

typedef struct MflParam MflParam;

struct MflParam {
    MflText      name;
    MflTypeExpr *type; /* NULL: not annotated (Any) */
    int          line;
    int          column;
    MflParam    *next;
};

struct MflStmt {
    MflStmtKind  kind;
    int          line;
    int          column;
    int          file;       /* top-level statements: index in the program's file table (v0.3) */
    MflStmt     *next;
    MflText      name;       /* LET, VAR, FN, FOR (loop variable); IMPORT: module name, or the
                                path string with its quotes */
    int          import_path; /* IMPORT: 1 for import "path.mfl", 0 for import name */
    MflTypeExpr *annotation; /* LET, VAR */
    MflExpr     *target;     /* ASSIGN */
    MflExpr     *value;      /* LET VAR ASSIGN EXPR RETURN(may be NULL); IF/WHILE condition; FOR iterable or range start */
    MflExpr     *range_end;  /* FOR over a range */
    MflParam    *params;     /* FN */
    int          param_count;
    MflTypeExpr *ret_type;   /* FN: NULL if not annotated */
    MflStmt     *body;       /* FN IF WHILE FOR BLOCK: first statement */
    MflStmt     *else_body;  /* IF */

    /* Filled in by the checker for the compiler. */
    MflType      decl_type;  /* LET/VAR: annotated or inferred type; FN: unused */
    int          is_global;  /* LET/VAR: top-level binding (global slot) */
    int          slot;       /* LET/VAR/FOR loop variable slot; FN: function number */
    int          slot2;      /* FOR: hidden slots (range end / array, index) */
    int          slot3;
    int          frame_size; /* FN: number of local slots, parameters included */
};

typedef struct {
    MflStmt *first; /* linked through ->next */
    int      count;
    int      main_frame_size; /* set by the checker: local slots of top-level blocks */

    /* Programs made of several files (v0.3, see compiler/loader.h). A
     * program from mfl_parse() alone has file_count 0, which means one file
     * (index 0) that sees everything. */
    int                  file_count;
    const char *const   *file_names; /* file_count names, as shown in messages */
    const unsigned char *visible;    /* file_count * file_count: visible[a * file_count + b]
                                        is 1 when file a may use the names of file b */
} MflProgram;

/* Output sink for the printer. */
typedef void (*MflWriteFn)(void *ctx, const char *data, size_t length);

/* Prints the program as S-expressions, one statement per line:
 *   "12:1 (let speed (quantity 5 (/ mm s)))"                           */
void mfl_ast_print_program(const MflProgram *program, MflWriteFn write, void *ctx);
void mfl_ast_print_expr(const MflExpr *expr, MflWriteFn write, void *ctx);

#endif /* MFL_AST_H */
