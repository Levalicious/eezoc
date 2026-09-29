#include <libeezo/mem.h>
#include <string.h>
#include "tree.h"
#include "opp/operator.h"
#include "ast.h"
#include "brackets.h"

Node* Nil(Tag tag) {return FixedName(tag, "[]");}

Node* prepend(Tag tag, Node* item, Node* list) {
    return Juxtaposition(tag, Juxtaposition(tag,
        Name(newLiteralTag("::", getLexeme(tag).location, INFIX)), item), list);
}

/* the comma list's elements, counted along its left spine (a loop) */
static size_t getCommaListLength(Node* node) {
    size_t n = 1;
    for (; isCommaPair(node); node = getLeft(node)) n++;
    return n;
}

/* base applied to the comma list's elements, left to right: the list's left spine folded from its bottom (a loop) */
static Node* applyToCommaList(Tag tag, Node* base, Node* arguments) {
    Stack rights = STACK_INIT(Node*);
    for (; isCommaPair(arguments); arguments = getLeft(arguments)) STACK_PUSH(&rights, Node*, getRight(arguments));
    Node* acc = Juxtaposition(tag, base, arguments);
    while (rights.n) acc = Juxtaposition(tag, acc, STACK_POP(&rights, Node*));
    stack_drop(&rights);
    return acc;
}

/* the name of the n-tuple's constructor: n - 1 commas, however many (a lexeme's length is its 32-bit field) */
static Node* newSpineName(Tag tag, char c, size_t length) {
    syntaxErrorIf(length > MAX_LEXEME_LENGTH, "too many arguments", tag);
    char* name = rmalloc(length + 1); memset(name, c, length); name[length] = 0;   /* the lexeme's text lives on */
    Lexeme lexeme = newLexeme(name, (unsigned int)length, getLexeme(tag).location);
    return Name(newTag(lexeme, NOFIX));
}

static Node* newTuple(Tag tag, Node* commaList) {
    Node* name = newSpineName(tag, ',', getCommaListLength(commaList) - 1);
    return applyToCommaList(tag, name, commaList);
}

Node* reduceOpenParenthesis(Tag tag, Node* before, Node* contents) {
    if (contents == NULL) {
        syntaxErrorIf(before != NULL, "missing argument to", tag);
        return FixedName(tag, "()");
    }
    if (isDefinition(contents))
        syntaxErrorNode("missing scope for definition", contents);
    if (before != NULL)
        return applyToCommaList(tag, before, contents);
    if (isCommaPair(contents))
        return newTuple(tag, contents);
    if (isArrow(contents))
        setVariety(contents, SINGLE);
    if (isJuxtaposition(contents))
        setTag(contents, tag);
    return contents;
}

Node* reduceOpenSquareBracket(Tag tag, Node* before, Node* contents) {
    if (contents == NULL) {
        syntaxErrorIf(before != NULL, "missing argument to", tag);
        return Nil(tag);
    }
    if (before != NULL) {
        Node* name = newSpineName(tag, '[', getCommaListLength(contents));
        Node* base = Juxtaposition(tag, name, before);
        return applyToCommaList(tag, base, contents);
    }
    Node* list = Nil(tag);
    if (!isCommaPair(contents))
        return prepend(tag, contents, list);
    for (; isCommaPair(contents); contents = getLeft(contents))
        list = prepend(tag, getRight(contents), list);
    return prepend(tag, contents, list);
}

Node* reduceOpenBrace(Tag tag, Node* before, Node* patterns) {
    syntaxErrorIf(before != NULL, "invalid operand before", tag);
    if (patterns == NULL)
        return SetBuilder(
            newLiteralTag("{}", getLexeme(tag).location, 0), NULL);
    return SetBuilder(tag, newTuple(tag, patterns));
}

Node* reduceOpenFile(Tag tag, Node* before, Node* contents) {
    syntaxErrorIf(before != NULL, "invalid operand before", tag);
    return contents;
}
