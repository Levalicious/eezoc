/*
 * jit.h - Fast SKI reduction engine
 *
 * This implements a spine-stack based graph reducer that's faster
 * than the recursive redex-finding approach in term.c.
 */
#ifndef JIT_H
#define JIT_H

#include "term.h"

/* Compiled function type (placeholder for future true JIT) */
typedef SKITerm *(*JitFunc)(SKIPool *pool);

/* "Compile" a term - currently just validates it */
JitFunc jit_compile(SKIPool *pool, SKITerm *term);

/* Free compiled code */
void jit_free(JitFunc fn);

/* Fast reduction using STG machine (C interpreter) */
SKITerm *jit_reduce(SKIPool *pool, SKITerm *term, i64 *steps);

/* Fast reduction using native x86-64 code */
SKITerm *jit_reduce_native(SKIPool *pool, SKITerm *term, i64 *steps);

/* Emit standalone x86-64 PIE executable that reduces term and outputs BCL */
int jit_emit_elf(int fd, SKITerm *term);

#endif
