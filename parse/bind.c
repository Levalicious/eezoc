#include <libeezo/mem.h>
#include "tree.h"
#include "array.h"
#include "ast.h"
#include "term.h"
#include "bind.h"

extern bool isIO, TRACE;
bool INLINE = true;
Term *TRUE = NULL, *FALSE = NULL;

static unsigned long long findDebruijnIndex(Node* name, Array* parameters) {
    syntaxErrorNodeIf(isUnused(name),
        "cannot reference a symbol starting with underscore", name);
    for (size_t i = 1; i <= length(parameters); ++i) {
        Node* parameter = elementAt(parameters, length(parameters) - i);
        if (isSameTag(getTag(parameter), getTag(name))) {
            syntaxErrorNodeIf(isForbidden(name), "cannot reference", name);
            return (unsigned long long)i;
        }
    }
    return 0;
}

static OperationCode findOperationCode(Node* name) {
    for (OperationCode i = 0; i < sizeof(Operations)/sizeof(char*); ++i)
        if (isThisName(name, Operations[i]))
            return i;
    return NONE;
}

static void bindReference(Node* node, Array* parameters, size_t globalDepth) {
    OperationCode operationCode = findOperationCode(node);
    if (isPseudoOperation(operationCode)) {
        setType(node, OPERATION);
        setVariety(node, (char)operationCode);
        return;
    }
    unsigned long long i = (unsigned long long)getValue(node);
    unsigned long long index = i > 0 ? i : findDebruijnIndex(node, parameters);
    syntaxErrorNodeIf(index == 0, "undefined symbol", node);
    unsigned long long localDepth = length(parameters) - globalDepth;
    long long debruijn = (long long)(index <= localDepth ? index :
        index - length(parameters) - 1);
    setType(node, VARIABLE);
    setValue(node, debruijn);
}

/* Binding: a depth-first walk with frames on a heap stack, not C recursion. An arrow's parameter is in scope for its
   body (appended before, removed after); an application's sides are bound left then right; each node takes its bound
   form after its children, as before. */
typedef struct { Node* node; int st; } BindFrame;
static void bindWith(Node* root, Array* parameters, const Array* globals) {
    Stack frames = STACK_INIT(BindFrame);
    BindFrame f0 = { root, 0 }; STACK_PUSH(&frames, BindFrame, f0);
    while (frames.n) {
        BindFrame* f = &STACK_TOP(&frames, BindFrame);
        Node* node = f->node;
        switch (getASTType(node)) {
            case REFERENCE:
                bindReference(node, parameters, length(globals)); frames.n--; break;
            case ARROW:
                if (f->st == 0) {
                    append(parameters, getParameter(node));
                    f->st = 1;
                    BindFrame c = { getBody(node), 0 }; STACK_PUSH(&frames, BindFrame, c);
                    break;
                }
                unappend(parameters);
                setTag(node, getTag(getParameter(node)));
                setType(node, ABSTRACTION);
                if (INLINE && isGlobal(getBody(node)))
                    setBody(node, getGlobalReferent(getBody(node), globals));
                frames.n--; break;
            case JUXTAPOSITION:
            case LET:
                if (f->st < 2) {
                    Node* side = f->st == 0 ? getLeft(node) : getRight(node);
                    f->st += 1;
                    BindFrame c = { side, 0 }; STACK_PUSH(&frames, BindFrame, c);
                    break;
                }
                setType(node, APPLICATION);
                if (INLINE && isGlobal(getLeft(node)))
                    setLeft(node, getGlobalReferent(getLeft(node), globals));
                if (INLINE && isGlobal(getRight(node)))
                    setRight(node, getGlobalReferent(getRight(node), globals));
                frames.n--; break;
            case NUMBER:
                setType(node, NUMERAL); frames.n--; break;
            case DEFINITION:
                syntaxErrorNode("missing scope for definition", node); frames.n--; break;
            case ASPATTERN:
                syntaxErrorNode("as pattern not in valid location", node); frames.n--; break;
            case COMMAPAIR:
                syntaxErrorNode("comma not inside brackets", node); frames.n--; break;
            case COLONPAIR:
                syntaxErrorNode("colon not in valid location", node); frames.n--; break;
            case SETBUILDER:
                syntaxErrorNode("bracket not in valid location", node); frames.n--; break;
            case OPERATOR:
                assert(false); frames.n--; break;
            default:
                frames.n--; break;
        }
    }
    stack_drop(&frames);
}

Array* bind(Hold* root) {
    INLINE = isIO && !TRACE;
    Node* node = root;
    Array* parameters = newArray(2048);         // names of globals and locals
    Array* globals = newArray(2048);            // values of globals
    while (isLet(node) && !isUnderscore(getParameter(getLeft(node)))) {
        Node* definiendum = getParameter(getLeft(node));
        Node* definiens = getRight(node);
        Tag tag = getTag(definiendum);
        bindWith(definiens, parameters, globals);
        OperationCode code = findOperationCode(definiendum);
        if (code != NONE && !isPseudoOperation(code)) {
            syntaxErrorIf(!TRUE || !FALSE, "must define booleans before", tag);
            setRight(node, Operation(tag, code, definiens));
        } else if (TRUE == NULL && isThisTag(tag, "True"))
            TRUE = definiens;
        else if (FALSE == NULL && isThisTag(tag, "False"))
            FALSE = definiens;
        append(parameters, definiendum);
        append(globals, getRight(node));
        setType(node, APPLICATION);
        setType(getLeft(node), ABSTRACTION);
        setTag(getLeft(node), tag);
        node = getBody(getLeft(node));
    }
    bindWith(node, parameters, globals);
    deleteArray(parameters);
    append(globals, node);
    return globals;
}
