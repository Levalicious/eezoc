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
#include "parse/parse.h"
#include "parse/opp/operator.h"

/* Binding context for resolving de Bruijn indices to names */
typedef struct BindCtx {
    Symbol name;
    struct BindCtx *next;
} BindCtx;

/* Conversion context including globals array */
typedef struct {
    AstPool *pool;
    Array *globals;
    BindCtx *bindings;
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
 * Convert LZ VARIABLE to our Ast
 */
static Ast *convert_variable(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    Lexeme lex = getLexeme(tag);
    long long debruijn = getValue(term);
    
    /* Global variable reference (negative de Bruijn index) */
    if (debruijn < 0) {
        /* Look up in globals array and recursively convert */
        Term *global_term = getGlobalReferent(term, ctx->globals);
        return convert_term_ctx(ctx, global_term);
    }
    
    /* Check for built-in combinators (only when de Bruijn index is 0, meaning unbound) */
    if (debruijn == 0 && lex.length == 1) {
        if (lex.start[0] == 'S') return ast_s(ctx->pool, tag_to_loc(tag));
        if (lex.start[0] == 'K') return ast_k(ctx->pool, tag_to_loc(tag));
        if (lex.start[0] == 'I') return ast_i(ctx->pool, tag_to_loc(tag));
    }
    
    /* For bound variables, look up name in context */
    if (debruijn > 0) {
        Symbol name = lookup_debruijn(ctx->bindings, debruijn);
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
 * Convert LZ ABSTRACTION to our Ast
 */
static Ast *convert_abstraction(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    Term *body = getRight(term);
    
    /* LZ uses (parameter, body) layout */
    /* Parameter tag gives the name */
    Term *param = getLeft(term);
    Symbol param_sym = tag_to_symbol(getTag(param));
    
    /* Extend context with new binding */
    BindCtx new_binding = { .name = param_sym, .next = ctx->bindings };
    BindCtx *old_bindings = ctx->bindings;
    ctx->bindings = &new_binding;
    
    Ast *body_ast = convert_term_ctx(ctx, body);
    
    ctx->bindings = old_bindings;
    
    if (!body_ast) return NULL;
    
    return ast_abs(ctx->pool, tag_to_loc(tag), param_sym, body_ast);
}

/*
 * Convert LZ APPLICATION to our Ast
 */
static Ast *convert_application(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    Term *func = getLeft(term);
    Term *arg = getRight(term);
    
    Ast *func_ast = convert_term_ctx(ctx, func);
    Ast *arg_ast = convert_term_ctx(ctx, arg);
    
    if (!func_ast || !arg_ast) return NULL;
    
    return ast_app(ctx->pool, tag_to_loc(tag), func_ast, arg_ast);
}

/*
 * Convert LZ NUMERAL to our Ast (Church numeral)
 */
static Ast *convert_numeral(ConvCtx *ctx, Term *term) {
    Tag tag = getTag(term);
    long long value = getValue(term);
    return ast_num(ctx->pool, tag_to_loc(tag), value);
}

/*
 * Main term converter with context
 */
static Ast *convert_term_ctx(ConvCtx *ctx, Term *term) {
    if (!term) return NULL;
    
    switch (getTermType(term)) {
    case VARIABLE:
        return convert_variable(ctx, term);
    case ABSTRACTION:
        return convert_abstraction(ctx, term);
    case APPLICATION:
        return convert_application(ctx, term);
    case NUMERAL:
        return convert_numeral(ctx, term);
    case OPERATION:
        fprintf(stderr, "Error: operations not yet supported\n");
        return NULL;
    default:
        fprintf(stderr, "Error: unknown term type %d\n", getTermType(term));
        return NULL;
    }
}

/*
 * Wrapper for external use (starts with empty context)
 */
static Ast *convert_term(AstPool *pool, Term *term, Array *globals) {
    ConvCtx ctx = {
        .pool = pool,
        .globals = globals,
        .bindings = NULL
    };
    return convert_term_ctx(&ctx, term);
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
