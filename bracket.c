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
    case AST_B:
    case AST_C:
    case AST_T:
    case AST_R:
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

/* ---- lambda to combinators: Kiselyov's algorithm ----
 *
 * O. Kiselyov, "lambda to SKI, semantically" (FLOPS 2018), in the form given by Ben Lynn: a subterm is converted to a
 * pair (g, d) where d is a combinator term closed under the enclosing lambdas and g is a bit list over de Bruijn indices,
 * g[k] set iff the variable k occurs in the subterm. An abstraction pops the first bit (K when the variable is unused),
 * and an application combines the two pairs by the rules of `kapp` below, choosing B, C, S or plain application by which
 * sides use the innermost variable; the eta rule and the T and R combinators (T x f = f x, R x f y = f y x) come from the
 * eta-optimized variant. The output is linear in the size of the term times its nesting, where the classic
 * Schoenfinkel abstraction ([x](E F) = S [x]E [x]F, innermost binder first) multiplies the size at every enclosing binder
 * whose variable occurs inside: the codes eezott emits nest a dozen binders and exhausted the AST pool under it.
 * B, C, T and R are emitted as their S K terms (comb_to_term), so the encodings and the evaluators are unchanged. */
typedef struct { unsigned char *g; int n; Ast *d; } KT;   /* g[0..n): bit list, index 0 the innermost variable */

static KT kt(unsigned char *g, int n, Ast *d) { KT t = { g, n, d }; return t; }
static KT kt_closed(Ast *d) { return kt(NULL, 0, d); }
static KT kt_tail(KT t) { return kt(t.n > 1 ? t.g + 1 : NULL, t.n > 0 ? t.n - 1 : 0, t.d); }
static int kt_head(KT t) { return t.n > 0 && t.g[0]; }           /* True:g */
static int kt_is_var0(KT t) { return t.n == 1 && t.g[0] && t.d->tag == AST_I; }   /* (True:[], I): the innermost variable itself */
static Ast *kapp_ast(AstPool *p, Ast *f, Ast *a) { return ast_app(p, noloc, f, a); }
static Ast *kcomb(AstPool *p, AstTag tag) { Ast *a = ast_s(p, noloc); a->tag = tag; return a; }

static Ast *kapp(AstPool *p, KT t1, KT t2);
/* the rules of #: how the innermost variable is used on each side decides the combinator */
static Ast *kapp(AstPool *p, KT t1, KT t2) {
    if (t1.n == 0) {
        if (t2.n == 0) return kapp_ast(p, t1.d, t2.d);                                         /* d1 d2 */
        if (kt_is_var0(t2)) return t1.d;                                                        /* eta: \x. d1 x = d1 */
        if (kt_head(t2)) return kapp(p, kt_closed(kapp_ast(p, kcomb(p, AST_B), t1.d)), kt_tail(t2));   /* B d1 . */
        return kapp(p, t1, kt_tail(t2));                                                        /* x unused on the right */
    }
    if (kt_is_var0(t1)) {
        if (t2.n == 0) return kapp_ast(p, kcomb(p, AST_T), t2.d);                              /* \x. x d2 = T d2 */
        if (!kt_head(t2)) return kapp(p, kt_closed(kcomb(p, AST_T)), kt_tail(t2));
    }
    if (kt_head(t1)) {
        if (t2.n == 0) return kapp(p, kt_closed(kapp_ast(p, kcomb(p, AST_R), t2.d)), kt_tail(t1));    /* R d2 . */
        KT t1p = kt_tail(t1);
        if (kt_head(t2)) return kapp(p, kt(t1p.g, t1p.n, kapp(p, kt_closed(kcomb(p, AST_S)), t1p)), kt_tail(t2));   /* S */
        return kapp(p, kt(t1p.g, t1p.n, kapp(p, kt_closed(kcomb(p, AST_C)), t1p)), kt_tail(t2));                  /* C */
    }
    /* x unused on the left */
    KT t1p = kt_tail(t1);
    if (t2.n == 0) return kapp(p, t1p, t2);
    if (kt_is_var0(t2)) return t1.d;                                                            /* eta */
    if (kt_head(t2)) return kapp(p, kt(t1p.g, t1p.n, kapp(p, kt_closed(kcomb(p, AST_B)), t1p)), kt_tail(t2));   /* B */
    return kapp(p, t1p, kt_tail(t2));
}
/* the union of two bit lists */
static unsigned char *g_union(const unsigned char *a, int na, const unsigned char *b, int nb, int *n) {
    *n = na > nb ? na : nb;
    unsigned char *g = *n ? malloc(*n) : NULL;
    for (int i = 0; i < *n; i++) g[i] = (i < na && a[i]) || (i < nb && b[i]);
    return g;
}
static Ast *expand_num(AstPool *pool, i64 n);
static KT kconv(AstPool *p, Ast *e, bool *ok) {
    switch (e->tag) {
    case AST_VAR: {
        int k = e->var.debruijn;
        if (k < 0) { fprintf(stderr, "Error: free variable '%.*s' in term\n", e->var.name.len, e->var.name.str); *ok = false; return kt_closed(e); }
        unsigned char *g = malloc(k + 1); memset(g, 0, k + 1); g[k] = 1;
        return kt(g, k + 1, ast_i(p, noloc));
    }
    case AST_S: case AST_K: case AST_I: case AST_B: case AST_C: case AST_T: case AST_R:
        return kt_closed(e);
    case AST_ABS: {
        KT b = kconv(p, e->abs.body, ok);
        if (!*ok) return b;
        if (b.n == 0) return kt_closed(kapp_ast(p, ast_k(p, noloc), b.d));                    /* \x. d = K d */
        if (!b.g[0]) { KT t = kt_tail(b); return kt(t.g, t.n, kapp(p, kt_closed(ast_k(p, noloc)), t)); }
        return kt_tail(b);
    }
    case AST_APP: {
        KT t1 = kconv(p, e->app.func, ok); if (!*ok) return t1;
        KT t2 = kconv(p, e->app.arg, ok); if (!*ok) return t2;
        int n; unsigned char *g = g_union(t1.g, t1.n, t2.g, t2.n, &n);
        return kt(g, n, kapp(p, t1, t2));
    }
    case AST_NUM: {
        Ast *acc = expand_num(p, e->num);
        if (!acc) { *ok = false; return kt_closed(e); }
        if (!resolve_rec(acc, NULL)) { *ok = false; return kt_closed(e); }
        return kconv(p, acc, ok);
    }
    case AST_LET:
        fprintf(stderr, "Error: LET should be desugared before bracket abstraction\n"); *ok = false; return kt_closed(e);
    case AST_STR:
        fprintf(stderr, "Error: string literals should be desugared before bracket abstraction\n"); *ok = false; return kt_closed(e);
    }
    *ok = false; return kt_closed(e);
}
/* a numeral as a Church numeral built by binary expansion: O(log n) in size (ZERO, ONE, DBL, SUCC as closed lambdas) */
static Ast *expand_num(AstPool *pool, i64 n) {
    if (n < 0) { fprintf(stderr, "Error: negative numbers not supported\n"); return NULL; }
    Symbol f_sym = { "f", 1 };
    Symbol x_sym = { "x", 1 };
    Symbol m_sym = { "m", 1 };
    #define NUM_VAR(sym) ast_var(pool, noloc, (sym))
    #define NUM_APP(a, b) ast_app(pool, noloc, (a), (b))
    #define NUM_ABS(sym, body) ast_abs(pool, noloc, (sym), (body))
    Ast *acc;
    if (n == 0) {
        acc = NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_VAR(x_sym)));
    } else {
        acc = NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_APP(NUM_VAR(f_sym), NUM_VAR(x_sym))));
        int msb = 63 - __builtin_clzll((u64)n);
        for (int i = msb - 1; i >= 0; i--) {
            Ast *mf1 = NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym));
            Ast *mf2 = NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym));
            Ast *dbl = NUM_ABS(m_sym, NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_APP(mf1, NUM_APP(mf2, NUM_VAR(x_sym))))));
            acc = NUM_APP(dbl, acc);
            if ((n >> i) & 1) {
                Ast *mfx = NUM_APP(NUM_APP(NUM_VAR(m_sym), NUM_VAR(f_sym)), NUM_VAR(x_sym));
                Ast *succ = NUM_ABS(m_sym, NUM_ABS(f_sym, NUM_ABS(x_sym, NUM_APP(NUM_VAR(f_sym), mfx))));
                acc = NUM_APP(succ, acc);
            }
        }
    }
    #undef NUM_VAR
    #undef NUM_APP
    #undef NUM_ABS
    return acc;
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
    case AST_B: return ski_b(pool);     /* native leaves; the pure formats spell them as S K trees at emission */
    case AST_C: return ski_c(pool);
    case AST_T: return ski_t(pool);
    case AST_R: return ski_r(pool);
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
    bool ok = true;
    KT t = kconv(ast_pool, ast, &ok);
    if (!ok) return NULL;
    if (t.n > 0) { fprintf(stderr, "Error: the program is not closed\n"); return NULL; }
    return comb_to_term(ski_pool, t.d);
}
