#pragma once

#include <stdint.h>

#include "tide/delta.h"
#include "tide/net.h"
#include "tide/page.h"

// Memory a world keeps variable-length data in, such as the text of its
// components' string fields. It's part of the world, in pages its snapshots
// share (see tide/page.h), and it grows as it needs, with no limit but memory.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// Deterministic: blocks come from size classes, reused in a fixed order, so
// the same allocations and releases give the same offsets on every machine.
// A released block is only reused once the code running now is done (see
// tide_heap_flush), so text read before the release stays readable until then.
//
// One system at a time changes a world's heap, while others may read it on
// other threads: the heap's page table is read as a whole, and when it grows,
// the old one stays until tide_heap_settle, once only one thread runs.

#define TIDE_HEAP_CLASSES 24 // Block sizes 16 << class bytes

// Bytes in each page: 1 << this. A block never crosses from one page into
// the next: a bigger block has a page of its own, as big as it needs.
#define TIDE_HEAP_PAGE_SHIFT 14u

// Every block starts with its header: what it holds after it.
typedef struct tide_block {
    uint32_t size_class;
    uint32_t next; // In a free list or the pending list
    uint32_t a;    // Text: its bytes
    uint32_t b;    // Text: its characters
} tide_block;

typedef struct tide_heap {
    uint32_t used;                    // Bytes handed out from the start, headers included
    uint32_t free[TIDE_HEAP_CLASSES]; // Released blocks of each class, most recent first
    uint32_t pending;                 // Blocks released while code runs, freed when it's done
    uint32_t pages;                   // Pages as far as `used` goes
    uint32_t room;                    // Pages `page` has room for
    // The tick its world is running, which grids mark the chunks they change
    // with (see tide/grid.h). Set as each tick starts; it's no state of its own.
    uint32_t tick;
    // Counts the times a block may have moved or gone: a page copied to be
    // changed, or blocks released. Code that keeps a block's address a while
    // (a grid's chunk cache, see tide/grid.h) checks it didn't change. It's
    // no state of its own either, and read atomically, as other threads may
    // read it while one system changes the heap.
    uint32_t moves;
    // The page each page-sized piece of the heap is in. A page bigger than
    // that is at each of its places, with a reference for each. Before the
    // first, page[-1] holds the table this one replaced, until it's settled.
    tide_page **page;
} tide_heap;

// A block with room for `bytes` bytes after its header. Offsets are never 0.
// A zeroed heap is empty.
uint32_t tide_heap_alloc(tide_heap *h, uint32_t bytes);

// For snapshots: `to` becomes `from`, sharing its pages. The hash covers
// what's been handed out.
void tide_heap_copy(tide_heap *to, const tide_heap *from);
uint64_t tide_heap_hash(uint64_t h, const tide_heap *heap);

// Lets go of its pages, leaving it empty.
void tide_heap_free(tide_heap *h);

// The heap as bytes, for sending a world and carrying it over, and reading
// it back into an empty heap (false when the bytes aren't one).
uint32_t tide_heap_packed_size(const tide_heap *h);
void tide_heap_pack(const tide_heap *h, tide_writer *w);
bool tide_heap_unpack(tide_heap *h, tide_reader *r);

// The same as part of a delta (tide/delta.h), from `base` (NULL for none):
// what it's handed out, how many places each page takes, then its pages as
// regions, each paired with the base's page that starts at the same place.
// Unpacking `h` (empty) shares the pages that are the same as the base's.
void tide_heap_pack_delta(const tide_heap *h, const tide_heap *base, tide_delta_writer *d);
bool tide_heap_unpack_delta(tide_heap *h, const tide_heap *base, tide_delta_reader *d);
// Its pages' places and hashes, and which of them `base` lacks (tide_needs).
void tide_heap_hash_pages(const tide_heap *h, tide_writer *w);
void tide_heap_need_pages(const tide_heap *base, tide_needs *n);

// Gives a block back. It's freed at the next tide_heap_flush.
void tide_heap_release(tide_heap *h, uint32_t block);

// Frees the released blocks, zeroed, for reuse. Generated code calls it once
// the code that released them is done.
void tide_heap_flush(tide_heap *h);

// Frees the page tables the heap grew out of, which code on other threads may
// still have been reading. Generated code calls it once systems are done.
void tide_heap_settle(tide_heap *h);

// A block, to read. Its page is read as one, since the system changing the
// heap may be growing it or copying a page meanwhile.
static inline tide_block *tide_heap_block(const tide_heap *h, const uint32_t block)
{
    tide_page *const *pages = __atomic_load_n(&h->page, __ATOMIC_ACQUIRE);
    const tide_page *p = __atomic_load_n(&pages[block >> TIDE_HEAP_PAGE_SHIFT], __ATOMIC_ACQUIRE);
    return (tide_block *)(uintptr_t)((const uint8_t *)tide_page_data(p) + (block - (p->first << TIDE_HEAP_PAGE_SHIFT)));
}

// A block, to change: its page made this heap's own. Pages this heap owns
// already are changed in place, and the rest are copied first
// (tide_heap_write_shared).
tide_block *tide_heap_write_shared(tide_heap *h, uint32_t block);

static inline tide_block *tide_heap_write(tide_heap *h, const uint32_t block)
{
    tide_page *p = h->page[block >> TIDE_HEAP_PAGE_SHIFT];
    // A page bigger than one place has a reference for each (see tide_heap)
    const uint32_t places = p->size > (1u << TIDE_HEAP_PAGE_SHIFT) ? p->size >> TIDE_HEAP_PAGE_SHIFT : 1u;
    if (p->refs != places) return tide_heap_write_shared(h, block);
    // What it hashed to is out of date. Atomic, as a grid's chunk tasks can
    // change one page's blocks on several threads (see tide/grid.h).
    __atomic_store_n(&p->hashed, UINT32_MAX, __ATOMIC_RELAXED);
    return (tide_block *)(uintptr_t)((uint8_t *)tide_page_data(p) + (block - (p->first << TIDE_HEAP_PAGE_SHIFT)));
}

// A block of a page or less to change, on one of the threads that change the
// heap's blocks at once, each its own (a grid's chunk tasks), but whose blocks
// can share a page. The first to change a shared page puts its copy in the
// page's place, and the others change that one. The page it copied keeps this
// heap's reference, as other threads may still be reading it or about to
// change it: it's returned in `*copied` (NULL for none), for
// tide_page_release(copied, 1) once they're all done.
tide_block *tide_heap_write_parallel(tide_heap *h, uint32_t block, tide_page **copied);
