/*
 * node.h - Parse tree nodes with reference counting
 *
 * Two node types:
 *   - Branch: has left and right children
 *   - Leaf: has integer or pointer data
 *
 * Nodes are tagged with lexeme (source location) and type/variety bytes.
 * Reference counting via hold/release for memory management.
 */
#ifndef NODE_H
#define NODE_H

#include <stdbool.h>
#include <stdio.h>
#include "types.h"
#include "lexeme.h"

typedef struct Node Node;

/*
 * Tag: lexeme + fixity for operators
 * Stored as first 8 bytes of node, compact representation
 */
typedef struct {
    Lexeme lexeme;
    i8 fixity;      /* For operators: NOFIX, PREFIX, INFIX, etc. */
} Tag;

/* Fixity values */
typedef enum {
    NOFIX = 0,      /* Nullary operator (e.g., true, false) */
    PREFIX,         /* Prefix operator (e.g., -, not) */
    INFIX,          /* Infix operator (e.g., +, *, ->) */
    POSTFIX,        /* Postfix operator (e.g., !, ?) */
    OPENFIX,        /* Opening bracket (e.g., (, [, {) */
    CLOSEFIX        /* Closing bracket (e.g., ), ], }) */
} Fixity;

/* Associativity */
typedef enum {
    ASSOC_L,        /* Left associative */
    ASSOC_R,        /* Right associative */
    ASSOC_N         /* Non-associative */
} Assoc;

/* Precedence level (higher = binds tighter) */
typedef u8 Precedence;

/* Initialize node allocator */
void node_init(void);

/* Free all nodes */
void node_destroy(void);

/* Constructors */
Node *node_branch(Tag tag, i8 type, i8 variety, Node *left, Node *right);
Node *node_pair(Node *left, Node *right);  /* Untagged pair */
Node *node_leaf(Tag tag, i8 type, i8 variety, i64 data);
Node *node_ptr_leaf(Tag tag, i8 type, i8 variety, void *data);

/* Accessors */
Tag node_tag(Node *n);
void node_set_tag(Node *n, Tag tag);
Node *node_left(Node *n);
Node *node_right(Node *n);
void node_set_left(Node *n, Node *left);
void node_set_right(Node *n, Node *right);
i8 node_type(Node *n);
void node_set_type(Node *n, i8 type);
i8 node_variety(Node *n);
void node_set_variety(Node *n, i8 variety);
i64 node_value(Node *n);
void node_set_value(Node *n, i64 value);
void *node_data(Node *n);

/* Reference counting */
Node *node_hold(Node *n);   /* Increment refcount, return same node */
void node_release(Node *n); /* Decrement refcount, free if zero */

/* Tag operations */
Tag tag_new(Lexeme lexeme, i8 fixity);
Tag tag_literal(const char *name, Location loc, i8 fixity);
Lexeme tag_lexeme(Tag tag);
i8 tag_fixity(Tag tag);
bool tag_eq(Tag a, Tag b);
bool tag_eq_str(Tag a, const char *str);
void tag_print(Tag tag, FILE *out);

/* Error reporting */
void syntax_error(const char *msg, Tag tag);
void syntax_error_if(bool cond, const char *msg, Tag tag);
void syntax_error_node(const char *msg, Node *n);
void syntax_error_node_if(bool cond, const char *msg, Node *n);

#endif /* NODE_H */
