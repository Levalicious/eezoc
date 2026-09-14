#include <libeezo/res.h>
/*
 * ast.c - AST implementation
 */
#include "ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void ast_pool_init(AstPool *p, u32 capacity) {
    p->pool = rcalloc(capacity, sizeof(Ast));
    p->capacity = capacity;
    p->next = 0;
}

void ast_pool_free(AstPool *p) {
    free(p->pool);
    p->pool = NULL;
    p->capacity = 0;
}

static Ast *ast_alloc(AstPool *p) {
    if (p->next >= p->capacity) {
        fprintf(stderr, "Error: AST pool exhausted (%u nodes)\n", p->capacity);
        return NULL;
    }
    return &p->pool[p->next++];
}

Ast *ast_var(AstPool *p, SrcLoc loc, Symbol name) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_VAR;
    a->loc = loc;
    a->var.name = name;
    a->var.debruijn = -1;
    return a;
}

Ast *ast_debruijn(AstPool *p, SrcLoc loc, u32 index) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_VAR;
    a->loc = loc;
    a->var.name = (Symbol){ .str = "_", .len = 1 };
    a->var.debruijn = (i32)index;
    return a;
}

Ast *ast_abs(AstPool *p, SrcLoc loc, Symbol param, Ast *body) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_ABS;
    a->loc = loc;
    a->abs.param = param;
    a->abs.body = body;
    return a;
}

Ast *ast_app(AstPool *p, SrcLoc loc, Ast *func, Ast *arg) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_APP;
    a->loc = loc;
    a->app.func = func;
    a->app.arg = arg;
    return a;
}

Ast *ast_let(AstPool *p, SrcLoc loc, Symbol name, Ast *value, Ast *body) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_LET;
    a->loc = loc;
    a->let.name = name;
    a->let.value = value;
    a->let.body = body;
    return a;
}

Ast *ast_num(AstPool *p, SrcLoc loc, i64 n) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_NUM;
    a->loc = loc;
    a->num = n;
    return a;
}

Ast *ast_str(AstPool *p, SrcLoc loc, const char *data, u32 len) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_STR;
    a->loc = loc;
    a->str.data = data;
    a->str.len = len;
    return a;
}

Ast *ast_s(AstPool *p, SrcLoc loc) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_S;
    a->loc = loc;
    return a;
}

Ast *ast_k(AstPool *p, SrcLoc loc) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_K;
    a->loc = loc;
    return a;
}

Ast *ast_i(AstPool *p, SrcLoc loc) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_I;
    a->loc = loc;
    return a;
}

Ast *ast_word(AstPool *p, SrcLoc loc, u64 w) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_WORD;
    a->loc = loc;
    a->word = w;
    return a;
}

Ast *ast_prim(AstPool *p, SrcLoc loc, PrimOp op) {
    Ast *a = ast_alloc(p);
    if (!a) return NULL;
    a->tag = AST_PRIM;
    a->loc = loc;
    a->op = op;
    return a;
}

static void print_symbol(Symbol s) {
    printf("%.*s", s.len, s.str);
}

void ast_print(Ast *a) {
    if (!a) {
        printf("NULL");
        return;
    }
    
    switch (a->tag) {
    case AST_VAR:
        print_symbol(a->var.name);
        if (a->var.debruijn >= 0) {
            printf("[%d]", a->var.debruijn);
        }
        break;
    case AST_ABS:
        printf("(λ");
        print_symbol(a->abs.param);
        printf(". ");
        ast_print(a->abs.body);
        printf(")");
        break;
    case AST_APP:
        printf("(");
        ast_print(a->app.func);
        printf(" ");
        ast_print(a->app.arg);
        printf(")");
        break;
    case AST_LET:
        printf("(let ");
        print_symbol(a->let.name);
        printf(" = ");
        ast_print(a->let.value);
        printf(" in ");
        ast_print(a->let.body);
        printf(")");
        break;
    case AST_NUM:
        printf("%lld", (long long)a->num);
        break;
    case AST_STR:
        printf("\"%.*s\"", a->str.len, a->str.data);
        break;
    case AST_S:
        printf("S");
        break;
    case AST_K:
        printf("K");
        break;
    case AST_I:
        printf("I");
        break;
    case AST_B: printf("B"); break;
    case AST_C: printf("C"); break;
    case AST_T: printf("T"); break;
    case AST_R: printf("R"); break;
    case AST_WORD: printf("%lluw", (unsigned long long)a->word); break;
    case AST_PRIM: printf("w%s", prim_name(a->op)); break;
    }
}
