#include "bracket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bracket abstraction: convert lambda calculus to SKI
 * 
 * Uses the standard rules:
 *   [x]x       = I
 *   [x]c       = K c     (c is constant or different variable)
 *   [x](E F)   = S ([x]E) ([x]F)
 *
 * This implementation uses named variables (Symbol) rather than de Bruijn.
 * The param field of AST_ABS tells us what variable to abstract.
 */

static SrcLoc noloc = {NULL, 0, 0};

/* Check if two symbols are equal */
static int sym_eq(Symbol a, Symbol b) {
    return a.len == b.len && (a.str == b.str || 
           (a.len > 0 && memcmp(a.str, b.str, a.len) == 0));
}

/*
 * Resolve variable names to de Bruijn indices.
 * env is a linked list of bound variables.
 */
typedef struct Env {
    Symbol name;
    struct Env *next;
} Env;

static int env_lookup(Env *env, Symbol name) {
    int i = 0;
    while (env) {
        if (sym_eq(env->name, name)) return i;
        env = env->next;
        i++;
    }
    return -1;  /* Not found */
}

static bool resolve_rec(Ast *ast, Env *env) {
    switch (ast->tag) {
    case AST_VAR: {
        int idx = env_lookup(env, ast->var.name);
        if (idx < 0) {
            fprintf(stderr, "Error: undefined variable '%.*s' at line %u\n",
                    ast->var.name.len, ast->var.name.str, ast->loc.line);
            return false;
        }
        ast->var.debruijn = idx;
        return true;
    }
    case AST_S:
    case AST_K:
    case AST_I:
    case AST_NUM:
    case AST_STR:
        return true;
    case AST_APP:
        return resolve_rec(ast->app.func, env) && resolve_rec(ast->app.arg, env);
    case AST_ABS: {
        Env new_env = { ast->abs.param, env };
        return resolve_rec(ast->abs.body, &new_env);
    }
    case AST_LET: {
        /* let x = v in e  =>  resolve v in current env, resolve e with x bound */
        if (!resolve_rec(ast->let.value, env)) return false;
        Env new_env = { ast->let.name, env };
        return resolve_rec(ast->let.body, &new_env);
    }
    }
    return false;
}

bool bracket_resolve(Ast *ast) {
    return resolve_rec(ast, NULL);
}

/* Forward declarations */
static Ast* abstract_name(AstPool *pool, Ast *body, Symbol name);
static Ast* ast_to_comb(AstPool *pool, Ast *ast);
static SKITerm* comb_to_term(SKIPool *pool, Ast *comb);
static bool occurs_free(Ast *ast, Symbol name);

/* Check if variable 'name' occurs free in AST */
static bool occurs_free(Ast *ast, Symbol name) {
    switch (ast->tag) {
    case AST_VAR:
        return sym_eq(ast->var.name, name);
    case AST_S:
    case AST_K:
    case AST_I:
    case AST_NUM:
    case AST_STR:
        return false;
    case AST_APP:
        return occurs_free(ast->app.func, name) || occurs_free(ast->app.arg, name);
    case AST_ABS:
        /* Bound by this abstraction - not free */
        if (sym_eq(ast->abs.param, name)) return false;
        return occurs_free(ast->abs.body, name);
    case AST_LET:
        if (sym_eq(ast->let.name, name)) return occurs_free(ast->let.value, name);
        return occurs_free(ast->let.value, name) || occurs_free(ast->let.body, name);
    }
    return false;
}

/* 
 * abstract_name: perform [name]body - abstract variable 'name' from body
 * Returns an AST containing only combinators and free variables
 *
 * Optimizations applied:
 *   η: [x](E x) = E   if x ∉ FV(E)
 *   S(K p)(K q) = K(p q)  -- applied post-hoc
 *   S(K p) I = p          -- applied post-hoc
 */
static Ast* abstract_name(AstPool *pool, Ast *body, Symbol name) {
    switch (body->tag) {
    case AST_VAR:
        if (sym_eq(body->var.name, name)) {
            return ast_i(pool, noloc);  /* [x]x = I */
        } else {
            /* Different variable, wrap in K */
            return ast_app(pool, noloc, ast_k(pool, noloc), body);
        }
    
    case AST_S:
        return ast_app(pool, noloc, ast_k(pool, noloc), ast_s(pool, noloc));
    case AST_K:
        return ast_app(pool, noloc, ast_k(pool, noloc), ast_k(pool, noloc));
    case AST_I:
        return ast_app(pool, noloc, ast_k(pool, noloc), ast_i(pool, noloc));
    
    case AST_APP: {
        /* η-optimization: [x](E x) = E if x ∉ FV(E) */
        if (body->app.arg->tag == AST_VAR && 
            sym_eq(body->app.arg->var.name, name) &&
            !occurs_free(body->app.func, name)) {
            return body->app.func;
        }
        
        Ast *f = abstract_name(pool, body->app.func, name);
        Ast *a = abstract_name(pool, body->app.arg, name);
        if (!f || !a) return NULL;
        
        /* S-optimizations on the result:
         * S (K p) (K q) = K (p q)
         * S (K p) I = p
         * S K x = I (for any x, since S K x y z = K z (x z) = z = I y z)
         */
        
        /* Check for S (K p) I = p */
        if (a->tag == AST_I && 
            f->tag == AST_APP && f->app.func->tag == AST_K) {
            return f->app.arg;
        }
        
        /* Check for S (K p) (K q) = K (p q) */
        if (f->tag == AST_APP && f->app.func->tag == AST_K &&
            a->tag == AST_APP && a->app.func->tag == AST_K) {
            Ast *pq = ast_app(pool, noloc, f->app.arg, a->app.arg);
            return ast_app(pool, noloc, ast_k(pool, noloc), pq);
        }
        
        /* [x](E F) = S ([x]E) ([x]F) */
        return ast_app(pool, noloc, 
                      ast_app(pool, noloc, ast_s(pool, noloc), f), 
                      a);
    }
    
    case AST_ABS:
        /* [x](λy.E) = [x]([y]E) 
         * First abstract inner variable, then outer
         */
        {
            Ast *inner = abstract_name(pool, body->abs.body, body->abs.param);
            if (!inner) return NULL;
            return abstract_name(pool, inner, name);
        }
    
    case AST_LET:
        fprintf(stderr, "Error: LET should be desugared before bracket abstraction\n");
        return NULL;
    
    case AST_NUM:
    case AST_STR:
        fprintf(stderr, "Error: literals should be desugared before bracket abstraction\n");
        return NULL;
    }
    return NULL;
}

/* Convert AST to combinator-only AST */
static Ast* ast_to_comb(AstPool *pool, Ast *ast) {
    switch (ast->tag) {
    case AST_VAR:
        /* Free variable - error for closed terms */
        fprintf(stderr, "Error: free variable '%.*s' in term\n",
                ast->var.name.len, ast->var.name.str);
        return NULL;
    
    case AST_S:
    case AST_K:
    case AST_I:
        return ast;
    
    case AST_APP: {
        Ast *f = ast_to_comb(pool, ast->app.func);
        Ast *a = ast_to_comb(pool, ast->app.arg);
        if (!f || !a) return NULL;
        return ast_app(pool, noloc, f, a);
    }
    
    case AST_ABS: {
        /* λparam.body  -->  [param](ast_to_comb(body, but keep param free)) 
         * Actually simpler: abstract the param from the body directly
         */
        Ast *inner = abstract_name(pool, ast->abs.body, ast->abs.param);
        if (!inner) return NULL;
        /* Now inner still might have lambdas, recurse */
        return ast_to_comb(pool, inner);
    }
    
    case AST_LET:
        fprintf(stderr, "Error: LET should be desugared\n");
        return NULL;
    
    case AST_NUM: {
        /* Church numeral: λf.λx.f^n x 
         * 0 = λf.λx.x = K I
         * 1 = λf.λx.f x 
         * 2 = λf.λx.f (f x)
         * etc.
         */
        i64 n = ast->num;
        if (n < 0) {
            fprintf(stderr, "Error: negative numbers not supported\n");
            return NULL;
        }
        
        /* Build f^n x as AST */
        Symbol f_sym = { "f", 1 };
        Symbol x_sym = { "x", 1 };
        
        Ast *body = ast_var(pool, noloc, x_sym);  /* Start with x */
        for (i64 i = 0; i < n; i++) {
            body = ast_app(pool, noloc, ast_var(pool, noloc, f_sym), body);  /* f^(i+1) x */
        }
        
        /* λx. body */
        Ast *lx = ast_abs(pool, noloc, x_sym, body);
        /* λf. λx. body */
        Ast *lf = ast_abs(pool, noloc, f_sym, lx);
        
        return ast_to_comb(pool, lf);
    }
    
    case AST_STR:
        fprintf(stderr, "Error: strings should be desugared\n");
        return NULL;
    }
    return NULL;
}

/* Convert combinator-only AST to SKITerm */
static SKITerm* comb_to_term(SKIPool *pool, Ast *comb) {
    switch (comb->tag) {
    case AST_S:
        return ski_s(pool);
    case AST_K:
        return ski_k(pool);
    case AST_I:
        return ski_i(pool);
    case AST_APP: {
        SKITerm *f = comb_to_term(pool, comb->app.func);
        SKITerm *a = comb_to_term(pool, comb->app.arg);
        if (!f || !a) return NULL;
        return ski_app(pool, f, a);
    }
    case AST_VAR:
        fprintf(stderr, "Error: variable '%.*s' in combinator term\n",
                comb->var.name.len, comb->var.name.str);
        return NULL;
    case AST_ABS:
        fprintf(stderr, "Error: abstraction in combinator term\n");
        return NULL;
    case AST_LET:
        fprintf(stderr, "Error: let in combinator term\n");
        return NULL;
    case AST_NUM:
    case AST_STR:
        fprintf(stderr, "Error: literal in combinator term\n");
        return NULL;
    }
    return NULL;
}

/* Main entry point */
SKITerm* bracket_compile(AstPool *ast_pool, SKIPool *ski_pool, Ast *ast) {
    Ast *comb = ast_to_comb(ast_pool, ast);
    if (!comb) return NULL;
    return comb_to_term(ski_pool, comb);
}
