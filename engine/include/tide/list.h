#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/heap.h"
#include "tide/text.h"

// Tide's `List<T>`: a list of values, as a value. Generated code calls the
// functions below with each element's size, and handles the text in elements
// itself (see tide/text.h).
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A tide_list is where its elements are: a block in a world's heap, or in the
// scratch area. As with text, generated code says where the list is with
// `where` (see tide/text.h): a list that's part of a world (a component's
// field, a singleton's, the command queue's) has its own block in that
// world's heap, and any other list has one in the scratch area. A list is
// never shared: taking one out of a field or a variable copies it
// (tide_list_copy), so changing a list in place is safe.
//
// Nothing fails: past the end, reads find nothing and writes do nothing. A
// world's lists grow with its heap, and a list in the scratch area with the
// area, which adds pieces as it needs (see tide/text.h); only running out of
// memory ends the program.
//
// A list's block starts with a header: the count in `a`, and the capacity in
// `b`. The elements follow it, side by side, unless the list is a world's and
// they take more than a page: then they're in chunks, heap blocks of a page
// each, and the list's block lists where each chunk is, `b` saying how many
// there are (with TIDE_LIST_CHUNKED). A world and its snapshots share pages
// until one of them changes one (see tide/page.h), and hashes cover each page
// on its own, so changing an element of a big list copies and hashes its
// chunk, not the whole list. Growing adds chunks, without moving elements.

typedef struct tide_list {
    uint32_t at; // 0: empty. Otherwise its block's offset, and where in the top two bits
} tide_list;

// Bytes of elements in a chunk: a heap block of a page, less its header.
#define TIDE_LIST_CHUNK ((1u << TIDE_HEAP_PAGE_SHIFT) - (uint32_t)sizeof(tide_block))
// In a list's header's `b`: the elements are in chunks, as many as the rest says.
#define TIDE_LIST_CHUNKED 0x80000000u

// Reading and writing elements is inline, as code does it for every element
// it touches.

static inline int32_t tide_list_count(const tide_list l)
{
    const tide_block *b = tide_block_at(l.at);
    return b ? (int32_t)b->a : 0;
}

// Element `i` of a list in chunks: its chunk (a heap offset), and where in
// its chunk it is.
static inline uint32_t tide_list_chunk(const tide_block *b, const uint32_t i, const uint32_t size, uint32_t *place)
{
    const uint32_t per = TIDE_LIST_CHUNK / size;
    *place = i % per * size;
    return ((const uint32_t *)(b + 1))[i / per];
}

// The element at `i`, or NULL past the end. tide_list_at is to read it, and
// tide_list_at_mut to change it in place (a world's page is made its own,
// apart from its snapshots: see tide/page.h). It moves when a list with its
// elements side by side grows; one in chunks keeps them where they are.
//
// A world's list is found through its heap's page table, which other threads
// may be reading meanwhile (see tide/heap.h), once for its block and its
// chunk alike. A chunk is a page of its own, so its elements start right
// after the header at the page's start.
static inline void *tide_list_at(const tide_list l, const int32_t i, const uint32_t size)
{
    if (!l.at) return NULL;
    const uint32_t offset = l.at & 0x3FFFFFFFu;
    if (l.at >> 30 == TIDE_IN_SCRATCH) {
        tide_block *b = (tide_block *)(uintptr_t)tide_scratch_at(offset);
        return i >= 0 && (uint32_t)i < b->a ? (char *)(b + 1) + (size_t)i * size : NULL;
    }
    const tide_heap *heap = tide_heap_of(l.at >> 30);
    if (!heap) return NULL;
    tide_page *const *pages = __atomic_load_n(&heap->page, __ATOMIC_ACQUIRE);
    const tide_page *p = __atomic_load_n(&pages[offset >> TIDE_HEAP_PAGE_SHIFT], __ATOMIC_ACQUIRE);
    tide_block *b = (tide_block *)(uintptr_t)((const uint8_t *)tide_page_data(p) + (offset - (p->first << TIDE_HEAP_PAGE_SHIFT)));
    if (i < 0 || (uint32_t)i >= b->a) return NULL;
    if (!(b->b & TIDE_LIST_CHUNKED)) return (char *)(b + 1) + (size_t)i * size;
    uint32_t place;
    const uint32_t chunk = tide_list_chunk(b, (uint32_t)i, size, &place);
    const tide_page *c = __atomic_load_n(&pages[chunk >> TIDE_HEAP_PAGE_SHIFT], __ATOMIC_ACQUIRE);
    return (char *)tide_page_data(c) + sizeof(tide_block) + place;
}

// Only the code changing a heap writes it, so it reads the page table as it
// left it (see tide_heap_write).
static inline void *tide_list_at_mut(const tide_list l, const int32_t i, const uint32_t size)
{
    if (!l.at) return NULL;
    const uint32_t offset = l.at & 0x3FFFFFFFu;
    if (l.at >> 30 == TIDE_IN_SCRATCH) {
        tide_block *b = (tide_block *)(uintptr_t)tide_scratch_at(offset);
        return i >= 0 && (uint32_t)i < b->a ? (char *)(b + 1) + (size_t)i * size : NULL;
    }
    tide_heap *heap = tide_heap_of(l.at >> 30);
    if (!heap) return NULL;
    const tide_page *p = heap->page[offset >> TIDE_HEAP_PAGE_SHIFT];
    const tide_block *b = (const tide_block *)(uintptr_t)((const uint8_t *)tide_page_data(p) + (offset - (p->first << TIDE_HEAP_PAGE_SHIFT)));
    if (i < 0 || (uint32_t)i >= b->a) return NULL;
    if (!(b->b & TIDE_LIST_CHUNKED)) return (char *)(tide_heap_write(heap, offset) + 1) + (size_t)i * size;
    uint32_t place;
    const uint32_t chunk = tide_list_chunk(b, (uint32_t)i, size, &place);
    return (char *)(tide_heap_write(heap, chunk) + 1) + place;
}

// The run of elements code last read from a world's list, so a loop over its
// elements finds each chunk once rather than at every element: generated code
// keeps one for each list type in each function. It holds while the list's
// heap hasn't moved or released a block since (tide_heap.moves), and only for
// elements below the list's count as it is then, so whatever the code does to
// the list meanwhile, it never reads where it shouldn't.
typedef struct tide_list_cache {
    uint32_t at;              // The list (its tide_list.at), 0 for none
    uint32_t moves;           // Its heap's moves when it was looked up
    const tide_heap *heap;    // ...that heap
    const tide_block *header; // The list's block, whose `a` is its count
    uint32_t first;           // The run: elements from `first`, `count` of them, side by side from `elements`
    uint32_t count;
    const uint8_t *elements;
} tide_list_cache;

// tide_list_at, filling the cache with the run element `i` is in.
const void *tide_list_cache_fill(tide_list_cache *c, tide_list l, int32_t i, uint32_t size);

// tide_list_at, through a cache.
static inline const void *tide_list_cached_at(tide_list_cache *c, const tide_list l, const int32_t i, const uint32_t size)
{
    if (c->at == l.at && c->at && (uint32_t)i - c->first < c->count && (uint32_t)i < c->header->a
        && __atomic_load_n(&c->heap->moves, __ATOMIC_RELAXED) == c->moves) {
        return c->elements + (size_t)((uint32_t)i - c->first) * size;
    }
    return tide_list_cache_fill(c, l, i, size);
}

// A world's list, read from outside, like a host reading a component: its
// count, and its element at `i` (NULL past the end).
int32_t tide_list_read_count(const tide_heap *heap, tide_list l);
const void *tide_list_read(const tide_heap *heap, tide_list l, int32_t i, uint32_t size);

// A new element at the end, zeroed, or NULL when the list is as big as one
// gets (1 GiB of elements: what a block's offsets reach).
void *tide_list_add(tide_list *l, uint32_t size, uint32_t where);

// A new element at `i` (clamped to the list), zeroed, the ones after it moved
// along; or NULL when the list is as big as one gets.
void *tide_list_insert(tide_list *l, int32_t i, uint32_t size, uint32_t where);

// Removes the element at `i`, moving the ones after it back. Past the end, it
// does nothing. Release its text first.
void tide_list_remove_at(tide_list *l, int32_t i, uint32_t size);

// Removes every element. Release their text first.
void tide_list_clear(tide_list *l, uint32_t size);

// A copy in the scratch area: a list taken out of a field or variable.
tide_list tide_list_copy(tide_list l, uint32_t size);

// A list in the scratch area of `count` elements: [a, b, c].
tide_list tide_list_from(const void *items, int32_t count, uint32_t size);

// Assigning: `value`'s elements, a copy if `to` is part of a world, or `value`
// itself otherwise. Release the old elements' text first, and own the new ones'
// after.
void tide_list_set(tide_list *to, tide_list value, uint32_t size, uint32_t where);

// A list just copied into a world, like a spawn's component into the command
// queue: its elements in a block of the world's own. Own their text after.
void tide_list_own(tide_list *l, uint32_t size, uint32_t where);

// A world's list leaving it. Release its elements' text first.
void tide_list_release(tide_list *l, uint32_t where);

// For C, which takes a list as a pointer to its elements side by side (NULL
// for none): the list's own elements when they are, made its world's own when
// C `changes` them, or else a copy, which tide_list_unflatten lets go of,
// first writing it back into the list when C changes it. A copy is in memory
// of its own, as big as the list, rather than the scratch area. `copied` says
// which it was.
void *tide_list_flatten(tide_list l, uint32_t size, bool changes, bool *copied);
void tide_list_unflatten(tide_list l, void *flat, uint32_t size, bool changes, bool copied);

// ---------------------------------------------------------------------------
// Parallel loops over a list's elements, by index: `parallel (var i in items)`
// and `parallel (var i in items by 2 offset o)` (see docs/spec.md, Lists).
// Their steps run at once, on threads (tide_parallel_for): each reads the list
// as the loop found it, and writes only its own element or block, into its
// task's copy of its blocks' elements. The loop's end puts what changed into
// the list, chunk by chunk, so the result is the same however the steps were
// shared out, and a chunk nothing changed stays as it was. A block that would
// go past the list's end is left out.

typedef struct tide_par_list {
    tide_list *list;
    uint32_t size;      // An element's bytes
    int32_t block;      // Each step's block, in elements
    int32_t offset;     // Where the blocks start
    int32_t lo, hi;     // Blocks within the list, by index: from lo up to hi
    int32_t per_task;   // Blocks in a task; the last has what's left
    uint32_t tasks;
    uint8_t **copies;   // Each task's copy of its blocks' elements, once it's run
    uint64_t steps;     // Blocks in all: how much work it is
} tide_par_list;

// Sets `l` up to go over a world's list `list` of elements of `size` bytes, in
// blocks of `block` elements starting at `offset`.
void tide_par_list_begin(tide_par_list *l, tide_list *list, uint32_t size, int32_t block, int32_t offset);

// Task `task`'s blocks, by index: from *from up to *to. Its copy of their
// elements, as the loop found them, which its steps write: the first is the
// element `offset + *from * block`.
void *tide_par_list_task(tide_par_list *l, uint32_t task, int32_t *from, int32_t *to);

// Puts what the steps changed into the list and lets the copies go.
void tide_par_list_end(tide_par_list *l);
