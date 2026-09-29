#include <libeezo/mem.h>
#include "tree.h"
#include "ast.h"
#include "patterns.h"

/* every node a name, a colon pair over a valid pattern, or an application of valid patterns: a walk on a heap stack */
bool isValidPattern(Node* root) {
    Stack st = STACK_INIT(Node*);
    STACK_PUSH(&st, Node*, root);
    bool ok = true;
    while (ok && st.n) {
        Node* node = STACK_POP(&st, Node*);
        if (isName(node)) continue;
        if (isColonPair(node)) { STACK_PUSH(&st, Node*, getLeft(node)); continue; }
        if (isJuxtaposition(node)) { STACK_PUSH(&st, Node*, getRight(node)); STACK_PUSH(&st, Node*, getLeft(node)); continue; }
        ok = false;
    }
    stack_drop(&st);
    return ok;
}

unsigned int getArgumentCount(Node* application) {
    unsigned int i = 0;
    for (Node* n = application; isJuxtaposition(n); ++i)
        n = getLeft(n);
    return i;
}

static Node* newProjector(Tag tag, unsigned int size, unsigned int index) {
    Node* projector = Underscore(tag, size - index);
    for (unsigned int i = 0; i < size; ++i)
        projector = UnderscoreArrow(tag, projector);
    return projector;
}

/* A pattern's arrow, by the rules below - an explicit machine, not C recursion: an as-pattern waits for its inner
   arrow, an application's parameters are wrapped one at a time from the right, each in a frame. */
enum { NA_AS, NA_JUX };
typedef struct { int kind; Node* left; Node* node; } ArrowFrame;
Node* newArrow(Node* left, Node* right) {
    Stack frames = STACK_INIT(ArrowFrame);
    Node* ret;
call:
    for (;;) {
        if (isColonPair(left)) { left = getLeft(left); continue; }
        if (isName(left)) { ret = SingleArrow(left, right); break; }
        // example: p@(x, y) -> body  ~>  p -> (((x, y) -> body) p)
        if (isAsPattern(left)) {
            ArrowFrame f = { NA_AS, left, NULL }; STACK_PUSH(&frames, ArrowFrame, f);
            left = getRight(left); continue;
        }
        // example: (x, y) -> body  ~>  _ -> (x -> y -> body) first(_) second(_)
        syntaxErrorNodeIf(!isJuxtaposition(left), "invalid parameter", left);
        ArrowFrame f = { NA_JUX, left, left }; STACK_PUSH(&frames, ArrowFrame, f);
        left = getRight(left); continue;
    }
    while (frames.n) {
        ArrowFrame f = STACK_POP(&frames, ArrowFrame);
        if (f.kind == NA_AS) {
            right = Juxtaposition(getTag(f.left), ret, Underscore(getTag(f.left), 1));
            left = getLeft(f.left);
            goto call;
        }
        Node* node = getLeft(f.node);   /* the next parameter, leftwards, around the body so far */
        if (isJuxtaposition(node)) {
            f.node = node; STACK_PUSH(&frames, ArrowFrame, f);
            left = getRight(node); right = ret;
            goto call;
        }
        Tag tag = getTag(node);
        Node* body = ret;
        for (unsigned int i = 0, size = getArgumentCount(f.left); i < size; ++i)
            body = Juxtaposition(tag, body, Juxtaposition(tag,
                Underscore(tag, 1), newProjector(tag, size, i)));
        ret = UnderscoreArrow(tag, body);
    }
    stack_drop(&frames);
    return ret;
}

Node* newCase(Node* left, Node* right) {
    if (isUnderscore(left))
        return DefaultCaseArrow(FixedName(getTag(left), "this"), right);

    // example: (x, y) -> B ---> (,)(x)(y) -> B ---> this -> this(x -> y -> B)
    for (; isJuxtaposition(left); left = getLeft(left))
        right = newArrow(getRight(left), right);
    syntaxErrorNodeIf(!isName(left) && !isNumber(left), "invalid case", left);

    Tag tag = getTag(left);
    Node* this = FixedName(tag, "this");
    return ExplicitCaseArrow(tag, this, Juxtaposition(tag, this, right));
}

static Node* attachDefaultCase(Tag tag, Node* caseArrow, Node* fallback) {
    // Constructor(a) -> b; _ -> c  ~>
    //   this -> @Constructor(a -> b, _ -> c, this)
    // we wrap it in an arrow so that transformRecursion knows it's a function
    Tag constructorTag = getTag(caseArrow);
    Node* deconstructor = Name(addPrefix(constructorTag, '@'));
    Node* reconstructor = getRight(getRight(caseArrow));
    Node* this = FixedName(tag, "this");
    Node* body = Juxtaposition(tag, Juxtaposition(tag, Juxtaposition(tag,
        deconstructor, reconstructor), fallback), this);
    return DefaultCaseArrow(this, body);
}

/* base applied to the extension's arguments, left to right: its left spine folded from the bottom (a loop) */
static Node* combineCaseBodies(Tag tag, Node* base, Node* extension) {
    Stack rights = STACK_INIT(Node*);
    for (; isJuxtaposition(extension); extension = getLeft(extension)) STACK_PUSH(&rights, Node*, getRight(extension));
    Node* merged = base;
    while (rights.n) merged = Juxtaposition(tag, merged, STACK_POP(&rights, Node*));
    stack_drop(&rights);
    return merged;
}

static int getCaseCount(Node* body) {
    int n = 0;
    for (; isJuxtaposition(body); body = getLeft(body)) n++;
    return n;
}

Node* combineCases(Tag tag, Node* left, Node* right) {
    if (isDefaultCase(left))
        syntaxError("invalid default case position", getTag(left));
    if (getCaseCount(getRight(left)) > 1)
        syntaxError("invalid case indentation", getTag(left));
    if (isDefaultCase(right))
        return attachDefaultCase(tag, left, right);
    Node* this = FixedName(tag, "this");
    Node* body = combineCaseBodies(tag, getRight(left), getRight(right));
    return ExplicitCaseArrow(tag, this, body);
}
