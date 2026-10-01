#pragma once

#include <stdint.h>

// Pages: the memory a world keeps its data in (its archetypes' columns, its
// entity table and its heap), shared with its snapshots until one of them
// changes it.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A snapshot (tide_world_copy) points at the world's pages rather than copying
// them. Whoever changes a shared page first gets a copy of their own
// (tide_page_own), so a world and its snapshots only cost the memory of what's
// different between them. A page also keeps the hash of its contents until it
// changes, so hashing a world only reads what changed since it was last
// hashed.
//
// Pages hold plain data, with no pointers, and the world and its snapshots
// are used from one thread at a time.

typedef struct tide_page {
    uint32_t refs;   // References to it: one from each world or snapshot that has it, or more (see tide_heap)
    uint32_t size;   // Bytes after the header
    uint32_t first;  // A heap's: where it starts, in pages
    uint32_t hashed; // The bytes `hash` covers, or UINT32_MAX until it's hashed again
    uint64_t hash;
    uint64_t unused; // The data after the header starts 16 bytes in, as malloc's does
} tide_page;

// Ends the program, saying it ran out of memory. Worlds grow as they need, so
// running out of memory is the only way they can't, and it can't happen the
// same way on every machine.
_Noreturn void tide_out_of_memory(void);

// A page of `size` bytes, zeroed, with one reference.
tide_page *tide_page_new(uint32_t size);

static inline void *tide_page_data(const tide_page *p)
{
    return (void *)(uintptr_t)(p + 1);
}

static inline void tide_page_retain(tide_page *p)
{
    p->refs++;
}

// Lets go of `refs` references; the last one frees it. NULL does nothing.
void tide_page_release(tide_page *p, uint32_t refs);

// `p` to change, through `mine` of its references: itself when nobody else
// has it, or else a copy of its first `keep` bytes (the rest zero), which
// takes those references over. Its hash is out of date either way.
tide_page *tide_page_own(tide_page *p, uint32_t mine, uint32_t keep);

// The hash of its first `bytes` bytes, kept until it changes.
uint64_t tide_page_hash(tide_page *p, uint32_t bytes);
