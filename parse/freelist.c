#include <assert.h>
#include <stddef.h>
#include <libeezo/mem.h>
#include "freelist.h"

/* The parse tree's nodes: a Pool of the memory layer (libeezo/mem.h) - pages and a free list, growing, its failure
   the layer's resource abort */
static Pool POOL;
static int POOL_LIVE = 0;
static size_t SIZE = 0;

void initPool(size_t itemSize, size_t initialCapacity) {
    assert(!POOL_LIVE);
    SIZE = itemSize;
    pool_setup(&POOL, itemSize, initialCapacity);
    POOL_LIVE = 1;
}

void destroyPool(void) {
    pool_drop(&POOL);
    POOL_LIVE = 0;
}
size_t getMemoryUsage(void) {return POOL.live * SIZE;}

void* allocate(void) {return pool_get(&POOL);}

void reclaim(void* allocated) {pool_put(&POOL, allocated);}
