#include "tide/heap.h"

#include <string.h>

#include "tide/net.h"

// See tide/heap.h.

#define FIRST 16u // Offset 0 means no block, so blocks start after it

static uint32_t class_of(const uint32_t size)
{
    uint32_t c = 0;
    while (c + 1 < TIDE_HEAP_CLASSES && (16u << c) < size) c++;
    return c;
}

uint32_t tide_heap_alloc(tide_heap *h, const uint32_t bytes)
{
    const uint32_t size = (uint32_t)sizeof(tide_block) + bytes;
    const uint32_t c = class_of(size);
    const uint32_t block_size = 16u << c;
    if (block_size < size) {
        h->failed++;
        return 0;
    }
    if (h->free[c]) {
        const uint32_t block = h->free[c];
        tide_block *b = tide_heap_block(h, block);
        h->free[c] = b->next;
        b->next = 0;
        return block;
    }
    if (h->used < FIRST) h->used = FIRST;
    if (block_size > TIDE_HEAP_BYTES - h->used) {
        h->failed++;
        return 0;
    }
    const uint32_t block = h->used;
    h->used += block_size;
    tide_block *b = tide_heap_block(h, block);
    b->size_class = c;
    b->next = 0;
    return block;
}

void tide_heap_release(tide_heap *h, const uint32_t block)
{
    if (!block) return;
    tide_heap_block(h, block)->next = h->pending;
    h->pending = block;
}

void tide_heap_flush(tide_heap *h)
{
    while (h->pending) {
        const uint32_t block = h->pending;
        tide_block *b = tide_heap_block(h, block);
        h->pending = b->next;
        const uint32_t c = b->size_class;
        memset(b, 0, 16u << c); // Freed memory is zero, so it hashes the same everywhere
        b->size_class = c;
        b->next = h->free[c];
        h->free[c] = block;
    }
}

void tide_heap_copy(tide_heap *to, const tide_heap *from)
{
    if (to->used > from->used) memset(to->bytes + from->used, 0, to->used - from->used);
    to->used = from->used;
    memcpy(to->free, from->free, sizeof to->free);
    to->pending = from->pending;
    to->failed = from->failed;
    memcpy(to->bytes, from->bytes, from->used);
}

uint64_t tide_heap_hash(uint64_t h, const tide_heap *heap)
{
    h = tide_hash_more(h, &heap->used, sizeof heap->used);
    h = tide_hash_more(h, heap->free, sizeof heap->free);
    h = tide_hash_more(h, &heap->pending, sizeof heap->pending);
    return tide_hash_more(h, heap->bytes, heap->used);
}
