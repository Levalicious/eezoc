#include <libeezo/mem.h>
/*
 * compile.c - Source to BCL compilation
 *
 * Pipeline:
 *   Source text → LZ Parser → LZ Bound SKITerm → Bracket Abstraction → SKI → BCL
 *
 * Uses Lambda Zero's parser and binder, then applies bracket abstraction
 * to convert λ-terms to SKI combinators.
 */
#include "compile.h"
#include "bracket.h"
#include <libeezo/bcl.h>
#include <string.h>
#include <stdio.h>

/* LZ includes */
#include "parse/tree.h"
#include "parse/array.h"
#include "parse/term.h"
#include <stdlib.h>
#include <string.h>
#include "parse/parse.h"
#include "parse/opp/operator.h"

/* Binding context for resolving de Bruijn indices to names */
typedef struct BindCtx {
    Symbol name;
    struct BindCtx *next;
} BindCtx;

/* Conversion context including globals array. A global (a top-level definition) is converted ONCE, on its first
   reference, and every reference becomes a variable bound by a let around the entry ((\g. body) V): the definition is
   shared at run time as one thunk. (Converting the referent at every reference inlined it, and a chain of definitions
   each using the previous one compiled to a term exponential in the length of the chain.) */
typedef struct {
    AstPool *pool;
    Array *globals;
    BindCtx *bindings;
    Ast **gast;        /* per global: its converted term (NULL: not referenced yet) */
    Symbol *gname;     /* per global: the let-bound name ('$' + its name: no source identifier collides) */
    size_t nglobals;
} ConvCtx;

/* Forward declarations */
static Ast *convert_term_ctx(ConvCtx *ctx, Term *term);

/*
 * Get source location from LZ tag
 */
static SrcLoc tag_to_loc(Tag tag) {
    Lexeme lex = getLexeme(tag);
    SrcLoc loc = {
        .file = NULL,
        .line = (u32)lex.location.line,
        .col = (u32)lex.location.column
    };
    return loc;
}

/*
 * Get symbol from LZ tag
 */
static Symbol tag_to_symbol(Tag tag) {
    Lexeme lex = getLexeme(tag);
    return (Symbol){ .str = lex.start, .len = lex.length };
}

/*
 * Lookup de Bruijn index in binding context
 */
static Symbol lookup_debruijn(BindCtx *bindings, long long index) {
    while (bindings && index > 1) {
        bindings = bindings->next;
        index--;
    }
    if (bindings && index == 1) {
        return bindings->name;
    }
    return (Symbol){ NULL, 0 };  /* Not found */
}

/*
 * Convert LZ VARIABLE to our Ast: every case but a global met for the first time (convert_term_ctx converts that
 * global's definition first, then comes here). A bad global index is NULL.
 */
static Ast *convert_variable(ConvCtx *ctx, Term *term, BindCtx *bindings) {
    Tag tag = getTag(term);
    Lexeme lex = getLexeme(tag);
    long long debruijn = getValue(term);
    
    /* Global variable reference (negative de Bruijn index): converted once, then a let-bound variable */
    if (debruijn < 0) {
        size_t gi = (size_t)(-debruijn - 1);
        if (gi >= ctx->nglobals) { fprintf(stderr, "Error: global index %zu out of range\n", gi); return NULL; }
        return ast_var(ctx->pool, tag_to_loc(tag), ctx->gname[gi]);
    }
    
    /* Check for built-in combinators (only when de Bruijn index is 0, meaning unbound) */
    if (debruijn == 0 && lex.length == 1) {
        if (lex.start[0] == 'S') return ast_s(ctx->pool, tag_to_loc(tag));
        if (lex.start[0] == 'K') return ast_k(ctx->pool, tag_to_loc(tag));
        if (lex.start[0] == 'I') return ast_i(ctx->pool, tag_to_loc(tag));
    }
    
    /* For bound variables, look up name in context */
    if (debruijn > 0) {
        Symbol name = lookup_debruijn(bindings, debruijn);
        if (name.str) {
            return ast_var(ctx->pool, tag_to_loc(tag), name);
        }
        /* Fallback: shouldn't happen if LZ binding is correct */
        fprintf(stderr, "Warning: de Bruijn index %lld not found in context\n", debruijn);
        return ast_var(ctx->pool, tag_to_loc(tag), tag_to_symbol(tag));
    }
    
    /* Unbound variable - use name */
    return ast_var(ctx->pool, tag_to_loc(tag), tag_to_symbol(tag));
}

/*
 * Convert LZ NUMERAL to our Ast (Church numeral)
 */
static Ast *convert_numeral(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    long long value = getValue(term);
    if (getVariety(term) == 1) return ast_word(ctx->pool, tag_to_loc(tag), (u64)value);   /* 5w */
    return ast_num(ctx->pool, tag_to_loc(tag), value);
}

/*
 * Convert LZ OPERATION to our Ast: the word primitives are pseudo-operations of the parser
 * (parse/term.h), in PrimOp order from WADD.
 */
static Ast *convert_operation(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    OperationCode code = getOperationCode(term);
    if (code >= WADD && code <= WDIVMOD) return ast_prim(ctx->pool, tag_to_loc(tag), (PrimOp)(code - WADD));
    fprintf(stderr, "Error: operation '%s' is not supported\n", Operations[code]);
    return NULL;
}

/*
 * Main term converter with context: an explicit machine on a heap stack, not C recursion (a term's depth, and the
 * chain of first references from one global's definition to the next, are bounded by memory alone). Each frame
 * carries its own binding context; an abstraction's new binding lives in the conversion's arena. A failed subterm is
 * NULL, which its enclosing terms pass on (both sides of an application are still converted, as before).
 */
typedef struct { Term *t; BindCtx *b; int st; Ast *l; size_t gi; } CvFrame;
static Ast *convert_term_ctx(ConvCtx *ctx, Term *root) {
    Stack st = STACK_INIT(CvFrame); Arena binds = { 0 };
    Ast *ret = NULL; int have = 0;
    CvFrame f0 = { root, ctx->bindings, 0, NULL, 0 }; STACK_PUSH(&st, CvFrame, f0);
    while (st.n) {
        CvFrame *f = &STACK_TOP(&st, CvFrame);
        Term *term = f->t;
        if (!have) {
            if (!term) { ret = NULL; st.n--; have = 1; continue; }
            switch (getTermType(term)) {
            case VARIABLE: {
                long long debruijn = getValue(term);
                if (debruijn < 0) {
                    size_t gi = (size_t)(-debruijn - 1);
                    if (gi < ctx->nglobals && !ctx->gast[gi]) {   /* first reference: the definition, a closed term over the earlier globals */
                        Symbol name = tag_to_symbol(getTag(term));
                        char *s = rmalloc(name.len + 2); s[0] = '$'; memcpy(s + 1, name.str, name.len); s[name.len + 1] = 0;
                        ctx->gname[gi] = (Symbol){ s, name.len + 1 };
                        f->st = 1; f->gi = gi;
                        CvFrame c = { getGlobalReferent(term, ctx->globals), NULL, 0, NULL, 0 }; STACK_PUSH(&st, CvFrame, c);
                        continue;
                    }
                }
                ret = convert_variable(ctx, term, f->b); st.n--; have = 1; continue;
            }
            case ABSTRACTION: {
                BindCtx *nb = arena_alloc(&binds, sizeof *nb);
                nb->name = tag_to_symbol(getTag(getLeft(term))); nb->next = f->b;   /* LZ uses (parameter, body) layout */
                CvFrame c = { getRight(term), nb, 0, NULL, 0 }; STACK_PUSH(&st, CvFrame, c);
                continue;
            }
            case APPLICATION: {
                CvFrame c = { f->st == 0 ? getLeft(term) : getRight(term), f->b, 0, NULL, 0 }; STACK_PUSH(&st, CvFrame, c);
                continue;
            }
            case NUMERAL: ret = convert_numeral(ctx, term); st.n--; have = 1; continue;
            case OPERATION: ret = convert_operation(ctx, term); st.n--; have = 1; continue;
            default:
                fprintf(stderr, "Error: unknown term type %d\n", getTermType(term));
                ret = NULL; st.n--; have = 1; continue;
            }
        }
        have = 0;
        switch (getTermType(term)) {
        case VARIABLE:   /* the global's definition is converted: the reference is its let-bound name */
            if (ret) { ctx->gast[f->gi] = ret; ret = ast_var(ctx->pool, tag_to_loc(getTag(term)), ctx->gname[f->gi]); }
            st.n--; have = 1; break;
        case ABSTRACTION: {
            Symbol param_sym = tag_to_symbol(getTag(getLeft(term)));
            ret = ret ? ast_abs(ctx->pool, tag_to_loc(getTag(term)), param_sym, ret) : NULL;
            st.n--; have = 1; break;
        }
        default:   /* an application: its function, then its argument */
            if (f->st == 0) { f->l = ret; f->st = 1; break; }
            ret = f->l && ret ? ast_app(ctx->pool, tag_to_loc(getTag(term)), f->l, ret) : NULL;
            st.n--; have = 1; break;
        }
    }
    stack_drop(&st); arena_drop(&binds);
    return ret;
}

/*
 * Wrapper for external use (starts with empty context)
 */
static Ast *convert_term(AstPool *pool, Term *term, Array *globals) {
    size_t n = globals ? length(globals) : 0;
    ConvCtx ctx = {
        .pool = pool,
        .globals = globals,
        .bindings = NULL,
        .gast = rcalloc(n + 1, sizeof(Ast *)),
        .gname = rcalloc(n + 1, sizeof(Symbol)),
        .nglobals = n
    };
    Ast *body = convert_term_ctx(&ctx, term);
    if (!body) return NULL;
    /* the referenced globals, bound around the body in definition order (a definition refers only to earlier ones):
       let g_0 = V_0 in .. let g_k = V_k in body  ==  (\g_0. .. (\g_k. body) V_k ..) V_0 */
    for (size_t i = n; i-- > 0; ) {
        if (!ctx.gast[i]) continue;
        SrcLoc loc = body->loc;
        body = ast_app(pool, loc, ast_abs(pool, loc, ctx.gname[i], body), ctx.gast[i]);
    }
    free(ctx.gast); free(ctx.gname);
    return body;
}

/*
 * Compile source string to SKI term
 */
SKITerm *compile_to_ski(AstPool *ap, SKIPool *tp, const char *source) {
    /* Initialize LZ parser */
    initNodeAllocator();
    initSyntax();
    
    /* Parse and bind */
    Program prog = parse(source);
    
    if (!prog.entry) {
        fprintf(stderr, "Parse failed\n");
        deleteProgram(prog);
        destroyNodeAllocator();
        return NULL;
    }
    
    /* Convert LZ bound term to our AST, passing globals for resolution */
    Ast *ast = convert_term(ap, prog.entry, prog.globals);
    
    if (!ast) {
        fprintf(stderr, "AST conversion failed\n");
        deleteProgram(prog);
        destroyNodeAllocator();
        return NULL;
    }
    
    /* Resolve any remaining variable names to de Bruijn indices */
    if (!bracket_resolve(ast)) {
        fprintf(stderr, "Resolution failed\n");
        deleteProgram(prog);
        destroyNodeAllocator();
        return NULL;
    }
    
    /* Bracket abstraction: AST → SKI */
    SKITerm *result = bracket_compile(ap, tp, ast);
    
    deleteProgram(prog);
    destroyNodeAllocator();
    
    return result;
}

/*
 * Compile source string to BCL bitstring using BclBuffer
 */
bool compile_to_bcl(AstPool *ap, SKIPool *tp, const char *source,
                    BclBuffer *buf) {
    SKITerm *ski = compile_to_ski(ap, tp, source);
    if (!ski) return false;
    
    /* Convert SKI to BCL */
    return bcl_emit(ski, buf);
}
