/*
 * compile.h - Source to BCL compilation
 *
 * Takes LZ parsed/bound terms, converts to compiler AST, applies bracket
 * abstraction, and emits BCL.
 */
#ifndef COMPILE_H
#define COMPILE_H

#include "ast.h"
#include <libeezo/bcl.h>

/* Our SKI term type */
#include <libeezo/term.h>

/*
 * Compile source string to SKI term.
 * Returns NULL on error.
 */
SKITerm *compile_to_ski(AstPool *ap, SKIPool *tp, const char *source);

/*
 * Compile source string to BCL bitstring using BclBuffer.
 * Returns true on success.
 */
bool compile_to_bcl(AstPool *ap, SKIPool *tp, const char *source, 
                    BclBuffer *buf);

#endif /* COMPILE_H */
