#pragma once

#include <stdint.h>

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
    // The page each page-sized piece of the heap is in. A page bigger than
    // that is at each of its places, with a reference for each.
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

// Gives a block back. It's freed at the next tide_heap_flush.
void tide_heap_release(tide_heap *h, uint32_t block);

// Frees the released blocks, zeroed, for reuse. Generated code calls it once
// the code that released them is done.
void tide_heap_flush(tide_heap *h);

// A block, to read.
static inline tide_block *tide_heap_block(const tide_heap *h, const uint32_t block)
{
    const tide_page *p = h->page[block >> TIDE_HEAP_PAGE_SHIFT];
    return (tide_block *)(uintptr_t)((const uint8_t *)tide_page_data(p) + (block - (p->first << TIDE_HEAP_PAGE_SHIFT)));
}

// A block, to change: its page made this heap's own.
tide_block *tide_heap_write(tide_heap *h, uint32_t block);
