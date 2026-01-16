/*
 * node.c - Parse tree node implementation
 */
#include "node.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Node structure: 32 bytes
 * - tag: lexeme + fixity
 * - type, variety: classification bytes  
 * - refcount: reference count
 * - is_branch: flag for branch vs leaf
 * - union: left/right or value/data
 */
struct Node {
    Tag tag;
    i8 type;
    i8 variety;
    u16 refcount;
    bool is_branch;
    union {
        struct { Node *left, *right; } branch;
        struct { i64 value; } leaf;
        struct { void *data; } ptr;
    };
};

/* Simple freelist allocator */
#define POOL_SIZE 65536
static Node *pool = NULL;
static Node *freelist = NULL;
static u32 pool_used = 0;

void node_init(void) {
    pool = calloc(POOL_SIZE, sizeof(Node));
    freelist = NULL;
    pool_used = 0;
}

void node_destroy(void) {
    free(pool);
    pool = NULL;
    freelist = NULL;
    pool_used = 0;
}

static Node *alloc_node(void) {
    if (freelist) {
        Node *n = freelist;
        freelist = freelist->branch.left;
        return n;
    }
    if (pool_used >= POOL_SIZE) {
        fprintf(stderr, "Error: node pool exhausted\n");
        exit(1);
    }
    return &pool[pool_used++];
}

static void free_node(Node *n) {
    n->branch.left = freelist;
    freelist = n;
}

Node *node_branch(Tag tag, i8 type, i8 variety, Node *left, Node *right) {
    Node *n = alloc_node();
    n->tag = tag;
    n->type = type;
    n->variety = variety;
    n->refcount = 0;
    n->is_branch = true;
    n->branch.left = left;
    n->branch.right = right;
    if (left) node_hold(left);
    if (right) node_hold(right);
    return n;
}

Node *node_pair(Node *left, Node *right) {
    Tag empty = { EMPTY_LEXEME, NOFIX };
    return node_branch(empty, -1, 0, left, right);
}

Node *node_leaf(Tag tag, i8 type, i8 variety, i64 data) {
    Node *n = alloc_node();
    n->tag = tag;
    n->type = type;
    n->variety = variety;
    n->refcount = 0;
    n->is_branch = false;
    n->leaf.value = data;
    return n;
}

Node *node_ptr_leaf(Tag tag, i8 type, i8 variety, void *data) {
    Node *n = alloc_node();
    n->tag = tag;
    n->type = type;
    n->variety = variety;
    n->refcount = 0;
    n->is_branch = false;
    n->ptr.data = data;
    return n;
}

Tag node_tag(Node *n) {
    return n->tag;
}

void node_set_tag(Node *n, Tag tag) {
    n->tag = tag;
}

Node *node_left(Node *n) {
    assert(n->is_branch);
    return n->branch.left;
}

Node *node_right(Node *n) {
    assert(n->is_branch);
    return n->branch.right;
}

void node_set_left(Node *n, Node *left) {
    assert(n->is_branch);
    if (n->branch.left) node_release(n->branch.left);
    n->branch.left = left;
    if (left) node_hold(left);
}

void node_set_right(Node *n, Node *right) {
    assert(n->is_branch);
    if (n->branch.right) node_release(n->branch.right);
    n->branch.right = right;
    if (right) node_hold(right);
}

i8 node_type(Node *n) {
    return n->type;
}

void node_set_type(Node *n, i8 type) {
    n->type = type;
}

i8 node_variety(Node *n) {
    return n->variety;
}

void node_set_variety(Node *n, i8 variety) {
    n->variety = variety;
}

i64 node_value(Node *n) {
    assert(!n->is_branch);
    return n->leaf.value;
}

void node_set_value(Node *n, i64 value) {
    assert(!n->is_branch);
    n->leaf.value = value;
}

void *node_data(Node *n) {
    assert(!n->is_branch);
    return n->ptr.data;
}

Node *node_hold(Node *n) {
    if (n) n->refcount++;
    return n;
}

void node_release(Node *n) {
    if (!n) return;
    if (n->refcount == 0) {
        fprintf(stderr, "Error: releasing node with zero refcount\n");
        exit(1);
    }
    n->refcount--;
    if (n->refcount == 0) {
        if (n->is_branch) {
            node_release(n->branch.left);
            node_release(n->branch.right);
        }
        free_node(n);
    }
}

Tag tag_new(Lexeme lexeme, i8 fixity) {
    Tag t = { lexeme, fixity };
    return t;
}

Tag tag_literal(const char *name, Location loc, i8 fixity) {
    return tag_new(lexeme_literal(name, loc), fixity);
}

Lexeme tag_lexeme(Tag tag) {
    return tag.lexeme;
}

i8 tag_fixity(Tag tag) {
    return tag.fixity;
}

bool tag_eq(Tag a, Tag b) {
    return lexeme_eq(a.lexeme, b.lexeme);
}

bool tag_eq_str(Tag a, const char *str) {
    return lexeme_eq_str(a.lexeme, str);
}

void tag_print(Tag tag, FILE *out) {
    lexeme_print(tag.lexeme, out);
}

void syntax_error(const char *msg, Tag tag) {
    fprintf(stderr, "Syntax error: %s '", msg);
    tag_print(tag, stderr);
    fprintf(stderr, "' at ");
    location_print(tag.lexeme.loc, stderr);
    fprintf(stderr, "\n");
    exit(1);
}

void syntax_error_if(bool cond, const char *msg, Tag tag) {
    if (cond) syntax_error(msg, tag);
}

void syntax_error_node(const char *msg, Node *n) {
    syntax_error(msg, node_tag(n));
}

void syntax_error_node_if(bool cond, const char *msg, Node *n) {
    if (cond) syntax_error_node(msg, n);
}
