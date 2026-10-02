#pragma once

#include <stddef.h>
#include <stdint.h>

// Storage of each thread's own, for state code on several threads keeps
// apart (the scratch area, the heaps code runs with, the task running).
#if defined(__wasm__) && !defined(__wasm_atomics__)
#define TIDE_THREAD_LOCAL // Web builds without threads
#else
#define TIDE_THREAD_LOCAL _Thread_local
#endif

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

// malloc, calloc and realloc (which takes the old block's size), for memory
// code on several threads uses: running out ends the program. On the web,
// they make sure this thread can use the memory they give (tide_memory_sync)
// before anything touches it, which calloc and realloc can't.
void *tide_alloc(size_t size);
void *tide_alloc_zeroed(size_t count, size_t size);
void *tide_realloc(void *p, size_t old_size, size_t size);

// On the web with threads: when another thread grew the program's memory, V8
// (Node 24's, at least) lets this one use what was added only once it hears
// of it, which can come after it got memory there from malloc or from another
// thread. memcpy and memset into it trap until then. This brings it up to
// date, and costs a comparison when it is. Code that picks up what other
// threads made calls it first (tide/jobs.h's tasks and the systems' ends).
#if defined(__wasm__) && defined(__wasm_atomics__)
void tide_memory_sync(void);
#else
static inline void tide_memory_sync(void)
{
}
#endif

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

// A copy of `p`'s first `keep` bytes (the rest zero), with `refs` references
// and its hash out of date. `p` keeps its own.
tide_page *tide_page_copy(const tide_page *p, uint32_t refs, uint32_t keep);

// The hash of its first `bytes` bytes, kept until it changes.
uint64_t tide_page_hash(tide_page *p, uint32_t bytes);
