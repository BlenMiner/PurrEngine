#pragma once

#include <stdint.h>

// Memory a world keeps variable-length data in, such as the text of its
// components' string fields. It's part of the world, so a snapshot copies it,
// and it has a fixed size, so every machine runs out at the same point.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// Deterministic: blocks come from size classes, reused in a fixed order, so
// the same allocations and releases give the same offsets on every machine.
// A released block is only reused once the code running now is done (see
// purr_heap_flush), so text read before the release stays readable until then.

#ifndef PURR_HEAP_BYTES
#define PURR_HEAP_BYTES (256u * 1024u)
#endif

#define PURR_HEAP_CLASSES 24 // Block sizes 16 << class bytes

// Every block starts with its header: what it holds after it.
typedef struct purr_block {
    uint32_t size_class;
    uint32_t next; // In a free list or the pending list
    uint32_t a;    // Text: its bytes
    uint32_t b;    // Text: its characters
} purr_block;

typedef struct purr_heap {
    uint32_t used;                    // Bytes handed out from the start, headers included
    uint32_t free[PURR_HEAP_CLASSES]; // Released blocks of each class, most recent first
    uint32_t pending;                 // Blocks released while code runs, freed when it's done
    uint32_t failed;                  // Allocations that didn't fit, for debugging
    uint8_t bytes[PURR_HEAP_BYTES];
} purr_heap;

// A block with room for `bytes` bytes after its header, or 0 when the heap is
// full. Offsets are from `bytes` and never 0. A zeroed heap is empty.
uint32_t purr_heap_alloc(purr_heap *h, uint32_t bytes);

// For snapshots: copies what's been handed out, and clears what `to` had
// beyond it, so past `used` a heap is all zeros. The hash covers the same.
void purr_heap_copy(purr_heap *to, const purr_heap *from);
uint64_t purr_heap_hash(uint64_t h, const purr_heap *heap);

// Gives a block back. It's freed at the next purr_heap_flush.
void purr_heap_release(purr_heap *h, uint32_t block);

// Frees the released blocks, zeroed, for reuse. Generated code calls it once
// the code that released them is done.
void purr_heap_flush(purr_heap *h);

static inline purr_block *purr_heap_block(const purr_heap *h, const uint32_t block)
{
    return (purr_block *)(uintptr_t)(h->bytes + block);
}
