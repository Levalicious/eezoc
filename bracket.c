#include <libeezo/mem.h>
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

/* A depth-first walk on a heap stack, not C recursion (the term's depth is bounded by memory alone): each pending
   subterm with its environment, left to right; the environments' nodes live in the walk's arena. The first unbound
   variable ends it. */
typedef struct { Ast *ast; Env *env; } RTask;
static bool resolve_rec(Ast *root, Env *env0) {
    Stack st = STACK_INIT(RTask); Arena envs = { 0 };
    RTask t0 = { root, env0 }; STACK_PUSH(&st, RTask, t0);
    bool ok = true;
    while (ok && st.n) {
        RTask t = STACK_POP(&st, RTask);
        Ast *ast = t.ast;
        switch (ast->tag) {
        case AST_VAR: {
            int idx = env_lookup(t.env, ast->var.name);
            if (idx < 0) {
                fprintf(stderr, "Error: undefined variable '%.*s' at line %u\n",
                        ast->var.name.len, ast->var.name.str, ast->loc.line);
                ok = false; break;
            }
            ast->var.debruijn = idx;
            break;
        }
        case AST_S: case AST_K: case AST_I: case AST_B: case AST_C: case AST_T: case AST_R:
        case AST_WORD: case AST_PRIM: case AST_NUM: case AST_STR:
            break;
        case AST_APP: {
            RTask r = { ast->app.arg, t.env }, l = { ast->app.func, t.env };
            STACK_PUSH(&st, RTask, r); STACK_PUSH(&st, RTask, l);
            break;
        }
        case AST_ABS: {
            Env *ne = arena_alloc(&envs, sizeof *ne); ne->name = ast->abs.param; ne->next = t.env;
            RTask b = { ast->abs.body, ne }; STACK_PUSH(&st, RTask, b);
            break;
        }
        case AST_LET: {   /* let x = v in e: v in the current environment, then e with x bound */
            Env *ne = arena_alloc(&envs, sizeof *ne); ne->name = ast->let.name; ne->next = t.env;
            RTask b = { ast->let.body, ne }, v = { ast->let.value, t.env };
            STACK_PUSH(&st, RTask, b); STACK_PUSH(&st, RTask, v);
            break;
        }
        default: ok = false; break;
        }
    }
    stack_drop(&st); arena_drop(&envs);
    return ok;
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

/* the rules of #: how the innermost variable is used on each side decides the combinator. The recursion runs as deep
   as the binders nest, so it is a loop: a rule that needs an inner combination first (S, C, B with the left side
   open) pushes the outer one - its bits and its right side - and continues with the inner; a result completes the
   innermost pending outer combination, or is the answer. */
typedef struct { unsigned char *g; int n; KT t2; } KPend;
static Ast *kapp(AstPool *p, KT t1, KT t2) {
    Stack pend = STACK_INIT(KPend);
    Ast *r;
#define KRET(x) do { r = (x); goto result; } while (0)
#define KINNER(outer_g, outer_n, rest2, in1, in2) do { KPend k_ = { (outer_g), (outer_n), (rest2) }; STACK_PUSH(&pend, KPend, k_); t1 = (in1); t2 = (in2); goto next; } while (0)
    for (;;) {
    next:
        if (t1.n == 0) {
            if (t2.n == 0) KRET(kapp_ast(p, t1.d, t2.d));                                        /* d1 d2 */
            if (kt_is_var0(t2)) KRET(t1.d);                                                       /* eta: \x. d1 x = d1 */
            if (kt_head(t2)) { t1 = kt_closed(kapp_ast(p, kcomb(p, AST_B), t1.d)); t2 = kt_tail(t2); continue; }   /* B d1 . */
            t2 = kt_tail(t2); continue;                                                           /* x unused on the right */
        }
        if (kt_is_var0(t1)) {
            if (t2.n == 0) KRET(kapp_ast(p, kcomb(p, AST_T), t2.d));                             /* \x. x d2 = T d2 */
            if (!kt_head(t2)) { t1 = kt_closed(kcomb(p, AST_T)); t2 = kt_tail(t2); continue; }
        }
        if (kt_head(t1)) {
            if (t2.n == 0) { KT n1 = kt_closed(kapp_ast(p, kcomb(p, AST_R), t2.d)), n2 = kt_tail(t1); t1 = n1; t2 = n2; }   /* R d2 . */
            else {
                KT t1p = kt_tail(t1);
                if (kt_head(t2)) KINNER(t1p.g, t1p.n, kt_tail(t2), kt_closed(kcomb(p, AST_S)), t1p);   /* S */
                KINNER(t1p.g, t1p.n, kt_tail(t2), kt_closed(kcomb(p, AST_C)), t1p);                    /* C */
            }
            continue;
        }
        /* x unused on the left */
        {
            KT t1p = kt_tail(t1);
            if (t2.n == 0) { t1 = t1p; continue; }
            if (kt_is_var0(t2)) KRET(t1.d);                                                       /* eta */
            if (kt_head(t2)) KINNER(t1p.g, t1p.n, kt_tail(t2), kt_closed(kcomb(p, AST_B)), t1p);  /* B */
            t1 = t1p; t2 = kt_tail(t2); continue;
        }
    result:
        if (!pend.n) break;
        { KPend k = STACK_POP(&pend, KPend); t1 = kt(k.g, k.n, r); t2 = k.t2; }
    }
#undef KRET
#undef KINNER
    stack_drop(&pend);
    return r;
}
/* the union of two bit lists */
static unsigned char *g_union(const unsigned char *a, int na, const unsigned char *b, int nb, int *n) {
    *n = na > nb ? na : nb;
    unsigned char *g = *n ? rmalloc(*n) : NULL;
    for (int i = 0; i < *n; i++) g[i] = (i < na && a[i]) || (i < nb && b[i]);
    return g;
}
static Ast *expand_num(AstPool *pool, i64 n);
/* A subterm's (bits, combinator) pair, bottom up: an explicit machine on a heap stack, not C recursion. A frame waits
   for its body (an abstraction) or its two sides (an application); a finished pair is handed to the frame below. */
typedef struct { Ast *e; int st; KT t1; } KFrame;
static KT kconv(AstPool *p, Ast *root, bool *ok) {
    Stack st = STACK_INIT(KFrame);
    KT ret = kt_closed(root); int have = 0;
    KFrame f0 = { root, 0, { 0 } }; STACK_PUSH(&st, KFrame, f0);
    while (st.n) {
        KFrame *f = &STACK_TOP(&st, KFrame);
        Ast *e = f->e;
        if (!have) {
            switch (e->tag) {
            case AST_VAR: {
                int k = e->var.debruijn;
                if (k < 0) { fprintf(stderr, "Error: free variable '%.*s' in term\n", e->var.name.len, e->var.name.str); *ok = false; ret = kt_closed(e); goto out; }
                unsigned char *g = rmalloc(k + 1); memset(g, 0, k + 1); g[k] = 1;
                ret = kt(g, k + 1, ast_i(p, noloc)); st.n--; have = 1; continue;
            }
            case AST_S: case AST_K: case AST_I: case AST_B: case AST_C: case AST_T: case AST_R: case AST_WORD: case AST_PRIM:
                ret = kt_closed(e); st.n--; have = 1; continue;
            case AST_ABS: { KFrame c = { e->abs.body, 0, { 0 } }; STACK_PUSH(&st, KFrame, c); continue; }
            case AST_APP: { KFrame c = { f->st == 0 ? e->app.func : e->app.arg, 0, { 0 } }; STACK_PUSH(&st, KFrame, c); continue; }
            case AST_NUM: {   /* the numeral's expansion stands in for it */
                Ast *acc = expand_num(p, e->num);
                if (!resolve_rec(acc, NULL)) { *ok = false; ret = kt_closed(e); goto out; }
                f->e = acc; continue;
            }
            case AST_LET:
                fprintf(stderr, "Error: LET should be desugared before bracket abstraction\n"); *ok = false; ret = kt_closed(e); goto out;
            case AST_STR:
                fprintf(stderr, "Error: string literals should be desugared before bracket abstraction\n"); *ok = false; ret = kt_closed(e); goto out;
            default:
                *ok = false; ret = kt_closed(e); goto out;
            }
        }
        have = 0;
        if (e->tag == AST_ABS) {
            KT b = ret;
            if (b.n == 0) ret = kt_closed(kapp_ast(p, ast_k(p, noloc), b.d));                   /* \x. d = K d */
            else if (!b.g[0]) { KT t = kt_tail(b); ret = kt(t.g, t.n, kapp(p, kt_closed(ast_k(p, noloc)), t)); }
            else ret = kt_tail(b);
            st.n--; have = 1; continue;
        }
        /* an application: its function done, then its argument */
        if (f->st == 0) { f->t1 = ret; f->st = 1; continue; }
        { KT t1 = f->t1, t2 = ret;
          int n; unsigned char *g = g_union(t1.g, t1.n, t2.g, t2.n, &n);
          ret = kt(g, n, kapp(p, t1, t2)); }
        st.n--; have = 1;
    }
out:
    stack_drop(&st);
    return ret;
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

/* Convert combinator-only AST to SKITerm: bottom up, an explicit machine on a heap stack (as kconv). NULL, with a
   message, if something other than a combinator is met; the part built is released. */
typedef struct { Ast *a; int st; SKITerm *f; } CFrame;
static SKITerm* comb_leaf(SKIPool *pool, Ast *comb) {
    switch (comb->tag) {
    case AST_S: return ski_s(pool);
    case AST_K: return ski_k(pool);
    case AST_I: return ski_i(pool);
    case AST_B: return ski_b(pool);     /* native leaves; the pure formats spell them as S K trees at emission */
    case AST_C: return ski_c(pool);
    case AST_T: return ski_t(pool);
    case AST_R: return ski_r(pool);
    case AST_WORD: return ski_word(pool, comb->word);
    case AST_PRIM: return ski_prim(pool, comb->op);
    case AST_VAR:
        fprintf(stderr, "Error: variable '%.*s' in combinator term\n", comb->var.name.len, comb->var.name.str);
        return NULL;
    case AST_ABS: fprintf(stderr, "Error: abstraction in combinator term\n"); return NULL;
    case AST_LET: fprintf(stderr, "Error: let in combinator term\n"); return NULL;
    case AST_NUM: case AST_STR: fprintf(stderr, "Error: literal in combinator term\n"); return NULL;
    default: return NULL;
    }
}
static SKITerm* comb_to_term(SKIPool *pool, Ast *root) {
    Stack st = STACK_INIT(CFrame);
    SKITerm *ret = NULL; int have = 0;
    CFrame f0 = { root, 0, NULL }; STACK_PUSH(&st, CFrame, f0);
    while (st.n) {
        CFrame *f = &STACK_TOP(&st, CFrame);
        if (!have) {
            if (f->a->tag != AST_APP) {
                ret = comb_leaf(pool, f->a); st.n--;
                if (!ret) goto fail;
                have = 1; continue;
            }
            CFrame c = { f->st == 0 ? f->a->app.func : f->a->app.arg, 0, NULL }; STACK_PUSH(&st, CFrame, c);
            continue;
        }
        have = 0;
        if (f->st == 0) { f->f = ret; f->st = 1; continue; }
        ret = ski_app(pool, f->f, ret); st.n--; have = 1;
    }
    stack_drop(&st);
    return ret;
fail:
    while (st.n) { CFrame f = STACK_POP(&st, CFrame); if (f.st == 1) ski_unref(pool, f.f); }
    stack_drop(&st);
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
