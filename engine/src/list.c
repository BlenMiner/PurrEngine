#include "tide/list.h"

#include <string.h>

#include "tide/text.h"

// See tide/list.h. A block's header holds the count in `a` and the capacity
// in `b`, and the elements follow it.

static void *items(tide_block *b)
{
    return b + 1;
}

int32_t tide_list_count(const tide_list l)
{
    const tide_block *b = tide_block_at(l.at);
    return b ? (int32_t)b->a : 0;
}

void *tide_list_at(const tide_list l, const int32_t i, const uint32_t size)
{
    tide_block *b = tide_block_at(l.at);
    if (!b || i < 0 || (uint32_t)i >= b->a) return NULL;
    return (char *)items(b) + (size_t)i * size;
}

// A new block for `capacity` elements where `l` lives: its world's heap, or the
// scratch area. Returns its tagged offset, or 0 when there's no room.
static uint32_t new_block(const tide_list *l, const uint32_t capacity, const uint32_t size)
{
    const uint64_t bytes = (uint64_t)capacity * size;
    if (bytes > TIDE_HEAP_BYTES) return 0;
    uint32_t where;
    tide_heap *heap = tide_heap_of(l, &where);
    uint32_t at = 0;
    tide_block *b;
    if (heap) {
        const uint32_t block = tide_heap_alloc(heap, (uint32_t)bytes);
        if (!block) return 0;
        b = tide_heap_block(heap, block);
        at = where << 30 | block;
    } else {
        b = tide_scratch_block((uint32_t)bytes, &at);
        if (!b) return 0;
    }
    b->a = 0;
    b->b = capacity;
    return at;
}

// Gives back a block that was `l`'s own: freed with the world's others, or
// left in the scratch area, which goes back in one piece.
static void drop_block(const tide_list *l, const uint32_t at)
{
    uint32_t where;
    tide_heap *heap = tide_heap_of(l, &where);
    if (heap && at && at >> 30 == where) tide_heap_release(heap, at & 0x3FFFFFFFu);
}

// Room for `more` elements: in the list's own block, grown if it's full, and
// moved into a block of its own if it borrowed one. False when there's no room.
static bool reserve(tide_list *l, const uint32_t more, const uint32_t size)
{
    tide_block *b = tide_block_at(l->at);
    const uint32_t count = b ? b->a : 0;
    uint32_t where = TIDE_IN_SCRATCH;
    const bool in_world = tide_heap_of(l, &where) != NULL;
    const bool own = !b || (in_world ? l->at >> 30 == where : l->at >> 30 == TIDE_IN_SCRATCH);
    if (b && own && count + more <= b->b) return true;
    uint32_t capacity = b && b->b ? b->b : 4u;
    while (capacity < count + more) capacity *= 2u;
    const uint32_t at = new_block(l, capacity, size);
    if (!at) return false;
    tide_block *grown = tide_block_at(at);
    if (b) memcpy(items(grown), items(b), (size_t)count * size);
    grown->a = count;
    if (own) drop_block(l, l->at);
    l->at = at;
    return true;
}

void *tide_list_add(tide_list *l, const uint32_t size)
{
    return tide_list_insert(l, tide_list_count(*l), size);
}

void *tide_list_insert(tide_list *l, int32_t i, const uint32_t size)
{
    if (!reserve(l, 1, size)) return NULL;
    tide_block *b = tide_block_at(l->at);
    const int32_t count = (int32_t)b->a;
    if (i < 0) i = 0;
    if (i > count) i = count;
    char *at = (char *)items(b) + (size_t)i * size;
    memmove(at + size, at, (size_t)(count - i) * size);
    memset(at, 0, size);
    b->a++;
    return at;
}

void tide_list_remove_at(tide_list *l, const int32_t i, const uint32_t size)
{
    tide_block *b = tide_block_at(l->at);
    if (!b || i < 0 || (uint32_t)i >= b->a) return;
    char *at = (char *)items(b) + (size_t)i * size;
    memmove(at, at + size, (size_t)(b->a - (uint32_t)i - 1u) * size);
    b->a--;
    memset((char *)items(b) + (size_t)b->a * size, 0, size); // Freed space is zero, as in the heap
}

void tide_list_clear(tide_list *l, const uint32_t size)
{
    tide_block *b = tide_block_at(l->at);
    if (!b) return;
    // It keeps its block, zeroed, for what's added next.
    memset(items(b), 0, (size_t)b->a * size);
    b->a = 0;
}

tide_list tide_list_copy(const tide_list l, const uint32_t size)
{
    const int32_t count = tide_list_count(l);
    return tide_list_from(count ? items(tide_block_at(l.at)) : NULL, count, size);
}

tide_list tide_list_from(const void *elements, const int32_t count, const uint32_t size)
{
    tide_list out = {0};
    if (count <= 0) return out;
    uint32_t at;
    tide_block *b = tide_scratch_block((uint32_t)count * size, &at);
    if (!b) return out;
    memcpy(items(b), elements, (size_t)count * size);
    b->a = (uint32_t)count;
    b->b = (uint32_t)count;
    out.at = at;
    return out;
}

void tide_list_set(tide_list *to, const tide_list value, const uint32_t size)
{
    uint32_t where;
    if (!tide_heap_of(to, &where)) {
        *to = value;
        return;
    }
    const int32_t count = tide_list_count(value);
    const tide_block *from = tide_block_at(value.at);
    tide_list_clear(to, size);
    if (count == 0 || !reserve(to, (uint32_t)count, size)) return;
    tide_block *b = tide_block_at(to->at);
    memmove(items(b), items((tide_block *)(uintptr_t)from), (size_t)count * size);
    b->a = (uint32_t)count;
}

void tide_list_own(tide_list *l, const uint32_t size)
{
    uint32_t where;
    if (!tide_heap_of(l, &where) || !l->at) return;
    const tide_list borrowed = *l;
    l->at = 0;
    tide_list_set(l, borrowed, size);
}

void tide_list_release(tide_list *l)
{
    drop_block(l, l->at);
    l->at = 0;
}
