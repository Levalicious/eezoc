#include <libeezo/mem.h>
#include <assert.h>
#include <stdlib.h>
#include "util.h"
#include "array.h"

struct Array { Stack s; };   /* a Stack of the memory layer (libeezo/mem.h) */

size_t length(const Array* array) {return array->s.n;}

Array* newArray(size_t initialCapacity) {
    Array* array = (Array*)rmalloc(sizeof(Array));
    stack_init(&array->s, sizeof(void*));
    stack_reserve(&array->s, initialCapacity);
    return array;
}

void deleteArray(Array* array) {
    stack_drop(&array->s);
    free(array);
}

void append(Array* array, void* value) { STACK_PUSH(&array->s, void*, value); }

void* unappend(Array* array) {
    assert(array->s.n > 0);
    return STACK_POP(&array->s, void*);
}

void* elementAt(const Array* array, size_t index) {
    assert(index < array->s.n);
    return STACK_AT(&array->s, void*, index);
}
