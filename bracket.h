/*
 * bracket.h - Bracket abstraction (λ-calculus → SKI)
 *
 * Standard bracket abstraction algorithm:
 *   [x] x       = I
 *   [x] y       = K y           (y ≠ x, y is variable or combinator)
 *   [x] (E₁ E₂) = S ([x] E₁) ([x] E₂)
 *
 * Optimizations (η-reduction, etc.) can be added later.
 */
#ifndef BRACKET_H
#define BRACKET_H

#include "ast.h"
#include <libeezo/term.h>

/*
 * Convert AST to SKI term via bracket abstraction.
 * 
 * Handles:
 *   - Variables (must be resolved to de Bruijn indices first)
 *   - Abstractions (via bracket abstraction)
 *   - Applications (recursive)
 *   - Let (desugar to application of abstraction)
 *   - Numbers (Church encoding)
 *   - S, K, I primitives
 *
 * Returns NULL on error.
 */
SKITerm *bracket_compile(AstPool *ap, SKIPool *tp, Ast *ast);

/*
 * Resolve variable names to de Bruijn indices.
 * Must be called before bracket_compile.
 * Returns false on error (undefined variable).
 */
bool bracket_resolve(Ast *ast);

#endif /* BRACKET_H */
