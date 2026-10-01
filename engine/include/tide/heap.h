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
// tide_heap_flush), so text read before the release stays readable until then.

#ifndef TIDE_HEAP_BYTES
#define TIDE_HEAP_BYTES (256u * 1024u)
#endif

#define TIDE_HEAP_CLASSES 24 // Block sizes 16 << class bytes

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
    uint32_t failed;                  // Allocations that didn't fit, for debugging
    uint8_t bytes[TIDE_HEAP_BYTES];
} tide_heap;

// A block with room for `bytes` bytes after its header, or 0 when the heap is
// full. Offsets are from `bytes` and never 0. A zeroed heap is empty.
uint32_t tide_heap_alloc(tide_heap *h, uint32_t bytes);

// For snapshots: copies what's been handed out, and clears what `to` had
// beyond it, so past `used` a heap is all zeros. The hash covers the same.
void tide_heap_copy(tide_heap *to, const tide_heap *from);
uint64_t tide_heap_hash(uint64_t h, const tide_heap *heap);

// Gives a block back. It's freed at the next tide_heap_flush.
void tide_heap_release(tide_heap *h, uint32_t block);

// Frees the released blocks, zeroed, for reuse. Generated code calls it once
// the code that released them is done.
void tide_heap_flush(tide_heap *h);

static inline tide_block *tide_heap_block(const tide_heap *h, const uint32_t block)
{
    return (tide_block *)(uintptr_t)(h->bytes + block);
}
