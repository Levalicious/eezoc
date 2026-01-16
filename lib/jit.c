/*
 * jit.c - True STG Machine for SKI Combinators
 *
 * This is a proper Spineless Tagless G-machine implementation:
 * - Spineless: Arguments passed on stack, not in spine
 * - Tagless: No runtime tag inspection, dispatch via entry code pointer
 * - G-machine: Graph reduction with sharing via self-updating thunks
 *
 * The key insight from SPJ's 1992 paper:
 * - "Tagless" means no interpretive switch/case dispatch
 * - Each closure has an entry code pointer - we call/jump to it directly
 * - This eliminates the "spine" of the G-machine's central eval loop
 *
 * Closure layout: [entry_ptr | payload...]
 *   entry_ptr: Function pointer to entry code for this closure type
 *   payload: Type-specific data (captured variables, etc.)
 *
 * Entry code contract:
 *   - Receives: STG machine state, current closure
 *   - Either: consumes args from stack and tail-calls next closure
 *   - Or: returns a result (closure in WHNF)
 *
 * Registers (x86-64 native):
 *   rbx = current closure (Node register)
 *   r12 = heap pointer
 *   r13 = stack base (for underflow check)
 *   r14 = stack pointer (grows down)
 *   r15 = update stack pointer
 */

#include "jit.h"
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <unistd.h>

#define HEAP_SIZE   (16 * 1024 * 1024)  /* 16MB per semi-space */
#define STACK_SIZE  (256 * 1024)
#define WORD        sizeof(void*)

/* Forward declarations */
struct Closure;
struct STG;

/*
 * Entry code function type - this is the "tagless" dispatch mechanism
 * Instead of switch(tag), we call closure->entry(stg, closure)
 */
typedef struct Closure *(*EntryCode)(struct STG *stg, struct Closure *self);

/*
 * Closure in memory - TRUE STG layout
 * First word is always the entry code pointer (tagless!)
 */
typedef struct Closure {
    EntryCode entry;      /* Entry code - NO TAG, just jump here */
    union {
        struct { struct Closure *x; } s1;           /* S x */
        struct { struct Closure *x, *y; } s2;       /* S x y */
        struct { struct Closure *x; } k1;           /* K x */
        struct { struct Closure *f, *arg; } ap;     /* f @ arg */
        struct { struct Closure *target; } ind;     /* -> target (also used as forwarding ptr) */
    } payload;
} Closure;

/*
 * STG Machine State with Cheney GC
 */
typedef struct STG {
    /* Two semi-spaces for copying GC */
    Closure *space[2];    /* base of each semi-space */
    int active_space;     /* which space is currently active (0 or 1) */
    Closure *hp;          /* heap allocation pointer */
    Closure *heap_end;    /* end of current semi-space */
    size_t heap_size;     /* size of each semi-space */
    
    Closure **stack;      /* argument stack */
    Closure **sp;         /* stack pointer (grows down) */
    Closure **stack_base;
    
    /* Current node being evaluated - needed for GC roots */
    Closure *current_node;
    
    /* Update frames: when we enter a thunk, push its address */
    /* so we can overwrite it when we get the result */
    struct {
        Closure *thunk;
        Closure **saved_sp;
    } *update_stack;
    int update_sp;
    int update_size;
    
    /* For returning to C */
    jmp_buf exit_jmp;
    Closure *result;
    
    /* Pre-built primitive closures (shared singletons) */
    Closure *prim_S;
    Closure *prim_K;
    Closure *prim_I;
    
    /* GC statistics */
    u64 gc_count;
    u64 bytes_copied;
} STG;

static STG *g_stg = NULL;

/* Forward declarations of entry codes */
static Closure *entry_S(STG *stg, Closure *self);
static Closure *entry_K(STG *stg, Closure *self);
static Closure *entry_I(STG *stg, Closure *self);
static Closure *entry_S1(STG *stg, Closure *self);
static Closure *entry_S2(STG *stg, Closure *self);
static Closure *entry_K1(STG *stg, Closure *self);
static Closure *entry_AP(STG *stg, Closure *self);
static Closure *entry_IND(STG *stg, Closure *self);

/* Special marker for forwarding pointers during GC */
static Closure *entry_FWD(STG *stg, Closure *self) {
    (void)stg;
    /* This should never be called - it's a forwarding pointer */
    fprintf(stderr, "GC error: tried to enter forwarding pointer\n");
    return self;
}

/* Forward declaration for GC */
static void stg_gc(STG *stg);
static Closure *gc_copy(STG *stg, Closure *from, Closure **to_hp);

/*
 * Get size of closure in words based on entry code
 */
static int closure_size(Closure *c) {
    if (c->entry == entry_S || c->entry == entry_K || c->entry == entry_I) {
        return 1;  /* just entry ptr */
    }
    if (c->entry == entry_S1 || c->entry == entry_K1 || c->entry == entry_IND) {
        return 2;  /* entry + 1 pointer */
    }
    if (c->entry == entry_S2 || c->entry == entry_AP) {
        return 3;  /* entry + 2 pointers */
    }
    if (c->entry == entry_FWD) {
        return 2;  /* forwarding pointer */
    }
    return 1;  /* unknown, assume minimal */
}

/*
 * Allocate on heap - triggers GC if needed
 */
static Closure *stg_alloc(STG *stg, int words) {
    Closure *p = stg->hp;
    Closure *new_hp = (Closure*)((char*)stg->hp + words * WORD);
    
    if ((char*)new_hp >= (char*)stg->heap_end) {
        /* Trigger garbage collection */
        stg_gc(stg);
        
        /* Try again after GC */
        p = stg->hp;
        new_hp = (Closure*)((char*)stg->hp + words * WORD);
        
        if ((char*)new_hp >= (char*)stg->heap_end) {
            fprintf(stderr, "STG: heap overflow after GC (need %d words)\n", words);
            longjmp(stg->exit_jmp, 1);
        }
    }
    
    stg->hp = new_hp;
    return p;
}

/*
 * Push to argument stack
 */
static void stg_push(STG *stg, Closure *c) {
    if (stg->sp <= stg->stack) {
        fprintf(stderr, "STG: stack overflow\n");
        longjmp(stg->exit_jmp, 1);
    }
    *--stg->sp = c;
}

/*
 * Pop from argument stack
 */
static Closure *stg_pop(STG *stg) {
    if (stg->sp >= stg->stack_base) {
        return NULL;  /* stack empty */
    }
    return *stg->sp++;
}

/*
 * Number of args on stack
 */
static int stg_stack_size(STG *stg) {
    return (int)(stg->stack_base - stg->sp);
}

/* ========================================================================
 * ENTRY CODES - The heart of tagless STG
 * 
 * Each closure type has its own entry code. No switch/case dispatch!
 * We simply call: closure->entry(stg, closure)
 * 
 * Contract:
 *   - If enough args: consume them, build result, tail-call into next closure
 *   - If not enough: build PAP and return it (we're in WHNF)
 * ======================================================================== */

/*
 * S combinator entry code
 * S x y z → x z (y z)
 * Needs 3 arguments
 */
static Closure *entry_S(STG *stg, Closure *self) {
    (void)self;  /* S is a singleton, no payload */
    
    int n = stg_stack_size(stg);
    if (n < 3) {
        /* Not enough args - build PAP */
        if (n == 0) {
            return stg->prim_S;
        } else if (n == 1) {
            Closure *x = stg_pop(stg);
            Closure *s1 = stg_alloc(stg, 2);
            s1->entry = entry_S1;
            s1->payload.s1.x = x;
            return s1;
        } else { /* n == 2 */
            Closure *x = stg_pop(stg);
            Closure *y = stg_pop(stg);
            Closure *s2 = stg_alloc(stg, 3);
            s2->entry = entry_S2;
            s2->payload.s2.x = x;
            s2->payload.s2.y = y;
            return s2;
        }
    }
    
    /* Have 3 args: S x y z → x z (y z) */
    Closure *x = stg_pop(stg);
    Closure *y = stg_pop(stg);
    Closure *z = stg_pop(stg);
    
    /* Build thunk for (y z) */
    Closure *yz = stg_alloc(stg, 3);
    yz->entry = entry_AP;
    yz->payload.ap.f = y;
    yz->payload.ap.arg = z;
    
    /* Push args for x: first (y z), then z (z is first arg) */
    stg_push(stg, yz);
    stg_push(stg, z);
    
    /* Tail-call into x */
    return x->entry(stg, x);
}

/*
 * S1 entry code (S with 1 captured arg)
 * (S x) y z → x z (y z)
 * Needs 2 more arguments
 */
static Closure *entry_S1(STG *stg, Closure *self) {
    int n = stg_stack_size(stg);
    if (n < 2) {
        if (n == 0) {
            return self;
        } else { /* n == 1 */
            Closure *y = stg_pop(stg);
            Closure *s2 = stg_alloc(stg, 3);
            s2->entry = entry_S2;
            s2->payload.s2.x = self->payload.s1.x;
            s2->payload.s2.y = y;
            return s2;
        }
    }
    
    Closure *x = self->payload.s1.x;
    Closure *y = stg_pop(stg);
    Closure *z = stg_pop(stg);
    
    Closure *yz = stg_alloc(stg, 3);
    yz->entry = entry_AP;
    yz->payload.ap.f = y;
    yz->payload.ap.arg = z;
    
    stg_push(stg, yz);
    stg_push(stg, z);
    
    return x->entry(stg, x);
}

/*
 * S2 entry code (S with 2 captured args)
 * (S x y) z → x z (y z)
 * Needs 1 more argument
 */
static Closure *entry_S2(STG *stg, Closure *self) {
    if (stg_stack_size(stg) < 1) {
        return self;
    }
    
    Closure *x = self->payload.s2.x;
    Closure *y = self->payload.s2.y;
    Closure *z = stg_pop(stg);
    
    Closure *yz = stg_alloc(stg, 3);
    yz->entry = entry_AP;
    yz->payload.ap.f = y;
    yz->payload.ap.arg = z;
    
    stg_push(stg, yz);
    stg_push(stg, z);
    
    return x->entry(stg, x);
}

/*
 * K combinator entry code
 * K x y → x
 * Needs 2 arguments
 */
static Closure *entry_K(STG *stg, Closure *self) {
    (void)self;
    
    int n = stg_stack_size(stg);
    if (n < 2) {
        if (n == 0) {
            return stg->prim_K;
        } else { /* n == 1 */
            Closure *x = stg_pop(stg);
            Closure *k1 = stg_alloc(stg, 2);
            k1->entry = entry_K1;
            k1->payload.k1.x = x;
            return k1;
        }
    }
    
    Closure *x = stg_pop(stg);
    stg_pop(stg);  /* discard y */
    
    return x->entry(stg, x);
}

/*
 * K1 entry code (K with 1 captured arg)
 * (K x) y → x
 * Needs 1 more argument
 */
static Closure *entry_K1(STG *stg, Closure *self) {
    if (stg_stack_size(stg) < 1) {
        return self;
    }
    
    stg_pop(stg);  /* discard y */
    Closure *x = self->payload.k1.x;
    
    return x->entry(stg, x);
}

/*
 * I combinator entry code
 * I x → x
 * Needs 1 argument
 */
static Closure *entry_I(STG *stg, Closure *self) {
    (void)self;
    
    if (stg_stack_size(stg) < 1) {
        return stg->prim_I;
    }
    
    Closure *x = stg_pop(stg);
    return x->entry(stg, x);
}

/*
 * Application thunk entry code
 * (f arg) → push arg, enter f
 */
static Closure *entry_AP(STG *stg, Closure *self) {
    Closure *f = self->payload.ap.f;
    Closure *arg = self->payload.ap.arg;
    
    stg_push(stg, arg);
    return f->entry(stg, f);
}

/*
 * Indirection entry code
 * Follows the indirection chain
 */
static Closure *entry_IND(STG *stg, Closure *self) {
    Closure *target = self->payload.ind.target;
    return target->entry(stg, target);
}

/*
 * Enter a closure - THE ONLY DISPATCH POINT
 * This is now just a single indirect call, not a switch!
 */
static Closure *stg_enter(STG *stg, Closure *c) {
    stg->current_node = c;  /* Mark as GC root */
    Closure *result = c->entry(stg, c);
    stg->current_node = result;  /* Update root after reduction */
    return result;
}

/*
 * Reduce to full HNF (not just WHNF)
 * After WHNF, recursively reduce arguments
 */
static Closure *stg_normalize(STG *stg, Closure *c) {
    c = stg_enter(stg, c);
    
    if (c->entry == entry_S || c->entry == entry_K || c->entry == entry_I) {
        /* Primitives with no payload to normalize */
        return c;
    }
    
    if (c->entry == entry_S1) {
        Closure *x = stg_normalize(stg, c->payload.s1.x);
        if (x != c->payload.s1.x) {
            Closure *new_c = stg_alloc(stg, 2);
            new_c->entry = entry_S1;
            new_c->payload.s1.x = x;
            return new_c;
        }
        return c;
    }
    
    if (c->entry == entry_S2) {
        Closure *x = stg_normalize(stg, c->payload.s2.x);
        Closure *y = stg_normalize(stg, c->payload.s2.y);
        if (x != c->payload.s2.x || y != c->payload.s2.y) {
            Closure *new_c = stg_alloc(stg, 3);
            new_c->entry = entry_S2;
            new_c->payload.s2.x = x;
            new_c->payload.s2.y = y;
            return new_c;
        }
        return c;
    }
    
    if (c->entry == entry_K1) {
        Closure *x = stg_normalize(stg, c->payload.k1.x);
        if (x != c->payload.k1.x) {
            Closure *new_c = stg_alloc(stg, 2);
            new_c->entry = entry_K1;
            new_c->payload.k1.x = x;
            return new_c;
        }
        return c;
    }
    
    if (c->entry == entry_AP) {
        /* Shouldn't have AP in normal form after stg_enter - evaluate it */
        stg_push(stg, c->payload.ap.arg);
        return stg_normalize(stg, stg_enter(stg, c->payload.ap.f));
    }
    
    if (c->entry == entry_IND) {
        return stg_normalize(stg, c->payload.ind.target);
    }
    
    return c;
}

/*
 * Convert SKITerm to STG closure
 */
static Closure *term_to_stg(STG *stg, SKITerm *t) {
    switch (t->tag) {
    case TERM_S:
        return stg->prim_S;
    case TERM_K:
        return stg->prim_K;
    case TERM_I:
        return stg->prim_I;
    case TERM_APP: {
        Closure *f = term_to_stg(stg, t->app.left);
        Closure *arg = term_to_stg(stg, t->app.right);
        Closure *ap = stg_alloc(stg, 3);
        ap->entry = entry_AP;
        ap->payload.ap.f = f;
        ap->payload.ap.arg = arg;
        return ap;
    }
    }
    return NULL;
}

/*
 * Convert STG closure back to SKITerm
 */
static SKITerm *stg_to_term(SKIPool *pool, Closure *c) {
    if (c->entry == entry_IND) {
        return stg_to_term(pool, c->payload.ind.target);
    }
    if (c->entry == entry_S) {
        return ski_s(pool);
    }
    if (c->entry == entry_K) {
        return ski_k(pool);
    }
    if (c->entry == entry_I) {
        return ski_i(pool);
    }
    if (c->entry == entry_S1) {
        SKITerm *x = stg_to_term(pool, c->payload.s1.x);
        return ski_app(pool, ski_s(pool), x);
    }
    if (c->entry == entry_S2) {
        SKITerm *x = stg_to_term(pool, c->payload.s2.x);
        SKITerm *y = stg_to_term(pool, c->payload.s2.y);
        return ski_app(pool, ski_app(pool, ski_s(pool), x), y);
    }
    if (c->entry == entry_K1) {
        SKITerm *x = stg_to_term(pool, c->payload.k1.x);
        return ski_app(pool, ski_k(pool), x);
    }
    if (c->entry == entry_AP) {
        SKITerm *f = stg_to_term(pool, c->payload.ap.f);
        SKITerm *arg = stg_to_term(pool, c->payload.ap.arg);
        return ski_app(pool, f, arg);
    }
    return ski_i(pool);  /* fallback */
}

/* ========================================================================
 * CHENEY COPYING GARBAGE COLLECTOR
 * 
 * Two semi-spaces: when one fills, copy live data to the other.
 * Uses forwarding pointers to preserve sharing.
 * ======================================================================== */

/*
 * Check if pointer is in the from-space
 */
static int in_from_space(STG *stg, Closure *p) {
    Closure *from_base = stg->space[stg->active_space];
    Closure *from_end = (Closure*)((char*)from_base + stg->heap_size);
    return p >= from_base && p < from_end;
}

/*
 * Copy a single closure to to-space, return new location
 * If already copied (forwarding pointer), return the forward address
 */
static Closure *gc_copy(STG *stg, Closure *from, Closure **to_hp) {
    if (!from) return NULL;
    
    /* Primitives are allocated at fixed locations at start - don't copy */
    if (from == stg->prim_S || from == stg->prim_K || from == stg->prim_I) {
        return from;
    }
    
    /* If not in from-space, don't copy (shouldn't happen) */
    if (!in_from_space(stg, from)) {
        return from;
    }
    
    /* Check for forwarding pointer */
    if (from->entry == entry_FWD) {
        return from->payload.ind.target;
    }
    
    /* Copy the closure */
    int size = closure_size(from);
    Closure *to = *to_hp;
    memcpy(to, from, size * WORD);
    *to_hp = (Closure*)((char*)*to_hp + size * WORD);
    stg->bytes_copied += size * WORD;
    
    /* Install forwarding pointer in from-space */
    from->entry = entry_FWD;
    from->payload.ind.target = to;
    
    return to;
}

/*
 * Scavenge a closure - update its pointers to point to to-space
 */
static void gc_scavenge(STG *stg, Closure *c, Closure **to_hp) {
    if (c->entry == entry_S1) {
        c->payload.s1.x = gc_copy(stg, c->payload.s1.x, to_hp);
    } else if (c->entry == entry_S2) {
        c->payload.s2.x = gc_copy(stg, c->payload.s2.x, to_hp);
        c->payload.s2.y = gc_copy(stg, c->payload.s2.y, to_hp);
    } else if (c->entry == entry_K1) {
        c->payload.k1.x = gc_copy(stg, c->payload.k1.x, to_hp);
    } else if (c->entry == entry_AP) {
        c->payload.ap.f = gc_copy(stg, c->payload.ap.f, to_hp);
        c->payload.ap.arg = gc_copy(stg, c->payload.ap.arg, to_hp);
    } else if (c->entry == entry_IND) {
        c->payload.ind.target = gc_copy(stg, c->payload.ind.target, to_hp);
    }
    /* S, K, I have no pointers to scavenge */
}

/*
 * Cheney's algorithm - the main GC loop
 */
static void stg_gc(STG *stg) {
    stg->gc_count++;
    stg->bytes_copied = 0;
    
    int from_space = stg->active_space;
    int to_space = 1 - from_space;
    
    Closure *to_base = stg->space[to_space];
    Closure *to_hp = to_base;  /* allocation pointer in to-space */
    Closure *scan = to_base;    /* scan pointer for scavenging */
    
    /* First, copy the primitives to the new space */
    /* (They're singletons, need to be in both spaces) */
    Closure *new_S = to_hp;
    new_S->entry = entry_S;
    to_hp = (Closure*)((char*)to_hp + WORD);
    
    Closure *new_K = to_hp;
    new_K->entry = entry_K;
    to_hp = (Closure*)((char*)to_hp + WORD);
    
    Closure *new_I = to_hp;
    new_I->entry = entry_I;
    to_hp = (Closure*)((char*)to_hp + WORD);
    
    scan = to_hp;  /* don't scavenge primitives */
    
    /* Copy roots: current node being evaluated */
    if (stg->current_node) {
        stg->current_node = gc_copy(stg, stg->current_node, &to_hp);
    }
    
    /* Copy roots: argument stack */
    for (Closure **p = stg->sp; p < stg->stack_base; p++) {
        *p = gc_copy(stg, *p, &to_hp);
    }
    
    /* Copy roots: update stack thunks */
    for (int i = 0; i < stg->update_sp; i++) {
        stg->update_stack[i].thunk = gc_copy(stg, stg->update_stack[i].thunk, &to_hp);
    }
    
    /* Cheney loop: scan copied closures and copy their children */
    while (scan < to_hp) {
        gc_scavenge(stg, scan, &to_hp);
        int size = closure_size(scan);
        scan = (Closure*)((char*)scan + size * WORD);
    }
    
    /* Update primitives */
    stg->prim_S = new_S;
    stg->prim_K = new_K;
    stg->prim_I = new_I;
    
    /* Swap spaces */
    stg->active_space = to_space;
    stg->hp = to_hp;
    stg->heap_end = (Closure*)((char*)to_base + stg->heap_size);
    
#if 1
    size_t used = (char*)to_hp - (char*)to_base;
    fprintf(stderr, "GC #%llu: copied %llu bytes, %zu bytes used\n", 
            stg->gc_count, stg->bytes_copied, used);
#endif
}

/*
 * Initialize STG machine with two semi-spaces
 */
static void stg_init(void) {
    if (g_stg) return;
    
    g_stg = malloc(sizeof(STG));
    memset(g_stg, 0, sizeof(STG));
    
    g_stg->heap_size = HEAP_SIZE;
    g_stg->space[0] = malloc(HEAP_SIZE);
    g_stg->space[1] = malloc(HEAP_SIZE);
    g_stg->active_space = 0;
    g_stg->hp = g_stg->space[0];
    g_stg->heap_end = (Closure*)((char*)g_stg->space[0] + HEAP_SIZE);
    
    g_stg->stack = malloc(STACK_SIZE);
    g_stg->stack_base = (Closure**)((char*)g_stg->stack + STACK_SIZE);
    g_stg->sp = g_stg->stack_base;
    
    g_stg->update_size = 1024;
    g_stg->update_stack = malloc(g_stg->update_size * sizeof(g_stg->update_stack[0]));
    g_stg->update_sp = 0;
    
    /* Pre-build primitive closures (singletons) */
    g_stg->prim_S = stg_alloc(g_stg, 1);
    g_stg->prim_S->entry = entry_S;
    
    g_stg->prim_K = stg_alloc(g_stg, 1);
    g_stg->prim_K->entry = entry_K;
    
    g_stg->prim_I = stg_alloc(g_stg, 1);
    g_stg->prim_I->entry = entry_I;
    
    g_stg->gc_count = 0;
}

/*
 * Reset STG machine for new evaluation
 */
static void stg_reset(void) {
    /* Reset to space 0, preserving primitives at start */
    g_stg->active_space = 0;
    g_stg->hp = (Closure*)((char*)g_stg->space[0] + 3 * WORD);
    g_stg->heap_end = (Closure*)((char*)g_stg->space[0] + g_stg->heap_size);
    g_stg->sp = g_stg->stack_base;
    g_stg->update_sp = 0;
    g_stg->current_node = NULL;
    
    /* Re-establish primitives at start of space 0 */
    g_stg->prim_S = g_stg->space[0];
    g_stg->prim_S->entry = entry_S;
    g_stg->prim_K = (Closure*)((char*)g_stg->space[0] + WORD);
    g_stg->prim_K->entry = entry_K;
    g_stg->prim_I = (Closure*)((char*)g_stg->space[0] + 2 * WORD);
    g_stg->prim_I->entry = entry_I;
}

/*
 * Public API
 */

JitFunc jit_compile(SKIPool *pool, SKITerm *term) {
    (void)pool;
    (void)term;
    stg_init();
    return (JitFunc)1;
}

void jit_free(JitFunc fn) {
    (void)fn;
}

SKITerm *jit_reduce(SKIPool *pool, SKITerm *term, i64 *steps) {
    stg_init();
    stg_reset();
    
    int status = setjmp(g_stg->exit_jmp);
    if (status != 0) {
        /* Error occurred */
        *steps = -1;
        return ski_i(pool);
    }
    
    /* Convert to STG representation */
    Closure *c = term_to_stg(g_stg, term);
    
    /* Reduce to HNF */
    Closure *result = stg_normalize(g_stg, c);
    
    /* Convert back */
    SKITerm *result_term = stg_to_term(pool, result);
    
    *steps = 0;  /* TODO: count steps */
    return result_term;
}

/* ========================================================================
 * NATIVE x86-64 STG IMPLEMENTATION
 * 
 * This compiles the STG machine to native code. Each info tag has a
 * corresponding entry point. The closure layout is the same as the
 * interpreted version, but we jump directly to code instead of switching.
 *
 * Native closure layout:
 *   [entry_code_ptr | payload...]
 *
 * The entry_code_ptr replaces the tag - we jump to it directly.
 * This is "tagless" - no runtime tag dispatch.
 *
 * Register allocation:
 *   rbx = current closure (Node)
 *   r12 = heap pointer (bump allocator)
 *   r13 = arg stack base (for underflow check)
 *   r14 = arg stack top (grows down)
 *   r15 = update stack top (grows down)
 *
 * Calling convention for entry code:
 *   - Closure in rbx
 *   - Args on stack at r14
 *   - Tail-call by jumping to [rbx] after setting up rbx
 * ======================================================================== */

#define CODE_SIZE (64 * 1024)
#define DEFAULT_HEAP_SIZE (16 * 1024)  /* 16KB default heap */
#define MAX_HEAP_SIZE (64 * 1024 * 1024)  /* 64MB max heap */
#define NATIVE_STACK_SIZE (256 * 1024)
#define UPDATE_STACK_SIZE (64 * 1024)

/* Configurable heap size - set via native_set_heap_size() or env vars */
static size_t g_initial_heap_size = DEFAULT_HEAP_SIZE;
static size_t g_max_heap_size = MAX_HEAP_SIZE;
static int g_gc_debug = 0;  /* Disable GC debug output by default */
static u64 g_gc_count = 0;  /* Global GC counter */

/* GC debug callback - called from assembly via indirect call */
static void gc_debug_callback(u64 from_used, u64 to_used) {
    g_gc_count++;
    if (g_gc_debug) {
        fprintf(stderr, "[GC #%lu] collected: %lu -> %lu bytes (%.1f%% recovered)\n",
                g_gc_count, from_used, to_used,
                from_used > 0 ? (1.0 - (double)to_used / from_used) * 100.0 : 0.0);
    }
}

/* Set heap sizes before calling native_reduce */
void native_set_heap_size(size_t initial, size_t max) {
    g_initial_heap_size = initial > 0 ? initial : DEFAULT_HEAP_SIZE;
    g_max_heap_size = max > 0 ? max : MAX_HEAP_SIZE;
}

void native_set_gc_debug(int enabled) {
    g_gc_debug = enabled;
}

u64 native_get_gc_count(void) {
    return g_gc_count;
}

typedef struct {
    u8 *buf;
    u32 pos;
    u32 cap;
} Code;

static void emit(Code *c, u8 b) {
    if (c->pos < c->cap) c->buf[c->pos++] = b;
}

static void emit32(Code *c, u32 v) {
    emit(c, v & 0xff);
    emit(c, (v >> 8) & 0xff);
    emit(c, (v >> 16) & 0xff);
    emit(c, (v >> 24) & 0xff);
}

static void emit64(Code *c, u64 v) {
    emit32(c, (u32)(v & 0xffffffff));
    emit32(c, (u32)(v >> 32));
}

/*
 * x86-64 instruction emission
 */

/* push reg */
static void emit_push(Code *c, int reg) {
    if (reg >= 8) { emit(c, 0x41); reg -= 8; }
    emit(c, 0x50 + reg);
}

/* pop reg */
static void emit_pop(Code *c, int reg) {
    if (reg >= 8) { emit(c, 0x41); reg -= 8; }
    emit(c, 0x58 + reg);
}

/* mov rax, [rbx + off] */
static void emit_mov_rax_rbx_off(Code *c, i32 off) {
    emit(c, 0x48); emit(c, 0x8b);
    if (off == 0) {
        emit(c, 0x03);
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x43); emit(c, (u8)off);
    } else {
        emit(c, 0x83); emit32(c, off);
    }
}

/* mov rbx, [rbx + off] */
static void emit_mov_rbx_rbx_off(Code *c, i32 off) {
    emit(c, 0x48); emit(c, 0x8b);
    if (off == 0) {
        emit(c, 0x1b);
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x5b); emit(c, (u8)off);
    } else {
        emit(c, 0x9b); emit32(c, off);
    }
}

/* mov rcx, [rbx + off] */
static void emit_mov_rcx_rbx_off(Code *c, i32 off) {
    emit(c, 0x48); emit(c, 0x8b);
    if (off == 0) {
        emit(c, 0x0b);
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x4b); emit(c, (u8)off);
    } else {
        emit(c, 0x8b); emit32(c, off);
    }
}

/* mov [r12 + off], reg */
static void emit_mov_r12_off_reg(Code *c, i32 off, int reg) {
    /* REX prefix for r12 and possibly reg */
    u8 rex = 0x49;
    if (reg >= 8) { rex |= 0x04; reg -= 8; }
    emit(c, rex);
    emit(c, 0x89);
    if (off == 0) {
        emit(c, 0x04 | (reg << 3)); emit(c, 0x24);  /* SIB for r12 */
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x44 | (reg << 3)); emit(c, 0x24); emit(c, (u8)off);
    } else {
        emit(c, 0x84 | (reg << 3)); emit(c, 0x24); emit32(c, off);
    }
}

/* mov rbx, r12 */
static void emit_mov_rbx_r12(Code *c) {
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);
}

/* mov rax, rbx */
static void emit_mov_rax_rbx(Code *c) {
    emit(c, 0x48); emit(c, 0x89); emit(c, 0xd8);
}

/* add r12, imm8 */
static void emit_add_r12_imm8(Code *c, i8 imm) {
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, (u8)imm);
}

/* jge rel32 */
static void emit_jge_rel32(Code *c, i32 rel) {
    emit(c, 0x0f); emit(c, 0x8d); emit32(c, rel);
}

/* jmp [rbx] - enter closure */
static void emit_enter_closure(Code *c) {
    emit(c, 0xff); emit(c, 0x23);  /* jmp [rbx] */
}

/* ret */
static void emit_ret(Code *c) {
    emit(c, 0xc3);
}

/* mov reg, [r8 + off] - load from RuntimeInfo */
static void emit_mov_reg_r8_off(Code *c, int reg, i32 off) {
    u8 rex = 0x49;  /* r8 is extended */
    if (reg >= 8) { rex |= 0x04; reg -= 8; }
    emit(c, rex);
    emit(c, 0x8b);
    if (off == 0) {
        emit(c, 0x00 | (reg << 3));
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x40 | (reg << 3)); emit(c, (u8)off);
    } else {
        emit(c, 0x80 | (reg << 3)); emit32(c, off);
    }
}

/* mov [r8 + off], reg - store to RuntimeInfo */
static void emit_mov_r8_off_reg(Code *c, i32 off, int reg) {
    u8 rex = 0x49;
    if (reg >= 8) { rex |= 0x04; reg -= 8; }
    emit(c, rex);
    emit(c, 0x89);
    if (off == 0) {
        emit(c, 0x00 | (reg << 3));
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x40 | (reg << 3)); emit(c, (u8)off);
    } else {
        emit(c, 0x80 | (reg << 3)); emit32(c, off);
    }
}

/* jmp [r8 + off] - indirect jump through RuntimeInfo */
static void emit_jmp_r8_off(Code *c, i32 off) {
    /* jmp qword ptr [r8 + off] */
    emit(c, 0x41);  /* REX.B for r8 */
    emit(c, 0xff);
    if (off == 0) {
        emit(c, 0x20);  /* ModRM: 00 100 000 = [r8], /4 for jmp */
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x60);  /* ModRM: 01 100 000 = [r8+disp8], /4 for jmp */
        emit(c, (u8)off);
    } else {
        emit(c, 0xa0);  /* ModRM: 10 100 000 = [r8+disp32], /4 for jmp */
        emit32(c, off);
    }
}

/* Forward declaration - defined in ELF section */
static void emit_cmp_reg_r8_off(Code *c, int reg, i32 off);

/*
 * GC check patch list - stores offsets of jae rel32 instructions to patch
 */
#define MAX_GC_PATCHES 64
static u32 gc_patches[MAX_GC_PATCHES];
static int gc_patch_count = 0;

/*
 * Emit GC check: if (r12 + alloc_size > heap_end) jmp gc_entry
 * Uses rax as scratch. Clobbers rax only.
 * Records patch location in gc_patches[] for later fixup.
 * Note: Uses RTI_HEAP_END offset (184) directly since it's defined later.
 */
static void emit_gc_check(Code *c, u32 alloc_size) {
    /* lea rax, [r12 + alloc_size] */
    emit(c, 0x49);  /* REX.WB - r12 is extended */
    emit(c, 0x8d);  /* lea */
    emit(c, 0x44);  /* ModRM: 01 000 100 = [r12 + disp8] + SIB */
    emit(c, 0x24);  /* SIB: 00 100 100 = [r12] */
    emit(c, (u8)alloc_size);
    
    /* cmp rax, [r8 + RTI_HEAP_END (184)] */
    emit_cmp_reg_r8_off(c, 0, 184);  /* RTI_HEAP_END = 184 */
    
    /* jae gc_entry (jump if above or equal - unsigned comparison) */
    emit(c, 0x0f);
    emit(c, 0x83);  /* jae rel32 */
    /* Record patch location and emit placeholder */
    gc_patches[gc_patch_count++] = c->pos;
    emit32(c, 0);  /* placeholder - will be patched */
}

/*
 * Patch all GC check jumps to point to gc_entry
 */
static void patch_gc_checks(Code *c, u32 gc_entry) {
    for (int i = 0; i < gc_patch_count; i++) {
        u32 patch_pos = gc_patches[i];
        *(i32*)(c->buf + patch_pos) = (i32)(gc_entry - (patch_pos + 4));
    }
    gc_patch_count = 0;  /* reset for next use */
}

/*
 * RuntimeInfo layout - pointed to by r8
 * This structure contains all addresses needed by the runtime.
 * Code is position-independent by loading addresses from here.
 */
#define RTI_DONE        0
#define RTI_S           8
#define RTI_K           16
#define RTI_I           24
#define RTI_S1          32
#define RTI_S2          40
#define RTI_K1          48
#define RTI_AP          56
#define RTI_IND         64
#define RTI_ARG_STACK   72
#define RTI_UPD_STACK   80
#define RTI_HEAP_PTR    88
#define RTI_S_CLOSURE   96
#define RTI_K_CLOSURE   104
#define RTI_I_CLOSURE   112
/* Continuation entries for CPS normalization */
#define RTI_CONT_DONE   120
#define RTI_CONT_APP    128
#define RTI_CONT_S1     136
#define RTI_CONT_S2X    144
#define RTI_CONT_S2Y    152
#define RTI_CONT_K1     160
/* GC fields */
#define RTI_SPACE0      168
#define RTI_SPACE1      176
#define RTI_HEAP_END    184
#define RTI_SPACE_SIZE  192
#define RTI_GC_ENTRY    200
#define RTI_GC_COUNT    208
/* ELF BCL output: CPS streaming */
#define RTI_OUTPUT_BUF  216   /* base of output buffer */
#define RTI_OUTPUT_POS  224   /* current write position */
#define RTI_OUTPUT_END  232   /* end of buffer (flush when pos >= end - margin) */
#define RTI_BCL_EMIT    240   /* bcl_emit entry point */
#define RTI_BCL_ARG     248   /* cont_bcl_arg entry point */
#define RTI_BCL_DONE    256   /* cont_bcl_done closure address */
#define RTI_SIZE        264

typedef struct {
    u64 done_entry;
    u64 S_entry;
    u64 K_entry;
    u64 I_entry;
    u64 S1_entry;
    u64 S2_entry;
    u64 K1_entry;
    u64 AP_entry;
    u64 IND_entry;
    u64 arg_stack_top;
    u64 upd_stack_top;
    u64 heap_ptr_save;
    u64 S_closure;
    u64 K_closure;
    u64 I_closure;
    /* Continuation entries for CPS normalization */
    u64 cont_done_entry;
    u64 cont_app_entry;
    u64 cont_s1_entry;
    u64 cont_s2x_entry;
    u64 cont_s2y_entry;
    u64 cont_k1_entry;
    /* GC fields */
    u64 space0;         /* base of semi-space 0 */
    u64 space1;         /* base of semi-space 1 */
    u64 heap_end;       /* end of current semi-space */
    u64 space_size;     /* size of each semi-space */
    u64 gc_entry;       /* entry point for GC routine */
    u64 gc_count;       /* number of GC cycles performed */
} RuntimeInfo;

/*
 * Entry point offsets relative to code start
 */
typedef struct {
    u32 trampoline;  /* only for in-memory runtime */
    u32 done;        /* only for in-memory runtime */
    u32 S;
    u32 K;
    u32 I;
    u32 S1;
    u32 S2;
    u32 K1;
    u32 AP;
    u32 IND;
    /* Continuation entries for CPS normalization */
    u32 cont_done;
    u32 cont_app;
    u32 cont_s1;
    u32 cont_s2x;
    u32 cont_s2y;
    u32 cont_k1;
    u32 norm_entry;  /* entry point for normalize(rbx, rcx) */
    u32 gc_entry;    /* entry point for GC routine */
} EntryOffsets;

/*
 * Emit reduction runtime (IND/I/K/K1/S/S1/S2/AP)
 * 
/*
 * Emit CPS normalizer - forward declaration
 */
static void emit_cps_normalize(Code *c, u64 code_base, EntryOffsets *eo);

/*
 * Emit Cheney GC routine - forward declaration
 */
static void emit_gc_routine(Code *c, EntryOffsets *eo);

/*
 * Emit the SHARED runtime core - used by BOTH native and ELF.
 * This emits: CPS normalizer, GC, entry markers.
 * 
 * Does NOT emit any platform-specific wrapper (no C trampoline).
 * Caller is responsible for setting up:
 *   rbx = closure to normalize
 *   rcx = done continuation (closure with entry that handles result)
 *   r12 = heap pointer
 *   r8  = RuntimeInfo pointer
 * Then jump to eo.norm_entry.
 */
static void emit_shared_runtime_core(Code *c, u64 code_base, EntryOffsets *eo) {
    /* Emit CPS normalizer - populates norm_entry, cont_done, cont_app, etc. */
    emit_cps_normalize(c, code_base, eo);
    
    /* eo.done = eo.cont_done for compatibility */
    eo->done = eo->cont_done;
    
    /* 
     * Entry markers - these are addresses used to identify closure types.
     * The normalizer compares [rbx] against RTI_S, RTI_K, etc.
     * These are just unique addresses, never actually executed.
     */
    eo->S = c->pos;
    emit(c, 0xcc);  /* int3 - should never be called */
    eo->K = c->pos;
    emit(c, 0xcc);
    eo->I = c->pos;
    emit(c, 0xcc);
    eo->S1 = c->pos;
    emit(c, 0xcc);
    eo->S2 = c->pos;
    emit(c, 0xcc);
    eo->K1 = c->pos;
    emit(c, 0xcc);
    eo->AP = c->pos;
    emit(c, 0xcc);
    eo->IND = c->pos;
    emit(c, 0xcc);
    
    /* Emit Cheney GC routine */
    emit_gc_routine(c, eo);
    
    /* Patch all GC check jumps to point to gc_entry */
    patch_gc_checks(c, eo->gc_entry);
}

/*
 * Emit native trampoline + shared runtime.
 * The trampoline is C ABI glue: saves registers, sets up state, calls core, restores, returns.
 * This is for calling from C code.
 */
static EntryOffsets emit_stg_runtime(Code *c, u64 code_base) {
    EntryOffsets eo = {0};
    
    /*
     * Native trampoline for C calling convention.
     * trampoline(closure, heap, runtime_info) -> result
     * rdi = closure, rsi = heap, rdx = runtime_info
     */
    eo.trampoline = c->pos;
    
    /* Save callee-saved registers */
    emit_push(c, 3);   /* rbx */
    emit_push(c, 12);  /* r12 */
    emit_push(c, 13);  /* r13 - not used but keep ABI consistent */
    emit_push(c, 14);  /* r14 - not used but keep ABI consistent */
    emit_push(c, 15);  /* r15 - not used but keep ABI consistent */
    emit_push(c, 8);   /* r8 - we use it for RuntimeInfo */
    
    /* Set up machine state */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0xfb);  /* mov rbx, rdi (closure) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0xf4);  /* mov r12, rsi (heap) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0xd0);  /* mov r8, rdx (runtime_info) */
    
    /* rcx = cont_done (from RuntimeInfo) - the initial continuation */
    emit_mov_reg_r8_off(c, 1, RTI_CONT_DONE);
    
    /* Jump to shared runtime (patch after emitting it) */
    u32 jmp_patch = c->pos;
    emit(c, 0xe9); emit32(c, 0);  /* jmp norm_entry (patch later) */
    
    /* Emit shared runtime core */
    emit_shared_runtime_core(c, code_base, &eo);
    
    /* Patch the jump to norm_entry */
    *(i32*)(c->buf + jmp_patch + 1) = (i32)(eo.norm_entry - (jmp_patch + 5));
    
    return eo;
}

/*
 * ========================================================================
 * TRUE CPS NORMALIZER
 * 
 * This is a completely CPS-transformed normalizer for HNF.
 * NO argument stack is used - all pending work is in continuation closures.
 * 
 * Convention:
 *   rbx = current closure to normalize
 *   rcx = current continuation (receives normalized result in rbx)
 *   r12 = heap pointer (bump allocation)
 *   r8  = RuntimeInfo (entry addresses)
 *   rsp = C call stack (only for final return)
 * 
 * normalize(term, k) =
 *   case term of
 *     S/K/I -> k(term)                          -- primitives are normal
 *     S1(x) -> normalize(x, cont_s1(k))         -- normalize argument
 *     S2(x,y) -> normalize(x, cont_s2x(y, k))   -- normalize x, then y
 *     K1(x) -> normalize(x, cont_k1(k))         -- normalize argument
 *     App(f, arg) -> normalize(f, cont_app(arg, k))  -- normalize f first
 *     IND(target) -> normalize(target, k)       -- follow indirection
 * 
 * cont_app receives normalized f, then applies arg:
 *   cont_app(arg, k)(f) =
 *     case f of
 *       I -> normalize(arg, k)                  -- I arg → arg
 *       K -> k(K1(arg))                         -- K arg → K1 (partial)
 *       S -> k(S1(arg))                         -- S arg → S1 (partial)
 *       K1(x) -> normalize(x, k)                -- K x arg → x
 *       S1(x) -> k(S2(x, arg))                  -- S x arg → S2 (partial)
 *       S2(x, y) ->                             -- S x y arg → (x arg) (y arg)
 *         let xz = App(x, arg)
 *         let yz = App(y, arg)
 *         normalize(App(xz, yz), k)
 * 
 * Continuation layouts (all start with entry pointer):
 *   cont_done:    [entry]                                  - 8 bytes
 *   cont_app:     [entry, arg, outer_k]                    - 24 bytes
 *   cont_s1:      [entry, orig_s1, outer_k]                - 24 bytes
 *   cont_s2x:     [entry, orig_s2, outer_k]                - 24 bytes
 *   cont_s2y:     [entry, orig_s2, norm_x, outer_k]        - 32 bytes
 *   cont_k1:      [entry, orig_k1, outer_k]                - 24 bytes
 * ======================================================================== */
static void emit_cps_normalize(Code *c, u64 code_base, EntryOffsets *eo) {
    
    /* ================================================================
     * norm_entry: normalize(rbx, rcx)
     * Entry point - dispatch based on closure type
     * ================================================================ */
    eo->norm_entry = c->pos;
    
    /* rax = [rbx] (entry pointer) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);  /* mov rax, [rbx] */
    
    /* Dispatch based on closure type */
    
    /* S - already normal */
    emit_cmp_reg_r8_off(c, 0, RTI_S);
    u32 je_S = c->pos; emit(c, 0x74); emit(c, 0);
    
    /* K - already normal */
    emit_cmp_reg_r8_off(c, 0, RTI_K);
    u32 je_K = c->pos; emit(c, 0x74); emit(c, 0);
    
    /* I - already normal */
    emit_cmp_reg_r8_off(c, 0, RTI_I);
    u32 je_I = c->pos; emit(c, 0x74); emit(c, 0);
    
    /* S1 - normalize argument */
    emit_cmp_reg_r8_off(c, 0, RTI_S1);
    emit(c, 0x0f); emit(c, 0x84); u32 je_S1 = c->pos; emit32(c, 0);
    
    /* S2 - normalize both arguments */
    emit_cmp_reg_r8_off(c, 0, RTI_S2);
    emit(c, 0x0f); emit(c, 0x84); u32 je_S2 = c->pos; emit32(c, 0);
    
    /* K1 - normalize argument */
    emit_cmp_reg_r8_off(c, 0, RTI_K1);
    emit(c, 0x0f); emit(c, 0x84); u32 je_K1 = c->pos; emit32(c, 0);
    
    /* AP - normalize function first */
    emit_cmp_reg_r8_off(c, 0, RTI_AP);
    emit(c, 0x0f); emit(c, 0x84); u32 je_AP = c->pos; emit32(c, 0);
    
    /* IND - follow indirection */
    emit_cmp_reg_r8_off(c, 0, RTI_IND);
    emit(c, 0x0f); emit(c, 0x84); u32 je_IND = c->pos; emit32(c, 0);
    
    /* Unknown - treat as normal, call continuation */
    /* call_cont: jmp [rcx] with rbx = result */
    u32 call_cont = c->pos;
    c->buf[je_S + 1] = (u8)(c->pos - je_S - 2);
    c->buf[je_K + 1] = (u8)(c->pos - je_K - 2);
    c->buf[je_I + 1] = (u8)(c->pos - je_I - 2);
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* ================================================================
     * handle_S1: normalize(S1(x), k) → normalize(x, cont_s1(orig, k))
     * ================================================================ */
    *(i32*)(c->buf + je_S1) = (i32)(c->pos - je_S1 - 4);
    /* GC check: need 24 bytes for cont_s1 */
    emit_gc_check(c, 24);
    /* Build cont_s1 at r12: [cont_s1_entry, orig_s1, outer_k] */
    emit_mov_reg_r8_off(c, 0, RTI_CONT_S1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx (orig_s1) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx (outer_k) */
    /* rbx = x = [orig_s1 + 8] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] */
    /* rcx = new continuation */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    /* Tail call to normalize */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * handle_S2: normalize(S2(x,y), k) → normalize(x, cont_s2x(orig, k))
     * ================================================================ */
    *(i32*)(c->buf + je_S2) = (i32)(c->pos - je_S2 - 4);
    /* GC check: need 24 bytes for cont_s2x */
    emit_gc_check(c, 24);
    emit_mov_reg_r8_off(c, 0, RTI_CONT_S2X);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx (orig_s2) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx (outer_k) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] - x */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * handle_K1: normalize(K1(x), k) → normalize(x, cont_k1(orig, k))
     * ================================================================ */
    *(i32*)(c->buf + je_K1) = (i32)(c->pos - je_K1 - 4);
    /* GC check: need 24 bytes for cont_k1 */
    emit_gc_check(c, 24);
    emit_mov_reg_r8_off(c, 0, RTI_CONT_K1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx (orig_k1) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx (outer_k) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] - x */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * handle_AP: normalize(App(f, arg), k) → normalize(f, cont_app(arg, k))
     * ================================================================ */
    *(i32*)(c->buf + je_AP) = (i32)(c->pos - je_AP - 4);
    /* GC check: need 24 bytes for cont_app */
    emit_gc_check(c, 24);
    emit_mov_reg_r8_off(c, 0, RTI_CONT_APP);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (cont_app_entry) */
    /* arg = [rbx + 16] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);  /* mov rax, [rbx+16] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx (outer_k) */
    /* rbx = f = [rbx + 8] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * handle_IND: normalize(IND(target), k) → normalize(target, k)
     * ================================================================ */
    *(i32*)(c->buf + je_IND) = (i32)(c->pos - je_IND - 4);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] - target */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * cont_done_entry: final continuation - return to C
     * rbx = normalized result
     * ================================================================ */
    eo->cont_done = c->pos;
    emit_mov_r8_off_reg(c, RTI_HEAP_PTR, 12);  /* save heap pointer */
    emit_mov_rax_rbx(c);                        /* rax = result */
    emit_pop(c, 8);
    emit_pop(c, 15);
    emit_pop(c, 14);
    emit_pop(c, 13);
    emit_pop(c, 12);
    emit_pop(c, 3);
    emit_ret(c);
    
    /* ================================================================
     * cont_app_entry: received normalized f, now apply arg
     * rcx = [entry, arg, outer_k]
     * rbx = normalized f
     * ================================================================ */
    eo->cont_app = c->pos;
    
    /* rax = f's entry */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);  /* mov rax, [rbx] */
    
    /* Check what f normalized to */
    
    /* I: I arg → normalize(arg, outer_k) */
    emit_cmp_reg_r8_off(c, 0, RTI_I);
    emit(c, 0x0f); emit(c, 0x84); u32 app_I = c->pos; emit32(c, 0);
    
    /* K: K arg → K1(arg), call outer_k */
    emit_cmp_reg_r8_off(c, 0, RTI_K);
    emit(c, 0x0f); emit(c, 0x84); u32 app_K = c->pos; emit32(c, 0);
    
    /* S: S arg → S1(arg), call outer_k */
    emit_cmp_reg_r8_off(c, 0, RTI_S);
    emit(c, 0x0f); emit(c, 0x84); u32 app_S = c->pos; emit32(c, 0);
    
    /* K1: K1(x) arg → normalize(x, outer_k) */
    emit_cmp_reg_r8_off(c, 0, RTI_K1);
    emit(c, 0x0f); emit(c, 0x84); u32 app_K1 = c->pos; emit32(c, 0);
    
    /* S1: S1(x) arg → S2(x, arg), call outer_k */
    emit_cmp_reg_r8_off(c, 0, RTI_S1);
    emit(c, 0x0f); emit(c, 0x84); u32 app_S1 = c->pos; emit32(c, 0);
    
    /* S2: S2(x,y) arg → normalize(App(App(x, arg), App(y, arg)), outer_k) */
    emit_cmp_reg_r8_off(c, 0, RTI_S2);
    emit(c, 0x0f); emit(c, 0x84); u32 app_S2 = c->pos; emit32(c, 0);
    
    /* Unknown f - shouldn't happen, but build App and call outer_k */
    /* GC check: need 24 bytes for App */
    emit_gc_check(c, 24);
    /* Build App(f, arg) */
    emit_mov_reg_r8_off(c, 0, RTI_AP);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (AP_entry) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx (f) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] (outer_k) */
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* ---- app_I: I arg → normalize(arg, outer_k) ---- */
    *(i32*)(c->buf + app_I) = (i32)(c->pos - app_I - 4);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] (arg) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] (outer_k) */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ---- app_K: K arg → k(K1(arg)) ---- */
    *(i32*)(c->buf + app_K) = (i32)(c->pos - app_K - 4);
    /* GC check: need 16 bytes for K1 */
    emit_gc_check(c, 16);
    emit_mov_reg_r8_off(c, 0, RTI_K1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (K1_entry) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 (new K1) */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add r12, 16 */
    /* But wait - we need to normalize the arg! K1(arg) needs normalized arg */
    /* Actually no - K1 is a partial application, arg doesn't need normalizing yet */
    /* The arg will be normalized when K1 is pattern-matched later */
    /* So we just build K1(arg) and call the continuation to normalize it */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] (outer_k) */
    /* Now normalize the K1 we just built */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ---- app_S: S arg → normalize(S1(arg), outer_k) ---- */
    *(i32*)(c->buf + app_S) = (i32)(c->pos - app_S - 4);
    /* GC check: need 16 bytes for S1 */
    emit_gc_check(c, 16);
    emit_mov_reg_r8_off(c, 0, RTI_S1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add r12, 16 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ---- app_K1: K1(x) arg → normalize(x, outer_k) ---- */
    *(i32*)(c->buf + app_K1) = (i32)(c->pos - app_K1 - 4);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] (x from K1) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] (outer_k) */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ---- app_S1: S1(x) arg → normalize(S2(x, arg), outer_k) ---- */
    *(i32*)(c->buf + app_S1) = (i32)(c->pos - app_S1 - 4);
    /* GC check: need 24 bytes for S2 */
    emit_gc_check(c, 24);
    emit_mov_reg_r8_off(c, 0, RTI_S2);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (S2_entry) */
    /* x from S1 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x08);  /* mov rax, [rbx+8] (x) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    /* arg from continuation */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ---- app_S2: S2(x,y) arg → normalize(App(App(x, arg), App(y, arg)), outer_k) ---- */
    *(i32*)(c->buf + app_S2) = (i32)(c->pos - app_S2 - 4);
    /* GC check: need 72 bytes for 3 App closures (24 * 3) */
    emit_gc_check(c, 72);
    /* Build App(y, arg) */
    emit_push(c, 3);  /* save S2 closure */
    emit_push(c, 1);  /* save cont */
    emit_mov_reg_r8_off(c, 0, RTI_AP);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (AP_entry) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);  /* mov rax, [rbx+16] (y) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rax */
    /* rdx = App(y, arg) */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe2);  /* mov rdx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    /* Build App(x, arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax... wait rax has arg */
    /* Need to reload AP_entry */
    emit_mov_reg_r8_off(c, 0, RTI_AP);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax (AP_entry) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x08);  /* mov rax, [rbx+8] (x) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rax */
    /* Save App(x, arg) */
    emit_push(c, 2);  /* save rdx = App(y, arg) */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe0);  /* mov rax, r12 (App(x, arg)) */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    /* Build App(App(x, arg), App(y, arg)) */
    emit_mov_reg_r8_off(c, 1, RTI_AP);  /* rcx = AP_entry temporarily */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x0c); emit(c, 0x24);  /* mov [r12], rcx (AP_entry) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax (App(x,arg)) */
    emit_pop(c, 0);  /* rax = App(y, arg) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 (new App) */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    emit_pop(c, 1);  /* rcx = outer cont */
    emit_pop(c, 0);  /* discard saved S2 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] (outer_k) */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * cont_s1_entry: received normalized x for S1
     * rcx = [entry, orig_s1, outer_k]
     * rbx = normalized x
     * Build S1(normalized_x) or reuse if unchanged, then call outer_k
     * ================================================================ */
    eo->cont_s1 = c->pos;
    /* GC check: might need 16 bytes for new S1 */
    emit_gc_check(c, 16);
    /* Compare old x vs new x */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (orig_s1) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x50); emit(c, 0x08);  /* mov rdx, [rax+8] (old x) */
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xd3);  /* cmp rbx, rdx */
    u32 s1_same = c->pos; emit(c, 0x74); emit(c, 0);  /* je same */
    /* Different: build new S1 */
    emit_mov_reg_r8_off(c, 0, RTI_S1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add r12, 16 */
    u32 s1_done = c->pos; emit(c, 0xeb); emit(c, 0);  /* jmp done */
    /* Same: use original */
    c->buf[s1_same + 1] = (u8)(c->pos - s1_same - 2);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] (orig_s1) */
    c->buf[s1_done + 1] = (u8)(c->pos - s1_done - 2);
    /* Call outer continuation */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] */
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* ================================================================
     * cont_s2x_entry: received normalized x for S2, now normalize y
     * rcx = [entry, orig_s2, outer_k]
     * rbx = normalized x
     * ================================================================ */
    eo->cont_s2x = c->pos;
    /* GC check: need 32 bytes for cont_s2y */
    emit_gc_check(c, 32);
    /* Build cont_s2y = [entry, orig_s2, norm_x, outer_k] */
    emit_mov_reg_r8_off(c, 0, RTI_CONT_S2Y);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (orig_s2) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rbx (norm_x) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x10);  /* mov rax, [rcx+16] (outer_k) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x18);  /* mov [r12+24], rax */
    /* rbx = y from orig_s2 */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] (orig_s2) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x10);  /* mov rbx, [rbx+16] (y) */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 32);  /* add r12, 32 */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
    
    /* ================================================================
     * cont_s2y_entry: received normalized y for S2
     * rcx = [entry, orig_s2, norm_x, outer_k]
     * rbx = normalized y
     * ================================================================ */
    eo->cont_s2y = c->pos;
    /* GC check: might need 24 bytes for new S2 */
    emit_gc_check(c, 24);
    /* Compare x and y */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (orig_s2) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x51); emit(c, 0x10);  /* mov rdx, [rcx+16] (norm_x) */
    /* Compare norm_x vs old x */
    emit(c, 0x48); emit(c, 0x3b); emit(c, 0x50); emit(c, 0x08);  /* cmp rdx, [rax+8] */
    u32 s2_x_diff = c->pos; emit(c, 0x75); emit(c, 0);  /* jne diff */
    /* Compare norm_y (rbx) vs old y */
    emit(c, 0x48); emit(c, 0x3b); emit(c, 0x58); emit(c, 0x10);  /* cmp rbx, [rax+16] */
    u32 s2_y_diff = c->pos; emit(c, 0x75); emit(c, 0);  /* jne diff */
    /* Both same: use original */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] (orig_s2) */
    u32 s2_done = c->pos; emit(c, 0xeb); emit(c, 0);  /* jmp done */
    /* Different: build new S2 */
    c->buf[s2_x_diff + 1] = (u8)(c->pos - s2_x_diff - 2);
    c->buf[s2_y_diff + 1] = (u8)(c->pos - s2_y_diff - 2);
    emit_mov_reg_r8_off(c, 0, RTI_S2);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x10);  /* mov rax, [rcx+16] (norm_x) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rbx (norm_y) */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    c->buf[s2_done + 1] = (u8)(c->pos - s2_done - 2);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x18);  /* mov rcx, [rcx+24] (outer_k) */
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* ================================================================
     * cont_k1_entry: received normalized x for K1
     * rcx = [entry, orig_k1, outer_k]
     * rbx = normalized x
     * ================================================================ */
    eo->cont_k1 = c->pos;
    /* GC check: might need 16 bytes for new K1 */
    emit_gc_check(c, 16);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x41); emit(c, 0x08);  /* mov rax, [rcx+8] (orig_k1) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x50); emit(c, 0x08);  /* mov rdx, [rax+8] (old x) */
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xd3);  /* cmp rbx, rdx */
    u32 k1_same = c->pos; emit(c, 0x74); emit(c, 0);  /* je same */
    /* Different: build new K1 */
    emit_mov_reg_r8_off(c, 0, RTI_K1);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x5c); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rbx */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add r12, 16 */
    u32 k1_done = c->pos; emit(c, 0xeb); emit(c, 0);  /* jmp done */
    /* Same: use original */
    c->buf[k1_same + 1] = (u8)(c->pos - k1_same - 2);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] */
    c->buf[k1_done + 1] = (u8)(c->pos - k1_done - 2);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] */
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    (void)call_cont;
}

/* ========================================================================
 * Cheney Copying GC for CPS runtime
 * 
 * This is a CPS continuation itself: receives (rbx, rcx), forwards both,
 * then jumps to norm_entry to retry the allocation that triggered GC.
 *
 * Register usage during GC:
 *   rbx = current term (root 1) - updated during GC
 *   rcx = current continuation (root 2) - updated during GC
 *   r12 = allocation pointer in to-space (scan pointer during GC)
 *   r8  = RuntimeInfo pointer (preserved)
 *   rdi = scan pointer during Cheney loop
 *   rsi = from-space base (for in_from_space check)
 *   rdx = from-space end
 *   rax = scratch
 *
 * Closure sizes (bytes):
 *   S/K/I/cont_done = 8
 *   S1/K1/IND = 16
 *   S2/AP/cont_app/cont_s1/cont_k1/cont_s2x = 24
 *   cont_s2y = 32
 * ======================================================================== */

/*
 * Emit gc_copy code inline.
 * Input: rbx = pointer to object
 * Output: rbx = forwarded pointer (either new location or followed forward ptr)
 * Preserves: rcx, rdi, r8, r12 (except r12 advances when copying)
 * Uses: rax as scratch, rsi/rdx must contain from_base/from_end
 *
 * Uses near jumps (rel32) to handle large code distances.
 */
static void emit_gc_copy_inline(Code *c) {
    /* Check if rbx in from_space */
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xf3);  /* cmp rbx, rsi (from_base) */
    u32 skip1 = c->pos;
    emit(c, 0x0f); emit(c, 0x82); emit32(c, 0);  /* jb skip (near jump) */
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xd3);  /* cmp rbx, rdx (from_end) */
    u32 skip2 = c->pos;
    emit(c, 0x0f); emit(c, 0x83); emit32(c, 0);  /* jae skip (near jump) */
    
    /* rbx is in from_space. Check if already forwarded */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);  /* mov rax, [rbx] */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_IND);  /* cmp rax, [r8+RTI_IND] */
    u32 forwarded = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);  /* je forwarded (near jump) */
    
    /* Not forwarded: dispatch on type to get size */
    /* rax = entry pointer from [rbx] */
    
    /* Check for size-8 types: S, K, I, cont_done */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_S);
    u32 sz8_S = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_K);
    u32 sz8_K = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_I);
    u32 sz8_I = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80); emit32(c, RTI_CONT_DONE);
    u32 sz8_done = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* Check for size-16 types: S1, K1, IND */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_S1);
    u32 sz16_S1 = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_K1);
    u32 sz16_K1 = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_IND);
    u32 sz16_IND = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* Check for size-32: cont_s2y */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80); emit32(c, RTI_CONT_S2Y);
    u32 sz32 = c->pos; emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* Default: size-24 (S2, AP, cont_app, cont_s1, cont_k1, cont_s2x) */
    /* Copy 24 bytes */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);        /* mov rax, [rbx] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x08);  /* mov rax, [rbx+8] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);  /* mov rax, [rbx+16] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);
    /* Install forwarding pointer */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x40); emit(c, RTI_IND);  /* mov rax, [r8+RTI_IND] */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x03);        /* mov [rbx], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0x63); emit(c, 0x08);  /* mov [rbx+8], r12 */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);  /* add r12, 24 */
    u32 done_24 = c->pos; emit(c, 0xe9); emit32(c, 0);  /* jmp done (near) */
    
    /* Size-8 */
    *(i32*)&c->buf[sz8_S + 2] = (i32)(c->pos - sz8_S - 6);
    *(i32*)&c->buf[sz8_K + 2] = (i32)(c->pos - sz8_K - 6);
    *(i32*)&c->buf[sz8_I + 2] = (i32)(c->pos - sz8_I - 6);
    *(i32*)&c->buf[sz8_done + 2] = (i32)(c->pos - sz8_done - 6);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);        /* mov rax, [rbx] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x40); emit(c, RTI_IND);
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x03);        /* mov [rbx], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0x63); emit(c, 0x08);  /* mov [rbx+8], r12 */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 8);  /* add r12, 8 */
    u32 done_8 = c->pos; emit(c, 0xe9); emit32(c, 0);  /* jmp done (near) */
    
    /* Size-16 */
    *(i32*)&c->buf[sz16_S1 + 2] = (i32)(c->pos - sz16_S1 - 6);
    *(i32*)&c->buf[sz16_K1 + 2] = (i32)(c->pos - sz16_K1 - 6);
    *(i32*)&c->buf[sz16_IND + 2] = (i32)(c->pos - sz16_IND - 6);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);        /* mov rax, [rbx] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x08);  /* mov rax, [rbx+8] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x40); emit(c, RTI_IND);
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x03);
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0x63); emit(c, 0x08);
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add r12, 16 */
    u32 done_16 = c->pos; emit(c, 0xe9); emit32(c, 0);  /* jmp done (near) */
    
    /* Size-32 */
    *(i32*)&c->buf[sz32 + 2] = (i32)(c->pos - sz32 - 6);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);        /* mov rax, [rbx] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x08);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x10);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x18);  /* mov rax, [rbx+24] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x18);
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x40); emit(c, RTI_IND);
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x03);
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0x63); emit(c, 0x08);
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe3);  /* mov rbx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 32);  /* add r12, 32 */
    /* falls through to done */
    
    /* All paths converge here */
    *(i32*)&c->buf[done_24 + 1] = (i32)(c->pos - done_24 - 5);
    *(i32*)&c->buf[done_8 + 1] = (i32)(c->pos - done_8 - 5);
    *(i32*)&c->buf[done_16 + 1] = (i32)(c->pos - done_16 - 5);
    u32 copy_done = c->pos;
    emit(c, 0xe9); emit32(c, 0);  /* jmp to end (near) */
    
    /* Already forwarded: follow forward pointer */
    *(i32*)&c->buf[forwarded + 2] = (i32)(c->pos - forwarded - 6);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] */
    u32 fwd_done = c->pos;
    emit(c, 0xe9); emit32(c, 0);  /* jmp end (near) */
    
    /* Not in from_space: keep rbx as-is */
    *(i32*)&c->buf[skip1 + 2] = (i32)(c->pos - skip1 - 6);
    *(i32*)&c->buf[skip2 + 2] = (i32)(c->pos - skip2 - 6);
    *(i32*)&c->buf[copy_done + 1] = (i32)(c->pos - copy_done - 5);
    *(i32*)&c->buf[fwd_done + 1] = (i32)(c->pos - fwd_done - 5);
    /* rbx unchanged, continue */
}

static void emit_gc_routine(Code *c, EntryOffsets *eo) {
    eo->gc_entry = c->pos;
    
    /* 
     * GC entry point
     * rbx = term, rcx = continuation, r12 = heap ptr (at limit)
     * r8 = RTI
     */
    
    /* Increment GC counter: [r8 + RTI_GC_COUNT]++ */
    emit(c, 0x49); emit(c, 0xff); emit(c, 0x80);  /* inc qword [r8 + disp32] */
    emit32(c, RTI_GC_COUNT);
    
    /* Step 1: Determine from-space and to-space
     * from-space = current active space (where objects are)
     * to-space = other space (where we'll copy to)
     * 
     * Current active space has objects; we need to find its base.
     * After GC, we swap: to-space becomes active.
     */
    
    /* rsi = from_base = space[active] = current space base
     * We need to figure out which space is active.
     * r12 is somewhere in the active space.
     * Compare r12 against space0: if r12 >= space0 && r12 < space0+size, active=0
     */
    
    /* For simplicity: space0 is always the initial active space.
     * After each GC, we toggle. Track via a flag in RTI? 
     * Actually, we can compute: if r12 is in space0, from=space0, to=space1
     */
    
    /* rsi = [r8 + RTI_SPACE0] */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb0);  /* mov rsi, [r8 + disp32] */
    emit32(c, RTI_SPACE0);
    
    /* rax = [r8 + RTI_SPACE_SIZE] */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);  /* mov rax, [r8 + disp32] */
    emit32(c, RTI_SPACE_SIZE);
    
    /* rdx = rsi + rax = space0_end */
    emit(c, 0x48); emit(c, 0x8d); emit(c, 0x14); emit(c, 0x06);  /* lea rdx, [rsi + rax] */
    
    /* if r12 >= rsi && r12 < rdx, then from=space0, to=space1 */
    /* cmp r12, rsi */
    emit(c, 0x4c); emit(c, 0x39); emit(c, 0xe6);  /* cmp rsi, r12 */
    u32 jmp_space1 = c->pos;
    emit(c, 0x77); emit(c, 0);  /* ja space1_active (r12 < space0 base) */
    
    /* cmp r12, rdx */
    emit(c, 0x4c); emit(c, 0x39); emit(c, 0xe2);  /* cmp rdx, r12 */
    u32 jmp_space1_2 = c->pos;
    emit(c, 0x76); emit(c, 0);  /* jbe space1_active (r12 >= space0 end) */
    
    /* space0 is active (from), space1 is to */
    /* rsi = from_base = space0 (already in rsi) */
    /* rdi = to_base = [r8 + RTI_SPACE1] */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb8);  /* mov rdi, [r8 + disp32] */
    emit32(c, RTI_SPACE1);
    u32 jmp_got_spaces = c->pos;
    emit(c, 0xeb); emit(c, 0);  /* jmp got_spaces */
    
    /* space1_active: space1 is from, space0 is to */
    c->buf[jmp_space1 + 1] = (u8)(c->pos - jmp_space1 - 2);
    c->buf[jmp_space1_2 + 1] = (u8)(c->pos - jmp_space1_2 - 2);
    /* rdi = to_base = space0 (currently in rsi) */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0xf7);  /* mov rdi, rsi */
    /* rsi = from_base = space1 */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb0);  /* mov rsi, [r8 + disp32] */
    emit32(c, RTI_SPACE1);
    
    /* got_spaces: rsi = from_base, rdi = to_base */
    c->buf[jmp_got_spaces + 1] = (u8)(c->pos - jmp_got_spaces - 2);
    
    /* rdx = from_end = rsi + space_size */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);  /* mov rax, [r8 + RTI_SPACE_SIZE] */
    emit32(c, RTI_SPACE_SIZE);
    emit(c, 0x48); emit(c, 0x8d); emit(c, 0x14); emit(c, 0x06);  /* lea rdx, [rsi + rax] */
    
    /* r12 = to_hp = to_base (start allocating at beginning of to-space) */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0xfc);  /* mov r12, rdi */
    
    /* Save from_base (rsi) and from_end (rdx) on stack for scavenge loop */
    emit_push(c, 6);  /* push rsi (from_base) */
    emit_push(c, 2);  /* push rdx (from_end) */
    
    /* Step 2: Copy roots - rbx and rcx using emit_gc_copy_inline */
    
    /* Copy rbx (current term) - emit_gc_copy_inline takes rbx, returns rbx */
    emit_gc_copy_inline(c);
    
    /* Copy rcx (continuation) - swap to rbx, copy, swap back */
    /* Reload from_base/from_end from stack */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x14); emit(c, 0x24);  /* mov rdx, [rsp] (from_end) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x74); emit(c, 0x24); emit(c, 0x08);  /* mov rsi, [rsp+8] (from_base) */
    /* xchg rbx, rcx */
    emit(c, 0x48); emit(c, 0x87); emit(c, 0xcb);  /* xchg rbx, rcx */
    emit_gc_copy_inline(c);
    /* xchg back */
    emit(c, 0x48); emit(c, 0x87); emit(c, 0xcb);  /* xchg rbx, rcx */
    
    /* Save forwarded roots for after scavenge loop */
    /* Stack is: [from_end] [from_base] ... */
    /* After pushing: [rcx] [rbx] [from_end] [from_base] ... */
    emit_push(c, 3);  /* push rbx (forwarded current term) */
    emit_push(c, 1);  /* push rcx (forwarded continuation) */
    
    /* Step 3: Cheney scan loop
     * rdi = scan pointer (starts at to_base, chases r12)
     * r12 = alloc pointer (grows as we copy)
     * For each object at scan, copy its children, advance scan by object size
     */
    
    /* rdi = to_base (saved on stack? No, we need to recompute) */
    /* Actually, we set r12 = to_base earlier, and it grew from there */
    /* scan = to_base = r12 - (bytes allocated so far) */
    /* Simpler: save to_base before root copying */
    
    /* Let's restart: save to_base in a callee-saved register before copying roots */
    /* Actually, use r13 (it's available, we don't use it in CPS normalize) */
    /* But that would require refactoring... */
    
    /* Alternative: pop from_base/from_end, recompute to_base from RTI */
    /* Stack: [rcx] [rbx] [from_end] [from_base] ... */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x54); emit(c, 0x24); emit(c, 0x10);  /* mov rdx, [rsp+16] (from_end) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x74); emit(c, 0x24); emit(c, 0x18);  /* mov rsi, [rsp+24] (from_base) */
    
    /* rdi = to_base. If from_base == space0, to_base = space1, else to_base = space0 */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);  /* mov rax, [r8 + RTI_SPACE0] */
    emit32(c, RTI_SPACE0);
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xc6);  /* cmp rsi, rax */
    u32 jmp_to_space1 = c->pos;
    emit(c, 0x75); emit(c, 0);  /* jne to_space1 */
    /* from = space0, so to = space1 */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb8);  /* mov rdi, [r8 + RTI_SPACE1] */
    emit32(c, RTI_SPACE1);
    u32 jmp_got_to = c->pos;
    emit(c, 0xeb); emit(c, 0);  /* jmp got_to */
    c->buf[jmp_to_space1 + 1] = (u8)(c->pos - jmp_to_space1 - 2);
    /* from = space1, so to = space0 */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb8);  /* mov rdi, [r8 + RTI_SPACE0] */
    emit32(c, RTI_SPACE0);
    c->buf[jmp_got_to + 1] = (u8)(c->pos - jmp_got_to - 2);
    
    /* Now: rdi = scan = to_base, r12 = alloc (after copying roots) */
    
    /* Cheney loop: while scan < alloc */
    u32 scan_loop = c->pos;
    emit(c, 0x4c); emit(c, 0x39); emit(c, 0xe7);  /* cmp rdi, r12 */
    u32 scan_done = c->pos;
    emit(c, 0x0f); emit(c, 0x83); emit32(c, 0);  /* jae done (near jump) */
    
    /* Scavenge object at rdi: update its pointer fields */
    /* Load entry to determine type and size */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x07);  /* mov rax, [rdi] (entry) */
    
    /* Dispatch on type to scavenge fields and get size */
    /* All jumps are near jumps (6 bytes) since scavenge cases are large */
    
    /* S/K/I: size=8, no fields */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_S);  /* cmp rax, [r8+RTI_S] */
    u32 is_S = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);  /* je near */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_K);
    u32 is_K = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_I);
    u32 is_I = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* S1/K1: size=16, one field at +8 */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_S1);
    u32 is_S1 = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_K1);
    u32 is_K1 = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* S2/AP: size=24, two fields at +8, +16 */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_S2);
    u32 is_S2 = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_AP);
    u32 is_AP = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* cont_app/s1/k1/s2x: size=24, fields at +8, +16 */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);  /* cmp rax, [r8+disp32] */
    emit32(c, RTI_CONT_APP);
    u32 is_cont_app = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);
    emit32(c, RTI_CONT_S1);
    u32 is_cont_s1 = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);
    emit32(c, RTI_CONT_K1);
    u32 is_cont_k1 = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);
    emit32(c, RTI_CONT_S2X);
    u32 is_cont_s2x = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* cont_s2y: size=32, fields at +8, +16, +24 */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);
    emit32(c, RTI_CONT_S2Y);
    u32 is_cont_s2y = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* cont_done: size=8, no fields (or it's static) */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x80);
    emit32(c, RTI_CONT_DONE);
    u32 is_cont_done = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* IND: size=16, one field at +8 (the target) */
    emit(c, 0x49); emit(c, 0x3b); emit(c, 0x40); emit(c, RTI_IND);
    u32 is_IND = c->pos;
    emit(c, 0x0f); emit(c, 0x84); emit32(c, 0);
    
    /* Unknown type - assume size 8, no fields (shouldn't happen) */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc7); emit(c, 8);  /* add rdi, 8 */
    emit(c, 0xe9); emit32(c, (i32)(scan_loop - (c->pos + 4)));  /* jmp scan_loop */
    
    /* S/K/I/cont_done: size 8, no fields */
    *(i32*)&c->buf[is_S + 2] = (i32)(c->pos - is_S - 6);
    *(i32*)&c->buf[is_K + 2] = (i32)(c->pos - is_K - 6);
    *(i32*)&c->buf[is_I + 2] = (i32)(c->pos - is_I - 6);
    *(i32*)&c->buf[is_cont_done + 2] = (i32)(c->pos - is_cont_done - 6);
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc7); emit(c, 8);  /* add rdi, 8 */
    emit(c, 0xe9); emit32(c, (i32)(scan_loop - (c->pos + 4)));
    
    /* S1/K1/IND: size 16, one field at +8 */
    *(i32*)&c->buf[is_S1 + 2] = (i32)(c->pos - is_S1 - 6);
    *(i32*)&c->buf[is_K1 + 2] = (i32)(c->pos - is_K1 - 6);
    *(i32*)&c->buf[is_IND + 2] = (i32)(c->pos - is_IND - 6);
    /* Scavenge [rdi+8]: one field */
    emit_push(c, 7);  /* save rdi */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x08);  /* mov rbx, [rdi+8] */
    emit_gc_copy_inline(c);
    emit_pop(c, 7);  /* restore rdi */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x08);  /* mov [rdi+8], rbx */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc7); emit(c, 16);  /* add rdi, 16 */
    emit(c, 0xe9); emit32(c, (i32)(scan_loop - (c->pos + 4)));
    
    /* S2/AP/cont_app/s1/k1/s2x: size 24, two fields at +8, +16 */
    *(i32*)&c->buf[is_S2 + 2] = (i32)(c->pos - is_S2 - 6);
    *(i32*)&c->buf[is_AP + 2] = (i32)(c->pos - is_AP - 6);
    *(i32*)&c->buf[is_cont_app + 2] = (i32)(c->pos - is_cont_app - 6);
    *(i32*)&c->buf[is_cont_s1 + 2] = (i32)(c->pos - is_cont_s1 - 6);
    *(i32*)&c->buf[is_cont_k1 + 2] = (i32)(c->pos - is_cont_k1 - 6);
    *(i32*)&c->buf[is_cont_s2x + 2] = (i32)(c->pos - is_cont_s2x - 6);
    /* Scavenge [rdi+8] and [rdi+16] */
    emit_push(c, 7);  /* save rdi */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x08);  /* mov rbx, [rdi+8] */
    emit_gc_copy_inline(c);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x3c); emit(c, 0x24);  /* mov rdi, [rsp] (peek) */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x08);  /* mov [rdi+8], rbx */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x10);  /* mov rbx, [rdi+16] */
    emit_gc_copy_inline(c);
    emit_pop(c, 7);  /* restore rdi */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x10);  /* mov [rdi+16], rbx */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc7); emit(c, 24);  /* add rdi, 24 */
    emit(c, 0xe9); emit32(c, (i32)(scan_loop - (c->pos + 4)));
    
    /* cont_s2y: size 32, three fields at +8, +16, +24 */
    *(i32*)&c->buf[is_cont_s2y + 2] = (i32)(c->pos - is_cont_s2y - 6);
    emit_push(c, 7);  /* save rdi */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x08);  /* mov rbx, [rdi+8] */
    emit_gc_copy_inline(c);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x3c); emit(c, 0x24);  /* mov rdi, [rsp] */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x08);  /* mov [rdi+8], rbx */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x10);  /* mov rbx, [rdi+16] */
    emit_gc_copy_inline(c);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x3c); emit(c, 0x24);  /* mov rdi, [rsp] */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x10);  /* mov [rdi+16], rbx */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5f); emit(c, 0x18);  /* mov rbx, [rdi+24] */
    emit_gc_copy_inline(c);
    emit_pop(c, 7);  /* restore rdi */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x5f); emit(c, 0x18);  /* mov [rdi+24], rbx */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc7); emit(c, 32);  /* add rdi, 32 */
    emit(c, 0xe9); emit32(c, (i32)(scan_loop - (c->pos + 4)));
    
    /* scan_done: finished Cheney loop */
    *(i32*)&c->buf[scan_done + 2] = (i32)(c->pos - scan_done - 6);
    
    /* Restore forwarded roots */
    /* Stack: [rcx] [rbx] [from_end] [from_base] ... */
    emit_pop(c, 1);  /* pop rcx (forwarded continuation) */
    emit_pop(c, 3);  /* pop rbx (forwarded current term) */
    
    /* Step 4: Swap spaces - update RTI fields */
    /* Clean up stack: [from_end] [from_base] still on stack */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc4); emit(c, 16);  /* add rsp, 16 (pop from_end, from_base) */
    
    /* The new to-space is now active. Update heap_end. */
    /* heap_end = to_base + space_size */
    /* to_base is where rdi started, which we can get from space0/space1 */
    /* if from_base was space0, new active is space1, so store space1 info */
    /* Actually: r12 is the new allocation pointer, heap_end = to_base + space_size */
    
    /* Recompute to_base into rax */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);  /* mov rax, [r8+RTI_SPACE0] */
    emit32(c, RTI_SPACE0);
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xc6);  /* cmp rsi, rax (rsi still has from_base) */
    u32 swap_to1 = c->pos;
    emit(c, 0x75); emit(c, 0);
    /* from was space0, to is space1 */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);
    emit32(c, RTI_SPACE1);
    u32 swap_done = c->pos;
    emit(c, 0xeb); emit(c, 0);
    c->buf[swap_to1 + 1] = (u8)(c->pos - swap_to1 - 2);
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0x80);
    emit32(c, RTI_SPACE0);
    c->buf[swap_done + 1] = (u8)(c->pos - swap_done - 2);
    
    /* rax = new active space base (to_base) */
    /* heap_end = rax + space_size */
    /* Use rdi as temp (no longer needed after scan loop) */
    emit(c, 0x49); emit(c, 0x8b); emit(c, 0xb8);  /* mov rdi, [r8+RTI_SPACE_SIZE] */
    emit32(c, RTI_SPACE_SIZE);
    emit(c, 0x48); emit(c, 0x01); emit(c, 0xf8);  /* add rax, rdi */
    /* Store heap_end */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x80);  /* mov [r8+RTI_HEAP_END], rax */
    emit32(c, RTI_HEAP_END);
    
    /* Step 5: Jump back to norm_entry to retry */
    emit(c, 0xe9); emit32(c, (i32)(eo->norm_entry - (c->pos + 4)));
}

/*
 * Native runtime state
 */
typedef struct {
    u8 *code;
    u8 *heap[2];          /* two semi-spaces for copying GC */
    int active_space;     /* which space is currently active (0 or 1) */
    u8 *heap_base;        /* start of user data (after primitives) */
    u8 *primitives;       /* separate buffer for S/K/I/cont_done - outside GC */
    u8 *arg_stack;
    u8 *update_stack;
    
    RuntimeInfo *rti;     /* RuntimeInfo for this instance */
    EntryOffsets eo;      /* Entry point offsets */
    
    void *S_closure;
    void *K_closure;
    void *I_closure;
} NativeRT;

static NativeRT *g_native = NULL;

/*
 * Build native runtime using PIC code
 */
static void build_native_runtime(void) {
    if (g_native) return;
    
    g_native = malloc(sizeof(NativeRT));
    g_native->code = mmap(NULL, CODE_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC,
                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    /* Check environment variables for heap size config */
    char *env_initial = getenv("EEZO_HEAP_SIZE");
    char *env_max = getenv("EEZO_MAX_HEAP");
    char *env_debug = getenv("EEZO_GC_DEBUG");
    if (env_initial) g_initial_heap_size = (size_t)atol(env_initial);
    if (env_max) g_max_heap_size = (size_t)atol(env_max);
    if (env_debug) g_gc_debug = atoi(env_debug);
    
    if (g_gc_debug) {
        fprintf(stderr, "[GC] Initial heap: %zu bytes, Max heap: %zu bytes\n",
                g_initial_heap_size, g_max_heap_size);
    }
    
    /* Allocate two semi-spaces for copying GC */
    g_native->heap[0] = malloc(g_initial_heap_size);
    g_native->heap[1] = malloc(g_initial_heap_size);
    g_native->active_space = 0;
    g_native->primitives = malloc(64);  /* S/K/I/cont_done closures - outside GC heap */
    g_native->arg_stack = malloc(NATIVE_STACK_SIZE);
    g_native->update_stack = malloc(UPDATE_STACK_SIZE);
    g_native->rti = malloc(sizeof(RuntimeInfo));
    
    Code c = { g_native->code, 0, CODE_SIZE };
    u64 code_base = (u64)g_native->code;
    
    /* Emit PIC runtime code */
    g_native->eo = emit_stg_runtime(&c, code_base);
    
    /* Fill in RuntimeInfo with actual addresses */
    RuntimeInfo *rti = g_native->rti;
    rti->done_entry = code_base + g_native->eo.done;
    rti->S_entry = code_base + g_native->eo.S;
    rti->K_entry = code_base + g_native->eo.K;
    rti->I_entry = code_base + g_native->eo.I;
    rti->S1_entry = code_base + g_native->eo.S1;
    rti->S2_entry = code_base + g_native->eo.S2;
    rti->K1_entry = code_base + g_native->eo.K1;
    rti->AP_entry = code_base + g_native->eo.AP;
    rti->IND_entry = code_base + g_native->eo.IND;
    rti->arg_stack_top = (u64)(g_native->arg_stack + NATIVE_STACK_SIZE);
    rti->upd_stack_top = (u64)(g_native->update_stack + UPDATE_STACK_SIZE);
    rti->heap_ptr_save = 0;
    
    /* Fill in continuation entries for CPS */
    rti->cont_done_entry = code_base + g_native->eo.cont_done;
    rti->cont_app_entry = code_base + g_native->eo.cont_app;
    rti->cont_s1_entry = code_base + g_native->eo.cont_s1;
    rti->cont_s2x_entry = code_base + g_native->eo.cont_s2x;
    rti->cont_s2y_entry = code_base + g_native->eo.cont_s2y;
    rti->cont_k1_entry = code_base + g_native->eo.cont_k1;
    
    /* Fill in GC fields */
    rti->space0 = (u64)g_native->heap[0];
    rti->space1 = (u64)g_native->heap[1];
    rti->space_size = g_initial_heap_size;
    rti->heap_end = (u64)(g_native->heap[0] + g_initial_heap_size);
    rti->gc_entry = code_base + g_native->eo.gc_entry;
    
    /* Pre-build primitive closures in SEPARATE buffer (not in GC heap) */
    u8 *prim = g_native->primitives;
    
    /* cont_done closure - just 8 bytes with entry pointer */
    u8 *cont_done_closure = prim;
    *(u64*)prim = rti->cont_done_entry;
    prim += 8;
    
    g_native->S_closure = prim;
    *(u64*)prim = rti->S_entry;
    prim += 8;
    rti->S_closure = (u64)g_native->S_closure;
    
    g_native->K_closure = prim;
    *(u64*)prim = rti->K_entry;
    prim += 8;
    rti->K_closure = (u64)g_native->K_closure;
    
    g_native->I_closure = prim;
    *(u64*)prim = rti->I_entry;
    prim += 8;
    rti->I_closure = (u64)g_native->I_closure;
    
    /* heap_base starts at the beginning of heap[0] - all user data */
    g_native->heap_base = g_native->heap[0];
    
    /* RTI_CONT_DONE holds the cont_done CLOSURE address (for trampoline).
     * RTI_CONT_APP, RTI_CONT_S1, etc. hold ENTRY addresses (for building continuations).
     * The cont_done_entry field in struct is for the entry address.
     * We need to store the closure address at offset RTI_CONT_DONE.
     */
    rti->cont_done_entry = (u64)cont_done_closure;  /* Reuse field for closure addr */
}

/*
 * Convert SKITerm to native closure
 */
static void *native_term_to_closure(SKITerm *t, u8 **hp) {
    RuntimeInfo *rti = g_native->rti;
    switch (t->tag) {
    case TERM_S: return g_native->S_closure;
    case TERM_K: return g_native->K_closure;
    case TERM_I: return g_native->I_closure;
    case TERM_APP: {
        void *f = native_term_to_closure(t->app.left, hp);
        void *arg = native_term_to_closure(t->app.right, hp);
        u8 *closure = *hp;
        *(u64*)(closure + 0) = rti->AP_entry;
        *(void**)(closure + 8) = f;
        *(void**)(closure + 16) = arg;
        *hp += 24;
        return closure;
    }
    }
    return NULL;
}

/*
 * Convert native closure back to SKITerm
 */
static SKITerm *native_closure_to_term(SKIPool *pool, void *closure) {
    RuntimeInfo *rti = g_native->rti;
    u64 entry = *(u64*)closure;
    
    if (entry == rti->S_entry) return ski_s(pool);
    if (entry == rti->K_entry) return ski_k(pool);
    if (entry == rti->I_entry) return ski_i(pool);
    
    if (entry == rti->S1_entry) {
        void *x = *(void**)((u8*)closure + 8);
        return ski_app(pool, ski_s(pool), native_closure_to_term(pool, x));
    }
    if (entry == rti->S2_entry) {
        void *x = *(void**)((u8*)closure + 8);
        void *y = *(void**)((u8*)closure + 16);
        return ski_app(pool, 
            ski_app(pool, ski_s(pool), native_closure_to_term(pool, x)),
            native_closure_to_term(pool, y));
    }
    if (entry == rti->K1_entry) {
        void *x = *(void**)((u8*)closure + 8);
        return ski_app(pool, ski_k(pool), native_closure_to_term(pool, x));
    }
    if (entry == rti->AP_entry) {
        void *f = *(void**)((u8*)closure + 8);
        void *arg = *(void**)((u8*)closure + 16);
        return ski_app(pool, 
            native_closure_to_term(pool, f),
            native_closure_to_term(pool, arg));
    }
    if (entry == rti->IND_entry) {
        void *target = *(void**)((u8*)closure + 8);
        return native_closure_to_term(pool, target);
    }
    
    return ski_i(pool);
}

/*
 * Run native trampoline
 * trampoline(closure, heap, runtime_info) -> result
 */
static void *run_native_code(void *closure, u8 **hp) {
    typedef void *(*Trampoline)(void*, u8*, RuntimeInfo*);
    u64 code_base = (u64)g_native->code;
    Trampoline fn = (Trampoline)(code_base + g_native->eo.trampoline);
    void *result = fn(closure, *hp, g_native->rti);
    *hp = (u8*)g_native->rti->heap_ptr_save;  /* Get updated heap pointer */
    return result;
}

/*
 * Normalize to HNF by recursively reducing
 */
static void *native_normalize(void *closure, u8 **hp);

static void *native_normalize(void *closure, u8 **hp) {
    RuntimeInfo *rti = g_native->rti;
    void *whnf = run_native_code(closure, hp);
    u64 entry = *(u64*)whnf;
    
    if (entry == rti->S_entry || 
        entry == rti->K_entry || 
        entry == rti->I_entry) {
        return whnf;
    }
    
    if (entry == rti->S1_entry) {
        void *x = *(void**)((u8*)whnf + 8);
        void *x_nf = native_normalize(x, hp);
        if (x_nf == x) return whnf;
        u8 *new_c = *hp;
        *(u64*)(new_c + 0) = rti->S1_entry;
        *(void**)(new_c + 8) = x_nf;
        *hp += 16;
        return new_c;
    }
    
    if (entry == rti->S2_entry) {
        void *x = *(void**)((u8*)whnf + 8);
        void *y = *(void**)((u8*)whnf + 16);
        void *x_nf = native_normalize(x, hp);
        void *y_nf = native_normalize(y, hp);
        if (x_nf == x && y_nf == y) return whnf;
        u8 *new_c = *hp;
        *(u64*)(new_c + 0) = rti->S2_entry;
        *(void**)(new_c + 8) = x_nf;
        *(void**)(new_c + 16) = y_nf;
        *hp += 24;
        return new_c;
    }
    
    if (entry == rti->K1_entry) {
        void *x = *(void**)((u8*)whnf + 8);
        void *x_nf = native_normalize(x, hp);
        if (x_nf == x) return whnf;
        u8 *new_c = *hp;
        *(u64*)(new_c + 0) = rti->K1_entry;
        *(void**)(new_c + 8) = x_nf;
        *hp += 16;
        return new_c;
    }
    
    if (entry == rti->AP_entry) {
        /* Shouldn't happen after WHNF, but handle it */
        void *f = *(void**)((u8*)whnf + 8);
        void *arg = *(void**)((u8*)whnf + 16);
        void *f_nf = native_normalize(f, hp);
        void *arg_nf = native_normalize(arg, hp);
        if (f_nf == f && arg_nf == arg) return whnf;
        u8 *new_c = *hp;
        *(u64*)(new_c + 0) = rti->AP_entry;
        *(void**)(new_c + 8) = f_nf;
        *(void**)(new_c + 16) = arg_nf;
        *hp += 24;
        return new_c;
    }
    
    return whnf;
}

/*
 * Public API for native reduction
 */
SKITerm *jit_reduce_native(SKIPool *pool, SKITerm *term, i64 *steps) {
    build_native_runtime();
    
    /* Reset GC count before run */
    g_native->rti->gc_count = 0;
    
    u8 *hp = g_native->heap_base;
    
    void *closure = native_term_to_closure(term, &hp);
    void *result = native_normalize(closure, &hp);
    
    /* Print GC stats if debug enabled */
    if (g_gc_debug && g_native->rti->gc_count > 0) {
        fprintf(stderr, "[GC] Total collections: %lu\n", g_native->rti->gc_count);
    }
    
    SKITerm *result_term = native_closure_to_term(pool, result);
    
    *steps = 0;
    return result_term;
}

/* ========================================================================
 * ELF EMISSION
 * 
 * Produces a standalone x86-64 PIE executable that:
 * 1. Allocates heap via mmap syscall
 * 2. Fills RuntimeInfo with computed addresses
 * 3. Patches S/K/I closure entry pointers
 * 4. Runs the CPS normalizer (shared with native runtime)
 * 5. Outputs BCL to stdout and exits
 * ======================================================================== */

/* ELF64 structures */
#define ELF_EHDR_SIZE   64
#define ELF_PHDR_SIZE   56

/* ELF header fields */
#define ET_DYN          3       /* PIE executable */
#define EM_X86_64       0x3e
#define EV_CURRENT      1

/* Program header types */
#define PT_LOAD         1

/* Program header flags */
#define PF_X            1
#define PF_W            2
#define PF_R            4

/* Syscall numbers */
#define SYS_write       1
#define SYS_mmap        9
#define SYS_exit        60

/* Memory sizes for ELF runtime */
#define ELF_HEAP_SIZE       (64 * 1024 * 1024)
#define ELF_ARG_STACK_SIZE  (256 * 1024)
#define ELF_UPD_STACK_SIZE  (64 * 1024)
#define ELF_OUTPUT_BUF_SIZE (1 * 1024 * 1024)

/*
 * ELF emission buffer
 */
typedef struct {
    u8 *buf;
    u64 capacity;
    u64 text_start;     /* file offset of .text */
    u64 text_size;      /* size of .text */
    u64 data_start;     /* file offset of .data */
    u64 data_size;      /* size of .data */
    
    /* Code offsets within .text (relative to text_start) */
    u32 start_off;
    u32 done_bcl_off;   /* ELF-specific cont_done entry: BCL output + exit */
    u32 bcl_emit_off;
    EntryOffsets eo;
    
    /* Data offsets within .data (relative to data_start) */
    u32 rti_off;        /* RuntimeInfo */
    u32 S_closure_off;
    u32 K_closure_off;
    u32 I_closure_off;
    u32 cont_done_closure_off;  /* cont_done closure for CPS (points to done_bcl_off) */
    u32 term_off;       /* start of term closures */
    u32 term_size;      /* size of term closures */
} ElfBuf;

/*
 * Emit ELF64 header
 */
static void emit_elf_header(u8 *buf, u64 entry_off, u64 phoff, u16 phnum) {
    memset(buf, 0, ELF_EHDR_SIZE);
    
    /* e_ident */
    buf[0] = 0x7f; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;    /* ELFCLASS64 */
    buf[5] = 1;    /* ELFDATA2LSB */
    buf[6] = 1;    /* EV_CURRENT */
    buf[7] = 0;    /* ELFOSABI_NONE */
    
    *(u16*)(buf + 16) = ET_DYN;         /* e_type: PIE */
    *(u16*)(buf + 18) = EM_X86_64;      /* e_machine */
    *(u32*)(buf + 20) = EV_CURRENT;     /* e_version */
    *(u64*)(buf + 24) = entry_off;      /* e_entry: offset from load base */
    *(u64*)(buf + 32) = phoff;          /* e_phoff */
    *(u64*)(buf + 40) = 0;              /* e_shoff: no sections */
    *(u32*)(buf + 48) = 0;              /* e_flags */
    *(u16*)(buf + 52) = ELF_EHDR_SIZE;  /* e_ehsize */
    *(u16*)(buf + 54) = ELF_PHDR_SIZE;  /* e_phentsize */
    *(u16*)(buf + 56) = phnum;          /* e_phnum */
    *(u16*)(buf + 58) = 0;              /* e_shentsize */
    *(u16*)(buf + 60) = 0;              /* e_shnum */
    *(u16*)(buf + 62) = 0;              /* e_shstrndx */
}

/*
 * Emit program header
 */
static void emit_phdr(u8 *buf, u32 type, u32 flags,
                      u64 offset, u64 vaddr, u64 filesz, u64 memsz, u64 align) {
    *(u32*)(buf + 0)  = type;
    *(u32*)(buf + 4)  = flags;
    *(u64*)(buf + 8)  = offset;
    *(u64*)(buf + 16) = vaddr;
    *(u64*)(buf + 24) = vaddr;      /* p_paddr = p_vaddr */
    *(u64*)(buf + 32) = filesz;
    *(u64*)(buf + 40) = memsz;
    *(u64*)(buf + 48) = align;
}

/*
 * Emit: mov r64, imm64 (10-byte instruction)
 */
static void emit_mov_r64_imm64(Code *c, int reg, u64 imm) {
    if (reg >= 8) {
        emit(c, 0x49);
        reg -= 8;
    } else {
        emit(c, 0x48);
    }
    emit(c, 0xb8 + reg);
    emit(c, imm & 0xff);
    emit(c, (imm >> 8) & 0xff);
    emit(c, (imm >> 16) & 0xff);
    emit(c, (imm >> 24) & 0xff);
    emit(c, (imm >> 32) & 0xff);
    emit(c, (imm >> 40) & 0xff);
    emit(c, (imm >> 48) & 0xff);
    emit(c, (imm >> 56) & 0xff);
}

/*
 * Emit: lea reg, [rip + disp32]
 * Returns the offset where disp32 is stored (for patching)
 */
static u32 emit_lea_rip_rel(Code *c, int reg) {
    u8 rex = 0x48;
    if (reg >= 8) { rex |= 0x04; reg -= 8; }
    emit(c, rex);
    emit(c, 0x8d);
    emit(c, 0x05 | (reg << 3));  /* ModRM: [rip+disp32] */
    u32 patch_off = c->pos;
    emit32(c, 0);  /* placeholder disp32 */
    return patch_off;
}

/*
 * Emit: syscall
 */
static void emit_syscall(Code *c) {
    emit(c, 0x0f);
    emit(c, 0x05);
}

/*
 * Emit: mov [reg + off], src_reg (64-bit)
 */
static void emit_mov_mem_reg(Code *c, int base_reg, i32 off, int src_reg) {
    u8 rex = 0x48;
    if (base_reg >= 8) { rex |= 0x01; base_reg -= 8; }
    if (src_reg >= 8) { rex |= 0x04; src_reg -= 8; }
    emit(c, rex);
    emit(c, 0x89);
    
    if (off == 0 && base_reg != 5) {  /* rbp/r13 needs disp8 */
        emit(c, (src_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);  /* SIB for rsp/r12 */
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x40 | (src_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);
        emit(c, (u8)off);
    } else {
        emit(c, 0x80 | (src_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);
        emit32(c, off);
    }
}

/*
 * Emit: mov dst_reg, [reg + off] (64-bit)
 */
static void emit_mov_reg_mem(Code *c, int dst_reg, int base_reg, i32 off) {
    u8 rex = 0x48;
    if (base_reg >= 8) { rex |= 0x01; base_reg -= 8; }
    if (dst_reg >= 8) { rex |= 0x04; dst_reg -= 8; }
    emit(c, rex);
    emit(c, 0x8b);
    
    if (off == 0 && base_reg != 5) {
        emit(c, (dst_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);
    } else if (off >= -128 && off <= 127) {
        emit(c, 0x40 | (dst_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);
        emit(c, (u8)off);
    } else {
        emit(c, 0x80 | (dst_reg << 3) | base_reg);
        if (base_reg == 4) emit(c, 0x24);
        emit32(c, off);
    }
}

/*
 * Emit: add reg, imm32
 */
static void emit_add_reg_imm32(Code *c, int reg, i32 imm) {
    u8 rex = 0x48;
    if (reg >= 8) { rex |= 0x01; reg -= 8; }
    emit(c, rex);
    if (imm >= -128 && imm <= 127) {
        emit(c, 0x83);
        emit(c, 0xc0 | reg);
        emit(c, (u8)imm);
    } else {
        emit(c, 0x81);
        emit(c, 0xc0 | reg);
        emit32(c, imm);
    }
}

/*
 * Emit: xor reg, reg (zero register)
 */
static void emit_xor_reg_reg(Code *c, int reg) {
    u8 rex = 0x48;
    int r = reg;
    if (reg >= 8) { rex |= 0x05; r -= 8; }
    emit(c, rex);
    emit(c, 0x31);
    emit(c, 0xc0 | (r << 3) | r);
}

/*
 * Emit: cmp reg, [r8 + off]
 */
static void emit_cmp_reg_r8_off(Code *c, int reg, i32 off) {
    u8 rex = 0x49;
    if (reg >= 8) { rex |= 0x04; reg -= 8; }
    emit(c, rex);
    emit(c, 0x3b);
    if (off >= -128 && off <= 127) {
        emit(c, 0x40 | (reg << 3));
        emit(c, (u8)off);
    } else {
        emit(c, 0x80 | (reg << 3));
        emit32(c, off);
    }
}

/*
 * Emit: je rel32
 * Returns patch offset
 */
static u32 emit_je_rel32(Code *c) {
    emit(c, 0x0f);
    emit(c, 0x84);
    u32 patch = c->pos;
    emit32(c, 0);
    return patch;
}

/*
 * Emit: jmp rel32
 * Returns patch offset
 */
static u32 emit_jmp_rel32(Code *c) {
    emit(c, 0xe9);
    u32 patch = c->pos;
    emit32(c, 0);
    return patch;
}

/*
 * Emit: call rel32
 * Returns patch offset
 */
static u32 emit_call_rel32(Code *c) {
    emit(c, 0xe8);
    u32 patch = c->pos;
    emit32(c, 0);
    return patch;
}

/*
 * Emit: mov byte [reg], imm8
 */
static void emit_mov_byte_mem_imm8(Code *c, int reg, u8 val) {
    if (reg >= 8) {
        emit(c, 0x41);
        reg -= 8;
    }
    emit(c, 0xc6);
    emit(c, reg);
    emit(c, val);
}

/*
 * Emit: inc reg (64-bit)
 */
static void emit_inc_reg(Code *c, int reg) {
    u8 rex = 0x48;
    if (reg >= 8) { rex |= 0x01; reg -= 8; }
    emit(c, rex);
    emit(c, 0xff);
    emit(c, 0xc0 | reg);
}

/*
 * Emit: mov reg1, reg2 (64-bit)
 */
static void emit_mov_reg_reg(Code *c, int dst, int src) {
    u8 rex = 0x48;
    if (dst >= 8) { rex |= 0x01; dst -= 8; }
    if (src >= 8) { rex |= 0x04; src -= 8; }
    emit(c, rex);
    emit(c, 0x89);
    emit(c, 0xc0 | (src << 3) | dst);
}

/*
 * Emit _start: PIE entry point
 * 
 * 1. Compute base address via lea
 * 2. Set r8 = RuntimeInfo address
 * 3. mmap heap, arg stack, update stack
 * 4. Fill RuntimeInfo with entry addresses
 * 5. Patch S/K/I closure entry pointers
 * 6. Set machine registers and enter reduction
 */
static void emit_elf_start(Code *c, ElfBuf *eb) {
    eb->start_off = c->pos;
    
    /*
     * Compute base address using lea rax, [rip + known_offset]
     * We emit this at a known offset, so we can calculate the displacement
     * to the start of the file (which is where the kernel loads us)
     */
    
    /* 
     * lea r8, [rip + disp]  -> r8 = &RuntimeInfo
     * The displacement is: (data_vaddr + rti_off) - (rip after this instruction)
     * Since PIE, data_vaddr = file_offset at runtime
     * We'll patch this after we know the layout
     */
    u32 r8_rti_patch = emit_lea_rip_rel(c, 8);
    
    /*
     * lea rax, [rip + disp] -> rax = code base (start of .text)
     * We'll use this to compute entry addresses
     */
    u32 code_base_patch = emit_lea_rip_rel(c, 0);
    emit_push(c, 0);  /* save code base on stack */
    
    /*
     * mmap(NULL, ELF_HEAP_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0)
     * rdi=addr, rsi=length, rdx=prot, r10=flags, r8=fd, r9=offset
     * But r8 has RuntimeInfo, so we need to save/restore
     */
    emit_push(c, 8);  /* save r8 (RuntimeInfo ptr) */
    
    /* Heap allocation */
    emit_xor_reg_reg(c, 7);                     /* rdi = 0 (NULL) */
    emit_mov_r64_imm64(c, 6, ELF_HEAP_SIZE);    /* rsi = size */
    emit_mov_r64_imm64(c, 2, 3);                /* rdx = PROT_READ|PROT_WRITE */
    emit_mov_r64_imm64(c, 10, 0x22);            /* r10 = MAP_PRIVATE|MAP_ANONYMOUS */
    emit_mov_r64_imm64(c, 8, (u64)-1);          /* r8 = -1 (fd) */
    emit_xor_reg_reg(c, 9);                     /* r9 = 0 (offset) */
    emit_mov_r64_imm64(c, 0, SYS_mmap);         /* rax = syscall number */
    emit_syscall(c);
    emit_mov_reg_reg(c, 12, 0);                 /* r12 = heap pointer */
    
    /* Arg stack allocation */
    emit_xor_reg_reg(c, 7);
    emit_mov_r64_imm64(c, 6, ELF_ARG_STACK_SIZE);
    emit_mov_r64_imm64(c, 2, 3);
    emit_mov_r64_imm64(c, 10, 0x22);
    emit_mov_r64_imm64(c, 8, (u64)-1);
    emit_xor_reg_reg(c, 9);
    emit_mov_r64_imm64(c, 0, SYS_mmap);
    emit_syscall(c);
    /* r14 = top of arg stack (stack grows down) */
    emit_add_reg_imm32(c, 0, ELF_ARG_STACK_SIZE);
    emit_mov_reg_reg(c, 14, 0);
    emit_mov_reg_reg(c, 13, 0);  /* r13 = stack base (for underflow check) */
    
    /* Update stack allocation */
    emit_xor_reg_reg(c, 7);
    emit_mov_r64_imm64(c, 6, ELF_UPD_STACK_SIZE);
    emit_mov_r64_imm64(c, 2, 3);
    emit_mov_r64_imm64(c, 10, 0x22);
    emit_mov_r64_imm64(c, 8, (u64)-1);
    emit_xor_reg_reg(c, 9);
    emit_mov_r64_imm64(c, 0, SYS_mmap);
    emit_syscall(c);
    emit_add_reg_imm32(c, 0, ELF_UPD_STACK_SIZE);
    emit_mov_reg_reg(c, 15, 0);  /* r15 = update stack top */
    
    /* Output buffer allocation */
    emit_xor_reg_reg(c, 7);
    emit_mov_r64_imm64(c, 6, ELF_OUTPUT_BUF_SIZE);
    emit_mov_r64_imm64(c, 2, 3);
    emit_mov_r64_imm64(c, 10, 0x22);
    emit_mov_r64_imm64(c, 8, (u64)-1);
    emit_xor_reg_reg(c, 9);
    emit_mov_r64_imm64(c, 0, SYS_mmap);
    emit_syscall(c);
    emit_push(c, 0);  /* save output buffer address */
    
    emit_pop(c, 8);   /* restore r8 = RuntimeInfo ptr... wait, this pops output_buf! */
    
    /* Fix: we need to manage the stack properly */
    /* Stack state: [output_buf] [r8_saved] [code_base] */
    /* Let's reorganize */
    
    /* Actually, let's save output_buf to RuntimeInfo later. Pop r8 from correct position */
    /* Current stack after output buf push: [out_buf, r8_saved, code_base] */
    emit_mov_reg_mem(c, 1, 4, 8);   /* rcx = r8_saved from stack */
    emit_mov_reg_reg(c, 8, 1);      /* r8 = RuntimeInfo */
    
    /* Store output buffer address in a temp location - use r9 */
    emit_pop(c, 9);                 /* r9 = output buffer */
    emit_add_reg_imm32(c, 4, 8);    /* pop the r8_saved slot */
    emit_pop(c, 1);                 /* rcx = code_base */
    
    /*
     * Fill RuntimeInfo with entry addresses
     * r8 = RuntimeInfo, rcx = code_base
     */
    
    /* done_entry = code_base + done_bcl_off */
    emit_mov_reg_reg(c, 0, 1);              /* rax = code_base */
    /* We need to add the offset - but we don't know it yet at emit time! */
    /* Solution: emit placeholder add instructions, patch later */
    
    /* Actually, we can compute: entry_addr = code_base + known_offset */
    /* The offsets are known after we emit all code */
    /* For now, emit mov rax, rcx; add rax, imm32; mov [r8+off], rax pattern */
    
    /* Helper: emit "mov [r8 + rti_off], rcx + entry_off" */
    /* We'll emit: lea rax, [rcx + entry_off]; mov [r8 + rti_off], rax */
    /* But lea can't do reg + imm32 directly. Use: mov rax, rcx; add rax, imm32 */
    
    /* Store patch locations for entry offsets */
    u32 patch_done, patch_S, patch_K, patch_I, patch_S1, patch_S2, patch_K1, patch_AP, patch_IND;
    
    /* done_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);  /* add rax, imm32 */
    patch_done = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_DONE, 0);
    
    /* S_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_S = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_S, 0);
    
    /* K_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_K = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_K, 0);
    
    /* I_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_I = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_I, 0);
    
    /* S1_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_S1 = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_S1, 0);
    
    /* S2_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_S2 = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_S2, 0);
    
    /* K1_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_K1 = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_K1, 0);
    
    /* AP_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_AP = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_AP, 0);
    
    /* IND_entry */
    emit_mov_reg_reg(c, 0, 1);
    emit(c, 0x48); emit(c, 0x05);
    patch_IND = c->pos;
    emit32(c, 0);
    emit_mov_mem_reg(c, 8, RTI_IND, 0);
    
    /* Store stack pointers in RuntimeInfo */
    emit_mov_mem_reg(c, 8, RTI_ARG_STACK, 14);
    emit_mov_mem_reg(c, 8, RTI_UPD_STACK, 15);
    
    /*
     * Patch S/K/I closures with entry pointers
     * The closures are in .data, we need their runtime addresses
     */
    
    /* lea rax, [rip + S_closure_off] */
    u32 S_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_S_CLOSURE, 0);  /* store in RuntimeInfo */
    /* Patch the closure's entry pointer */
    emit_mov_reg_mem(c, 2, 8, RTI_S);          /* rdx = S_entry */
    emit_mov_mem_reg(c, 0, 0, 2);              /* [S_closure] = S_entry */
    
    /* K closure */
    u32 K_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_K_CLOSURE, 0);
    emit_mov_reg_mem(c, 2, 8, RTI_K);
    emit_mov_mem_reg(c, 0, 0, 2);
    
    /* I closure */
    u32 I_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_I_CLOSURE, 0);
    emit_mov_reg_mem(c, 2, 8, RTI_I);
    emit_mov_mem_reg(c, 0, 0, 2);
    
    /*
     * Patch term closures that reference S/K/I
     * For now, we handle this differently: term closures store offsets to S/K/I,
     * and we patch them here to absolute addresses.
     * But actually, term closures for S/K/I should just point to the singleton closures.
     * Apps point to AP_entry, which needs patching.
     * 
     * Simpler approach: Walk the term closure data and patch all entry pointers.
     * Term closures are laid out sequentially in .data after I_closure.
     * Each closure starts with an entry offset that needs: entry_off -> code_base + entry_off
     */
    
    /* We'll handle term closure patching in the serialization phase */
    /* For now, save output buffer address to a known location */
    /* Use: push r9 (output buf) so done_bcl can find it */
    emit_push(c, 9);
    
    /*
     * Set rbx = root closure address
     * lea rbx, [rip + root_closure_off]
     */
    u32 root_patch = emit_lea_rip_rel(c, 3);
    
    /* Enter reduction: jmp [rbx] */
    emit_enter_closure(c);
    
    /* Store patch locations in ElfBuf for later patching */
    /* We'll need to return these somehow... use a struct */
    /* For now, store in a static - this is getting complex */
    
    /* Actually, let's store patch offsets in the ElfBuf */
    /* This requires restructuring. Let me do this properly. */
    
    /* The issue is: we need to patch after emitting all code, but we need the patch offsets now */
    /* Solution: Return the patch offsets from this function */
}

/*
 * CPS-based BCL output for ELF
 *
 * After normalization completes, rbx holds the normal form.
 * We need to emit BCL representation and exit.
 *
 * CPS design for BCL emission:
 *   bcl_emit_entry(rbx=closure, rcx=continuation, rdi=out_ptr)
 *     - Dispatches based on closure type
 *     - For atoms (S, K, I): emit chars, jump to [rcx] with updated rdi
 *     - For App/partials: emit prefix, create continuation, recurse
 *
 * Continuation layouts for BCL:
 *   cont_bcl_done: [entry]                    - final exit (flush and exit)
 *   cont_bcl_arg:  [entry, arg, outer_k]      - after emitting f, emit arg
 *   cont_bcl_s2y:  [entry, y, outer_k]        - after emitting S2.x, emit y
 *
 * Register usage:
 *   rbx = current closure to emit
 *   rcx = current continuation
 *   rdi = output buffer write position
 *   r8  = RuntimeInfo
 *   r12 = heap pointer
 *
 * Flow:
 *   1. cont_done_bcl_entry: called by normalizer when done
 *      - allocates output buffer
 *      - sets up rdi, creates cont_bcl_done, calls bcl_emit_entry
 *   2. bcl_emit_entry: CPS BCL serializer
 *   3. cont_bcl_done_entry: flush buffer, write newline, exit(0)
 */

/* Forward declare offsets we'll need */
typedef struct {
    u32 cont_done_bcl;      /* entry called when normalization done */
    u32 bcl_emit;           /* CPS BCL emitter entry */
    u32 cont_bcl_done;      /* final BCL continuation - flush & exit */
    u32 cont_bcl_arg;       /* after emitting f, emit arg */
} BclCpsOffsets;

static void emit_elf_bcl_cps(Code *c, ElfBuf *eb, BclCpsOffsets *bo) {
    
    /* ================================================================
     * cont_done_bcl_entry: Normalization is complete
     * rbx = normalized closure
     * 
     * Set up BCL emission: mmap output buffer, create cont_bcl_done, 
     * call bcl_emit_entry
     * ================================================================ */
    bo->cont_done_bcl = c->pos;
    eb->done_bcl_off = c->pos;
    
    /* mmap output buffer */
    emit_push(c, 3);                            /* save rbx */
    emit_push(c, 8);                            /* save r8 */
    emit_xor_reg_reg(c, 7);                     /* rdi = 0 */
    emit_mov_r64_imm64(c, 6, ELF_OUTPUT_BUF_SIZE);
    emit_mov_r64_imm64(c, 2, 3);                /* PROT_READ|PROT_WRITE */
    emit_mov_r64_imm64(c, 10, 0x22);            /* MAP_PRIVATE|MAP_ANONYMOUS */
    emit_mov_r64_imm64(c, 8, (u64)-1);          /* fd = -1 */
    emit_xor_reg_reg(c, 9);                     /* offset = 0 */
    emit_mov_r64_imm64(c, 0, SYS_mmap);
    emit_syscall(c);
    emit_pop(c, 8);                             /* restore r8 */
    emit_pop(c, 3);                             /* restore rbx */
    
    /* rax = buffer start, save it in RTI for flush */
    emit_mov_mem_reg(c, 8, RTI_OUTPUT_BUF, 0);
    emit_mov_reg_reg(c, 7, 0);                  /* rdi = output position */
    
    /* Create cont_bcl_done closure on heap: [entry] */
    /* Need to load entry from RTI_BCL_DONE */
    emit_mov_reg_r8_off(c, 0, RTI_BCL_DONE);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);                  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 8);     /* add r12, 8 */
    
    /* Jump to bcl_emit_entry */
    emit_mov_reg_r8_off(c, 0, RTI_BCL_EMIT);
    emit(c, 0xff); emit(c, 0xe0);  /* jmp rax */
    
    /* ================================================================
     * bcl_emit_entry: CPS BCL emitter
     * rbx = closure to emit
     * rcx = continuation (jumped to with updated rdi)
     * rdi = output write position
     * ================================================================ */
    bo->bcl_emit = c->pos;
    eb->bcl_emit_off = c->pos;
    
    /* rax = [rbx] (entry pointer) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x03);  /* mov rax, [rbx] */
    
    /* Check if K */
    emit_cmp_reg_r8_off(c, 0, RTI_K);
    u32 jne_not_K = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    /* K: emit "00", jump to continuation */
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* not_K: */
    *(i32*)(c->buf + jne_not_K + 2) = (i32)(c->pos - jne_not_K - 6);
    
    /* Check if S */
    emit_cmp_reg_r8_off(c, 0, RTI_S);
    u32 jne_not_S = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    /* S: emit "01", jump to continuation */
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* not_S: */
    *(i32*)(c->buf + jne_not_S + 2) = (i32)(c->pos - jne_not_S - 6);
    
    /* Check if I */
    emit_cmp_reg_r8_off(c, 0, RTI_I);
    u32 jne_not_I = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    /* I = SKK: emit "11010000" */
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit(c, 0xff); emit(c, 0x21);  /* jmp [rcx] */
    
    /* not_I: */
    *(i32*)(c->buf + jne_not_I + 2) = (i32)(c->pos - jne_not_I - 6);
    
    /* Check if K1: emit "100" + emit(x) */
    emit_cmp_reg_r8_off(c, 0, RTI_K1);
    u32 jne_not_K1 = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    /* rbx = x = [rbx + 8], tail call to bcl_emit */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] */
    emit(c, 0xe9); emit32(c, (i32)(bo->bcl_emit - (c->pos + 4)));  /* jmp bcl_emit */
    
    /* not_K1: */
    *(i32*)(c->buf + jne_not_K1 + 2) = (i32)(c->pos - jne_not_K1 - 6);
    
    /* Check if S1: emit "101" + emit(x) */
    emit_cmp_reg_r8_off(c, 0, RTI_S1);
    u32 jne_not_S1 = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] */
    emit(c, 0xe9); emit32(c, (i32)(bo->bcl_emit - (c->pos + 4)));
    
    /* not_S1: */
    *(i32*)(c->buf + jne_not_S1 + 2) = (i32)(c->pos - jne_not_S1 - 6);
    
    /* Check if S2: emit "1101" + emit(x) + emit(y) */
    emit_cmp_reg_r8_off(c, 0, RTI_S2);
    u32 jne_not_S2 = c->pos;
    emit(c, 0x0f); emit(c, 0x85); emit32(c, 0);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '0');
    emit_inc_reg(c, 7);
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    /* Build cont_bcl_arg for y: [entry, y, outer_k] */
    emit_mov_reg_r8_off(c, 0, RTI_BCL_ARG);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    /* y = [rbx + 16] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);  /* mov rax, [rbx+16] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx */
    /* rcx = new cont, rbx = x, recurse */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] - x */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);                  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);    /* add r12, 24 */
    emit(c, 0xe9); emit32(c, (i32)(bo->bcl_emit - (c->pos + 4)));
    
    /* not_S2: */
    *(i32*)(c->buf + jne_not_S2 + 2) = (i32)(c->pos - jne_not_S2 - 6);
    
    /* Must be AP: emit "1" + emit(f) + emit(arg) */
    emit_mov_byte_mem_imm8(c, 7, '1');
    emit_inc_reg(c, 7);
    /* Build cont_bcl_arg: [entry, arg, outer_k] */
    emit_mov_reg_r8_off(c, 0, RTI_BCL_ARG);
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x04); emit(c, 0x24);  /* mov [r12], rax */
    /* arg = [rbx + 16] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x43); emit(c, 0x10);  /* mov rax, [rbx+16] */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x44); emit(c, 0x24); emit(c, 0x08);  /* mov [r12+8], rax */
    emit(c, 0x49); emit(c, 0x89); emit(c, 0x4c); emit(c, 0x24); emit(c, 0x10);  /* mov [r12+16], rcx */
    /* rbx = f, rcx = new cont */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x5b); emit(c, 0x08);  /* mov rbx, [rbx+8] - f */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0xe1);                  /* mov rcx, r12 */
    emit(c, 0x49); emit(c, 0x83); emit(c, 0xc4); emit(c, 24);    /* add r12, 24 */
    emit(c, 0xe9); emit32(c, (i32)(bo->bcl_emit - (c->pos + 4)));
    
    /* ================================================================
     * cont_bcl_arg_entry: after emitting f, now emit arg
     * rcx = [entry, arg, outer_k]
     * rdi = updated output position
     * ================================================================ */
    bo->cont_bcl_arg = c->pos;
    /* rbx = arg = [rcx + 8] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x59); emit(c, 0x08);  /* mov rbx, [rcx+8] */
    /* new rcx = outer_k = [rcx + 16] */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x49); emit(c, 0x10);  /* mov rcx, [rcx+16] */
    /* tail call to bcl_emit */
    emit(c, 0xe9); emit32(c, (i32)(bo->bcl_emit - (c->pos + 4)));
    
    /* ================================================================
     * cont_bcl_done_entry: all BCL emitted, flush and exit
     * rdi = final output position
     * ================================================================ */
    bo->cont_bcl_done = c->pos;
    
    /* Calculate length: rdi - buffer_start */
    emit_mov_reg_r8_off(c, 6, RTI_OUTPUT_BUF);  /* rsi = buffer start */
    emit_mov_reg_reg(c, 2, 7);                   /* rdx = end pointer */
    emit(c, 0x48); emit(c, 0x29); emit(c, 0xf2); /* sub rdx, rsi -> rdx = length */
    
    /* write(1, buf, len) */
    emit_mov_r64_imm64(c, 7, 1);   /* rdi = 1 (stdout) */
    /* rsi = buffer (already set) */
    /* rdx = length (already set) */
    emit_mov_r64_imm64(c, 0, SYS_write);
    emit_syscall(c);
    
    /* Write newline */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xec); emit(c, 8);  /* sub rsp, 8 */
    emit(c, 0xc6); emit(c, 0x04); emit(c, 0x24); emit(c, '\n');  /* mov byte [rsp], '\n' */
    emit_mov_r64_imm64(c, 7, 1);   /* rdi = 1 */
    emit_mov_reg_reg(c, 6, 4);     /* rsi = rsp */
    emit_mov_r64_imm64(c, 2, 1);   /* rdx = 1 */
    emit_mov_r64_imm64(c, 0, SYS_write);
    emit_syscall(c);
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc4); emit(c, 8);  /* add rsp, 8 */
    
    /* exit(0) */
    emit_xor_reg_reg(c, 7);        /* rdi = 0 */
    emit_mov_r64_imm64(c, 0, SYS_exit);
    emit_syscall(c);
}

/*
 * Count closure nodes in term (for sizing data section)
 */
static u32 count_term_closures(SKITerm *t) {
    switch (t->tag) {
    case TERM_S:
    case TERM_K:
    case TERM_I:
        return 0;  /* Use singleton closures */
    case TERM_APP:
        return 1 + count_term_closures(t->app.left) + count_term_closures(t->app.right);
    }
    return 0;
}

/*
 * Serialize term to closure data
 * 
 * data: output buffer
 * pos: current position in data (updated)
 * t: term to serialize
 * S_off, K_off, I_off: offsets to singleton closures (relative to data section start)
 * 
 * Returns: offset of this closure (relative to data section start)
 * 
 * For S/K/I: returns offset to singleton
 * For App: allocates 24-byte closure [entry_marker | f_off | arg_off]
 *          entry_marker is a placeholder that _start will patch to AP_entry
 *          We use offset 0xFFFFFFFF as marker for "needs AP_entry patching"
 */
#define ENTRY_MARKER_AP  0xFFFFFFFFFFFFFFFFULL

static u32 serialize_term(u8 *data, u32 *pos, SKITerm *t, 
                          u32 S_off, u32 K_off, u32 I_off,
                          u32 data_base_off) {
    switch (t->tag) {
    case TERM_S:
        return S_off;
    case TERM_K:
        return K_off;
    case TERM_I:
        return I_off;
    case TERM_APP: {
        u32 f_off = serialize_term(data, pos, t->app.left, S_off, K_off, I_off, data_base_off);
        u32 arg_off = serialize_term(data, pos, t->app.right, S_off, K_off, I_off, data_base_off);
        u32 my_off = *pos;
        /* Store marker for AP_entry - will be patched by _start */
        *(u64*)(data + my_off) = ENTRY_MARKER_AP;
        /* Store offsets to f and arg (relative to data section start) */
        /* These will be converted to absolute addresses by _start */
        *(u64*)(data + my_off + 8) = data_base_off + f_off;
        *(u64*)(data + my_off + 16) = data_base_off + arg_off;
        *pos += 24;
        return my_off;
    }
    }
    return 0;
}

/*
 * Patch info for _start to fix up data section
 */
typedef struct {
    u32 r8_rti_patch;       /* lea r8, [rip+?] */
    u32 code_base_patch;    /* lea rax, [rip+?] */
    u32 patch_done;
    u32 patch_S;
    u32 patch_K;
    u32 patch_I;
    u32 patch_S1;
    u32 patch_S2;
    u32 patch_K1;
    u32 patch_AP;
    u32 patch_IND;
    /* CPS continuation entries */
    u32 patch_cont_app;
    u32 patch_cont_s1;
    u32 patch_cont_s2x;
    u32 patch_cont_s2y;
    u32 patch_cont_k1;
    u32 patch_norm_entry;
    /* BCL CPS entries */
    u32 patch_bcl_emit;
    u32 patch_bcl_arg;
    u32 patch_bcl_done;
    /* Closure patches */
    u32 S_closure_patch;
    u32 K_closure_patch;
    u32 I_closure_patch;
    u32 cont_done_closure_patch;  /* lea for cont_done closure */
    u32 root_patch;
    u32 term_start_patch;   /* lea for term closure start */
    u32 term_end_patch;     /* lea for term closure end */
    u32 data_base_patch;    /* lea for data section base */
    u32 file_data_off_patch; /* mov imm64 for file data offset */
} StartPatches;

/*
 * Re-emit _start with proper tracking of patch locations
 * 
 * CPS-based startup for unified backend:
 * - Allocates heap only (no arg/update stacks - CPS doesn't use them)
 * - Fills RuntimeInfo with entry addresses including CPS continuations
 * - Sets rbx = root closure, rcx = cont_done closure
 * - Jumps to norm_entry (the CPS normalizer)
 */
static StartPatches emit_elf_start_v2(Code *c, ElfBuf *eb) {
    StartPatches sp = {0};
    eb->start_off = c->pos;
    
    /* lea r8, [rip + disp] -> r8 = &RuntimeInfo */
    sp.r8_rti_patch = emit_lea_rip_rel(c, 8);
    
    /* lea rax, [rip + disp] -> rax = code base (start of .text) */
    sp.code_base_patch = emit_lea_rip_rel(c, 0);
    
    /* Save rax (code base) in r9 - we'll need it for computing addresses */
    emit_mov_reg_reg(c, 9, 0);  /* r9 = code base */
    
    /* mmap heap - this is the only allocation CPS needs */
    emit_xor_reg_reg(c, 7);                     /* rdi = 0 */
    emit_mov_r64_imm64(c, 6, ELF_HEAP_SIZE);    /* rsi = size */
    emit_mov_r64_imm64(c, 2, 3);                /* rdx = PROT_READ|PROT_WRITE */
    emit_mov_r64_imm64(c, 10, 0x22);            /* r10 = MAP_PRIVATE|MAP_ANONYMOUS */
    emit_push(c, 8);                            /* save r8 */
    emit_push(c, 9);                            /* save r9 (code base) */
    emit_mov_r64_imm64(c, 8, (u64)-1);          /* r8 = -1 (fd) */
    emit_xor_reg_reg(c, 9);                     /* r9 = 0 */
    emit_mov_r64_imm64(c, 0, SYS_mmap);
    emit_syscall(c);
    emit_pop(c, 9);                             /* restore r9 (code base) */
    emit_pop(c, 8);                             /* restore r8 */
    emit_mov_reg_reg(c, 12, 0);                 /* r12 = heap pointer */
    
    /* Fill RuntimeInfo with entry addresses */
    /* Pattern: mov rax, r9; add rax, imm32; mov [r8+off], rax */
    /* Using r9 as code base now */
    
#define EMIT_RTI_ENTRY(off, patch_var) \
    emit_mov_reg_reg(c, 0, 9); \
    emit(c, 0x48); emit(c, 0x05); \
    patch_var = c->pos; \
    emit32(c, 0); \
    emit_mov_mem_reg(c, 8, off, 0)
    
    EMIT_RTI_ENTRY(RTI_DONE, sp.patch_done);
    EMIT_RTI_ENTRY(RTI_S, sp.patch_S);
    EMIT_RTI_ENTRY(RTI_K, sp.patch_K);
    EMIT_RTI_ENTRY(RTI_I, sp.patch_I);
    EMIT_RTI_ENTRY(RTI_S1, sp.patch_S1);
    EMIT_RTI_ENTRY(RTI_S2, sp.patch_S2);
    EMIT_RTI_ENTRY(RTI_K1, sp.patch_K1);
    EMIT_RTI_ENTRY(RTI_AP, sp.patch_AP);
    EMIT_RTI_ENTRY(RTI_IND, sp.patch_IND);
    
    /* CPS continuation entries */
    EMIT_RTI_ENTRY(RTI_CONT_APP, sp.patch_cont_app);
    EMIT_RTI_ENTRY(RTI_CONT_S1, sp.patch_cont_s1);
    EMIT_RTI_ENTRY(RTI_CONT_S2X, sp.patch_cont_s2x);
    EMIT_RTI_ENTRY(RTI_CONT_S2Y, sp.patch_cont_s2y);
    EMIT_RTI_ENTRY(RTI_CONT_K1, sp.patch_cont_k1);
    
    /* BCL CPS entries */
    EMIT_RTI_ENTRY(RTI_BCL_EMIT, sp.patch_bcl_emit);
    EMIT_RTI_ENTRY(RTI_BCL_ARG, sp.patch_bcl_arg);
    EMIT_RTI_ENTRY(RTI_BCL_DONE, sp.patch_bcl_done);
    
#undef EMIT_RTI_ENTRY
    
    /* Patch S/K/I closures */
    /* lea rax, [rip + closure_off]; mov [r8+RTI_X_CLOSURE], rax; mov rdx, [r8+RTI_X]; mov [rax], rdx */
    
    sp.S_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_S_CLOSURE, 0);
    emit_mov_reg_mem(c, 2, 8, RTI_S);
    emit_mov_mem_reg(c, 0, 0, 2);
    
    sp.K_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_K_CLOSURE, 0);
    emit_mov_reg_mem(c, 2, 8, RTI_K);
    emit_mov_mem_reg(c, 0, 0, 2);
    
    sp.I_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_I_CLOSURE, 0);
    emit_mov_reg_mem(c, 2, 8, RTI_I);
    emit_mov_mem_reg(c, 0, 0, 2);
    
    /* Patch cont_done closure: lea rax, [rip + cont_done_closure]; 
       mov [r8+RTI_CONT_DONE], rax; patch its entry pointer */
    sp.cont_done_closure_patch = emit_lea_rip_rel(c, 0);
    emit_mov_mem_reg(c, 8, RTI_CONT_DONE, 0);
    /* Store done_bcl entry in the closure: mov rdx, r9; add rdx, done_bcl_off; mov [rax], rdx */
    emit_mov_reg_reg(c, 2, 9);
    emit(c, 0x48); emit(c, 0x81); emit(c, 0xc2);  /* add rdx, imm32 */
    sp.patch_done = c->pos;  /* Reuse patch_done for done_bcl offset */
    emit32(c, 0);
    emit_mov_mem_reg(c, 0, 0, 2);  /* [rax] = done_bcl entry */
    
    /*
     * Patch term closures: walk through term closure area, fix entry pointers
     */
    
    /* lea rsi, [rip + term_start] */
    sp.term_start_patch = emit_lea_rip_rel(c, 6);
    
    /* lea rdi, [rip + term_end] */
    sp.term_end_patch = emit_lea_rip_rel(c, 7);
    
    /* r9 still has code_base, need data_base */
    /* lea r10, [rip + data_base] */
    sp.data_base_patch = emit_lea_rip_rel(c, 10);
    
    /* mov r11, file_data_offset (imm64) */
    emit(c, 0x49); emit(c, 0xbb);  /* mov r11, imm64 */
    sp.file_data_off_patch = c->pos;
    emit64(c, 0);  /* placeholder */
    
    /* r9 = AP_entry from RuntimeInfo */
    emit_mov_reg_r8_off(c, 9, RTI_AP);
    
    /* Loop start */
    u32 loop_start = c->pos;
    
    /* cmp rsi, rdi */
    emit(c, 0x48); emit(c, 0x39); emit(c, 0xfe);  /* cmp rsi, rdi */
    
    /* jge done */
    emit(c, 0x0f); emit(c, 0x8d);  /* jge rel32 */
    u32 jge_done_patch = c->pos;
    emit32(c, 0);  /* placeholder */
    
    /* Check if [rsi] == ENTRY_MARKER_AP (0xFFFFFFFFFFFFFFFF) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x06);  /* mov rax, [rsi] */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xf8); emit(c, 0xff);  /* cmp rax, -1 */
    
    /* jne skip_patch */
    emit(c, 0x0f); emit(c, 0x85);  /* jne rel32 */
    u32 jne_skip_patch = c->pos;
    emit32(c, 0);  /* placeholder */
    
    /* This is an App closure - patch it */
    /* [rsi] = r9 (AP_entry) */
    emit(c, 0x4c); emit(c, 0x89); emit(c, 0x0e);  /* mov [rsi], r9 */
    
    /* Patch f: [rsi+8] = r10 + ([rsi+8] - r11) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x46); emit(c, 0x08);  /* mov rax, [rsi+8] */
    emit(c, 0x4c); emit(c, 0x29); emit(c, 0xd8);  /* sub rax, r11 */
    emit(c, 0x4c); emit(c, 0x01); emit(c, 0xd0);  /* add rax, r10 */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x46); emit(c, 0x08);  /* mov [rsi+8], rax */
    
    /* Patch arg: [rsi+16] = r10 + ([rsi+16] - r11) */
    emit(c, 0x48); emit(c, 0x8b); emit(c, 0x46); emit(c, 0x10);  /* mov rax, [rsi+16] */
    emit(c, 0x4c); emit(c, 0x29); emit(c, 0xd8);  /* sub rax, r11 */
    emit(c, 0x4c); emit(c, 0x01); emit(c, 0xd0);  /* add rax, r10 */
    emit(c, 0x48); emit(c, 0x89); emit(c, 0x46); emit(c, 0x10);  /* mov [rsi+16], rax */
    
    /* skip_patch: */
    *(i32*)(c->buf + jne_skip_patch) = (i32)(c->pos - jne_skip_patch - 4);
    
    /* rsi += 24 */
    emit(c, 0x48); emit(c, 0x83); emit(c, 0xc6); emit(c, 24);  /* add rsi, 24 */
    
    /* jmp loop_start */
    emit(c, 0xe9);  /* jmp rel32 */
    i32 jmp_back = (i32)(loop_start - c->pos - 4);
    emit32(c, jmp_back);
    
    /* done: */
    *(i32*)(c->buf + jge_done_patch) = (i32)(c->pos - jge_done_patch - 4);
    
    /* Set up CPS machine state and enter normalizer */
    /* rbx = root closure */
    sp.root_patch = emit_lea_rip_rel(c, 3);
    
    /* rcx = cont_done closure (from RTI_CONT_DONE) */
    emit_mov_reg_r8_off(c, 1, RTI_CONT_DONE);
    
    /* Jump to norm_entry */
    /* We need to patch this with the actual norm_entry offset */
    /* jmp [r8 + norm_entry_offset] would require norm_entry in RTI */
    /* Alternative: emit jmp rel32 and patch it */
    emit(c, 0xe9);  /* jmp rel32 */
    sp.patch_norm_entry = c->pos;
    emit32(c, 0);  /* placeholder - will be patched to norm_entry */
    
    return sp;
}

/*
 * Main ELF emission function
 */
int jit_emit_elf(int fd, SKITerm *term) {
    /* Allocate buffers */
    u64 max_size = 1024 * 1024;  /* 1MB should be enough */
    u8 *buf = malloc(max_size);
    if (!buf) return -1;
    memset(buf, 0, max_size);
    
    ElfBuf eb = {0};
    eb.buf = buf;
    eb.capacity = max_size;
    
    /*
     * Layout:
     * [ELF header: 64 bytes]
     * [Program headers: 2 * 56 = 112 bytes]
     * [Padding to 0x1000]
     * [.text: code]
     * [Padding to page boundary]
     * [.data: RuntimeInfo + closures]
     */
    
    u64 ehdr_size = ELF_EHDR_SIZE;
    u64 phdr_off = ehdr_size;
    u64 phdr_count = 2;
    u64 phdr_size = phdr_count * ELF_PHDR_SIZE;
    u64 headers_end = phdr_off + phdr_size;
    
    /* Align .text to 0x1000 */
    u64 text_off = (headers_end + 0xfff) & ~0xfffULL;
    eb.text_start = text_off;
    
    /* Emit code into a temporary Code struct */
    u8 *code_buf = buf + text_off;
    Code c = { code_buf, 0, max_size - text_off };
    
    /* Emit _start */
    StartPatches sp = emit_elf_start_v2(&c, &eb);
    
    /*
     * Emit the SHARED runtime core - same code as native.
     * No trampoline - ELF _start sets up registers directly.
     */
    u64 code_base = 0;  /* PIE: everything is relative */
    emit_shared_runtime_core(&c, code_base, &eb.eo);
    
    /* Emit ELF-specific CPS-based BCL output and exit */
    BclCpsOffsets bo;
    emit_elf_bcl_cps(&c, &eb, &bo);
    
    u64 text_size = c.pos;
    eb.text_size = text_size;
    
    /* Align .data to page boundary */
    u64 data_off = text_off + ((text_size + 0xfff) & ~0xfffULL);
    eb.data_start = data_off;
    
    /* Build .data section */
    u8 *data_buf = buf + data_off;
    u32 data_pos = 0;
    
    /* RuntimeInfo (120 bytes, zeroed - filled at runtime) */
    eb.rti_off = data_pos;
    data_pos += RTI_SIZE;
    
    /* S closure (8 bytes - entry pointer, filled at runtime) */
    eb.S_closure_off = data_pos;
    *(u64*)(data_buf + data_pos) = 0;  /* placeholder */
    data_pos += 8;
    
    /* K closure */
    eb.K_closure_off = data_pos;
    *(u64*)(data_buf + data_pos) = 0;
    data_pos += 8;
    
    /* I closure */
    eb.I_closure_off = data_pos;
    *(u64*)(data_buf + data_pos) = 0;
    data_pos += 8;
    
    /* cont_done closure (8 bytes - entry pointer, will be patched to cont_done_bcl) */
    eb.cont_done_closure_off = data_pos;
    *(u64*)(data_buf + data_pos) = 0;  /* placeholder */
    data_pos += 8;
    
    /* Term closures */
    eb.term_off = data_pos;
    u32 root_off = serialize_term(data_buf, &data_pos, term,
                                   eb.S_closure_off, eb.K_closure_off, eb.I_closure_off,
                                   data_off);
    eb.term_size = data_pos - eb.term_off;
    
    u64 data_size = data_pos;
    eb.data_size = data_size;
    
    /*
     * Patch _start with correct offsets
     */
    
    /* Patch lea r8, [rip + rti_off] */
    /* The displacement is: target - (rip after instruction) */
    /* rip after = text_off + sp.r8_rti_patch + 4 */
    /* target = data_off + eb.rti_off */
    i32 r8_disp = (data_off + eb.rti_off) - (text_off + sp.r8_rti_patch + 4);
    *(i32*)(code_buf + sp.r8_rti_patch) = r8_disp;
    
    /* Patch lea rcx, [rip + code_base] -> code base is start of .text */
    /* We want rcx = text_off (as an offset that, when added to load base, gives code start) */
    /* Actually for PIE, we want rcx to point to the start of .text in memory */
    /* lea rcx, [rip + disp] where disp = text_off - (rip after) */
    /* This gives us the runtime address of .text start */
    i32 code_base_disp = text_off - (text_off + sp.code_base_patch + 4);
    /* Hmm, this is just text_off - text_off - 4 - patch_offset? Let me think again */
    /* The lea is at offset sp.code_base_patch within .text */
    /* rip after instruction = load_base + text_off + sp.code_base_patch + 4 */
    /* We want rcx = load_base + text_off (start of .text) */
    /* disp = (load_base + text_off) - (load_base + text_off + sp.code_base_patch + 4) */
    /* disp = -(sp.code_base_patch + 4) */
    code_base_disp = -(i32)(sp.code_base_patch + 4);
    *(i32*)(code_buf + sp.code_base_patch) = code_base_disp;
    
    /* Patch entry offsets in RuntimeInfo setup */
    *(i32*)(code_buf + sp.patch_done) = bo.cont_done_bcl;  /* done entry points to CPS BCL handler */
    *(i32*)(code_buf + sp.patch_S) = eb.eo.S;
    *(i32*)(code_buf + sp.patch_K) = eb.eo.K;
    *(i32*)(code_buf + sp.patch_I) = eb.eo.I;
    *(i32*)(code_buf + sp.patch_S1) = eb.eo.S1;
    *(i32*)(code_buf + sp.patch_S2) = eb.eo.S2;
    *(i32*)(code_buf + sp.patch_K1) = eb.eo.K1;
    *(i32*)(code_buf + sp.patch_AP) = eb.eo.AP;
    *(i32*)(code_buf + sp.patch_IND) = eb.eo.IND;
    
    /* Patch CPS continuation entries */
    *(i32*)(code_buf + sp.patch_cont_app) = eb.eo.cont_app;
    *(i32*)(code_buf + sp.patch_cont_s1) = eb.eo.cont_s1;
    *(i32*)(code_buf + sp.patch_cont_s2x) = eb.eo.cont_s2x;
    *(i32*)(code_buf + sp.patch_cont_s2y) = eb.eo.cont_s2y;
    *(i32*)(code_buf + sp.patch_cont_k1) = eb.eo.cont_k1;
    
    /* Patch BCL CPS entries */
    *(i32*)(code_buf + sp.patch_bcl_emit) = bo.bcl_emit;
    *(i32*)(code_buf + sp.patch_bcl_arg) = bo.cont_bcl_arg;
    *(i32*)(code_buf + sp.patch_bcl_done) = bo.cont_bcl_done;
    
    /* Patch closure lea instructions */
    /* S closure: lea at sp.S_closure_patch, target = data_off + S_closure_off */
    i32 S_closure_disp = (data_off + eb.S_closure_off) - (text_off + sp.S_closure_patch + 4);
    *(i32*)(code_buf + sp.S_closure_patch) = S_closure_disp;
    
    i32 K_closure_disp = (data_off + eb.K_closure_off) - (text_off + sp.K_closure_patch + 4);
    *(i32*)(code_buf + sp.K_closure_patch) = K_closure_disp;
    
    i32 I_closure_disp = (data_off + eb.I_closure_off) - (text_off + sp.I_closure_patch + 4);
    *(i32*)(code_buf + sp.I_closure_patch) = I_closure_disp;
    
    /* Patch root closure lea */
    i32 root_disp = (data_off + root_off) - (text_off + sp.root_patch + 4);
    *(i32*)(code_buf + sp.root_patch) = root_disp;
    
    /*
     * Patch term closure patching loop parameters
     */
    
    /* term_start: lea rsi, [rip + term_start] */
    i32 term_start_disp = (data_off + eb.term_off) - (text_off + sp.term_start_patch + 4);
    *(i32*)(code_buf + sp.term_start_patch) = term_start_disp;
    
    /* term_end: lea rdi, [rip + term_end] */
    i32 term_end_disp = (data_off + eb.term_off + eb.term_size) - (text_off + sp.term_end_patch + 4);
    *(i32*)(code_buf + sp.term_end_patch) = term_end_disp;
    
    /* data_base: lea r9, [rip + data_start] */
    i32 data_base_disp_v2 = data_off - (text_off + sp.data_base_patch + 4);
    *(i32*)(code_buf + sp.data_base_patch) = data_base_disp_v2;
    
    /* file_data_offset: mov r10, data_off */
    *(u64*)(code_buf + sp.file_data_off_patch) = data_off;
    
    /* Patch cont_done closure LEA */
    i32 cont_done_closure_disp = (data_off + eb.cont_done_closure_off) - (text_off + sp.cont_done_closure_patch + 4);
    *(i32*)(code_buf + sp.cont_done_closure_patch) = cont_done_closure_disp;
    
    /* Patch the cont_done closure entry pointer in .data */
    /* It should point to cont_done_bcl entry */
    *(u64*)(data_buf + eb.cont_done_closure_off) = text_off + bo.cont_done_bcl;
    
    /* Patch jump to norm_entry */
    i32 norm_entry_disp = (text_off + eb.eo.norm_entry) - (text_off + sp.patch_norm_entry + 4);
    *(i32*)(code_buf + sp.patch_norm_entry) = norm_entry_disp;
    
    /*
     * Build ELF headers
     */
    
    /* Entry point is _start offset */
    u64 entry = text_off + eb.start_off;
    emit_elf_header(buf, entry, phdr_off, phdr_count);
    
    /* Program headers */
    /* Text segment: RX */
    emit_phdr(buf + phdr_off, PT_LOAD, PF_R | PF_X,
              text_off, text_off, text_size, text_size, 0x1000);
    
    /* Data segment: RW */
    emit_phdr(buf + phdr_off + ELF_PHDR_SIZE, PT_LOAD, PF_R | PF_W,
              data_off, data_off, data_size, data_size, 0x1000);
    
    /*
     * Write to file
     */
    u64 total_size = data_off + data_size;
    ssize_t written = write(fd, buf, total_size);
    
    free(buf);
    
    return (written == (ssize_t)total_size) ? 0 : -1;
}
