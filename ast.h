/*
 * ast.h - Abstract Syntax Tree for Eezo
 *
 * Lambda calculus with extensions:
 *   - Variables (de Bruijn or named)
 *   - Abstraction (λx.e)
 *   - Application (e₁ e₂)
 *   - Let bindings (let x = e₁ in e₂)
 *   - Literals (numbers, strings)
 *
 * Compiles to SKI via bracket abstraction.
 */
#ifndef AST_H
#define AST_H

#include <libeezo/types.h>

typedef enum {
    AST_VAR,        /* Variable reference */
    AST_ABS,        /* Lambda abstraction */
    AST_APP,        /* Application */
    AST_LET,        /* Let binding */
    AST_NUM,        /* Numeric literal */
    AST_STR,        /* String literal */
    AST_S,          /* S combinator (primitive) */
    AST_K,          /* K combinator (primitive) */
    AST_I,          /* I combinator (primitive) */
} AstTag;

typedef struct Ast Ast;

/* Source location for error messages */
typedef struct {
    const char *file;
    u32 line;
    u32 col;
} SrcLoc;

/* Interned string for variable names */
typedef struct {
    const char *str;
    u32 len;
} Symbol;

struct Ast {
    AstTag tag;
    SrcLoc loc;
    union {
        /* AST_VAR: variable */
        struct {
            Symbol name;
            i32 debruijn;   /* -1 if free/unresolved, >=0 if bound */
        } var;
        
        /* AST_ABS: λ name . body */
        struct {
            Symbol param;
            Ast *body;
        } abs;
        
        /* AST_APP: func arg */
        struct {
            Ast *func;
            Ast *arg;
        } app;
        
        /* AST_LET: let name = value in body */
        struct {
            Symbol name;
            Ast *value;
            Ast *body;
        } let;
        
        /* AST_NUM: numeric literal */
        i64 num;
        
        /* AST_STR: string literal */
        struct {
            const char *data;
            u32 len;
        } str;
    };
};

/* AST allocation pool */
typedef struct {
    Ast *pool;
    u32 capacity;
    u32 next;
} AstPool;

void ast_pool_init(AstPool *p, u32 capacity);
void ast_pool_free(AstPool *p);

/* Constructors */
Ast *ast_var(AstPool *p, SrcLoc loc, Symbol name);
Ast *ast_debruijn(AstPool *p, SrcLoc loc, u32 index);  /* Variable with de Bruijn index */
Ast *ast_abs(AstPool *p, SrcLoc loc, Symbol param, Ast *body);
Ast *ast_app(AstPool *p, SrcLoc loc, Ast *func, Ast *arg);
Ast *ast_let(AstPool *p, SrcLoc loc, Symbol name, Ast *value, Ast *body);
Ast *ast_num(AstPool *p, SrcLoc loc, i64 n);
Ast *ast_str(AstPool *p, SrcLoc loc, const char *data, u32 len);
Ast *ast_s(AstPool *p, SrcLoc loc);
Ast *ast_k(AstPool *p, SrcLoc loc);
Ast *ast_i(AstPool *p, SrcLoc loc);

/* Debug */
void ast_print(Ast *a);

#endif /* AST_H */
