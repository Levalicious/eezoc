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
        /* A literal has no free variables: [x]n = K n (n is expanded by ast_to_comb) */
        return ast_app(pool, noloc, ast_k(pool, noloc), body);
    
    case AST_STR:
        fprintf(stderr, "Error: string literals should be desugared before bracket abstraction\n");
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
        /* Church numeral, built by BINARY expansion so the term is
         * O(log n) in size instead of the O(n) unary f^n x.
         *
         *   ZERO = λf.λx.x
         *   ONE  = λf.λx.f x
         *   DBL  = λm.λf.λx. m f (m f x)      (add m m)
         *   SUCC = λm.λf.λx. f (m f x)
         *
         * n is read MSB-first: acc = ONE; for each lower bit:
         *   acc = DBL acc;  if bit set: acc = SUCC acc.
         * DBL and SUCC are closed lambdas applied to acc, so each step
         * adds a constant-size combinator and one application node; the
         * value is extensionally the same Church numeral as before.
         */
        i64 n = ast->num;
        if (n < 0) {
            fprintf(stderr, "Error: negative numbers not supported\n");
            return NULL;
        }
        
        Symbol f_sym = { "f", 1 };
        Symbol x_sym = { "x", 1 };
        Symbol m_sym = { "m", 1 };
        
        #define NUM_VAR(sym) ast_var(pool, noloc, (sym))
        #define NUM_APP(a, b) ast_app(pool, noloc, (a), (b))
        #define NUM_ABS(sym, body) ast_abs(pool, noloc, (sym), (body))
        
        Ast *acc;
        if (n == 0) {
            /* λf.λx.x */
            acc = NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_VAR(x_sym)));
        } else {
            /* λf.λx.f x */
            acc = NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_APP(NUM_VAR(f_sym), NUM_VAR(x_sym))));
            int msb = 63 - __builtin_clzll((u64)n);
            for (int i = msb - 1; i >= 0; i--) {
                /* DBL acc: λm.λf.λx. m f (m f x) */
                Ast *mf1 = NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym));
                Ast *mf2 = NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym));
                Ast *dbl = NUM_ABS(m_sym, NUM_ABS(f_sym, NUM_ABS(x_sym,
                               NUM_APP(mf1, NUM_APP(mf2, NUM_VAR(x_sym))))));
                acc = NUM_APP(dbl, acc);
                if ((n >> i) & 1) {
                    /* SUCC acc: λm.λf.λx. f (m f x) */
                    Ast *mfx = NUM_APP(NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym)), NUM_VAR(x_sym));
                    Ast *succ = NUM_ABS(m_sym, NUM_ABS(f_sym, NUM_ABS(x_sym,
                                    NUM_APP(NUM_VAR(f_sym), mfx))));
                    acc = NUM_APP(succ, acc);
                }
            }
        }
        
        #undef NUM_VAR
        #undef NUM_APP
        #undef NUM_ABS
        
        return ast_to_comb(pool, acc);
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
