#include "tide/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/net.h"

#if defined(__wasm__) && defined(__wasm_atomics__)
#include <errno.h>
#endif

// See tide/page.h.

_Noreturn void tide_out_of_memory(void)
{
    fprintf(stderr, "tide: out of memory\n");
    abort();
}

#if defined(__wasm__) && defined(__wasm_atomics__)

#define WASM_PAGE 65536u

// The program's memory, in WebAssembly's pages, as far as any thread grew it.
static uint32_t grown;

// The C library's sbrk, which its malloc grows the memory with: the same, and
// it notes how far, for tide_memory_sync.
void *sbrk(const intptr_t increment)
{
    if (increment == 0) return (void *)((uintptr_t)__builtin_wasm_memory_size(0) * WASM_PAGE);
    if (increment < 0 || (uintptr_t)increment % WASM_PAGE != 0) abort(); // As the C library's does
    const size_t old = __builtin_wasm_memory_grow(0, (size_t)increment / WASM_PAGE);
    if (old == SIZE_MAX) {
        errno = ENOMEM;
        return (void *)-1;
    }
    const uint32_t now = (uint32_t)__builtin_wasm_memory_size(0);
    uint32_t seen = __atomic_load_n(&grown, __ATOMIC_RELAXED);
    while (seen < now && !__atomic_compare_exchange_n(&grown, &seen, now, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
    return (void *)(old * WASM_PAGE);
}

void tide_memory_sync(void)
{
    // Growing by nothing brings this thread's size up to date
    if (__builtin_wasm_memory_size(0) < __atomic_load_n(&grown, __ATOMIC_ACQUIRE)) __builtin_wasm_memory_grow(0, 0);
}

void *tide_alloc(const size_t size)
{
    void *p = malloc(size ? size : 1u);
    if (!p) tide_out_of_memory();
    tide_memory_sync(); // Before anything touches it
    return p;
}

void *tide_alloc_zeroed(const size_t count, const size_t size)
{
    if (size && count > SIZE_MAX / size) tide_out_of_memory();
    void *p = tide_alloc(count * size); // Not calloc, which clears it before this thread can see it
    memset(p, 0, count * size);
    return p;
}

void *tide_realloc(void *p, const size_t old_size, const size_t size)
{
    // Not realloc, which can copy into memory this thread can't see yet
    void *grown_to = tide_alloc(size);
    if (p) memcpy(grown_to, p, old_size < size ? old_size : size);
    free(p);
    return grown_to;
}

#else

void *tide_alloc(const size_t size)
{
    void *p = malloc(size ? size : 1u);
    if (!p) tide_out_of_memory();
    return p;
}

void *tide_alloc_zeroed(const size_t count, const size_t size)
{
    void *p = calloc(count ? count : 1u, size ? size : 1u);
    if (!p) tide_out_of_memory();
    return p;
}

void *tide_realloc(void *p, const size_t old_size, const size_t size)
{
    (void)old_size;
    void *grown_to = realloc(p, size ? size : 1u);
    if (!grown_to) tide_out_of_memory();
    return grown_to;
}

#endif

tide_page *tide_page_new(const uint32_t size)
{
    tide_page *p = tide_alloc_zeroed(1, sizeof(tide_page) + size);
    p->refs = 1;
    p->size = size;
    p->hashed = UINT32_MAX;
    return p;
}

void tide_page_release(tide_page *p, const uint32_t refs)
{
    if (!p) return;
    p->refs -= refs;
    if (p->refs == 0) free(p);
}

tide_page *tide_page_copy(const tide_page *p, const uint32_t refs, const uint32_t keep)
{
    tide_page *copy = tide_alloc(sizeof *copy + p->size);
    const uint32_t kept = keep < p->size ? keep : p->size;
    *copy = (tide_page){refs, p->size, p->first, UINT32_MAX, 0, 0};
    memcpy(tide_page_data(copy), tide_page_data(p), kept);
    memset((uint8_t *)tide_page_data(copy) + kept, 0, p->size - kept);
    return copy;
}

tide_page *tide_page_own_copy(tide_page *p, const uint32_t mine, const uint32_t keep)
{
    tide_page *copy = tide_page_copy(p, mine, keep);
    p->refs -= mine;
    return copy;
}

uint64_t tide_page_hash(tide_page *p, const uint32_t bytes)
{
    if (p->hashed != bytes) {
        p->hash = tide_hash(tide_page_data(p), bytes);
        p->hashed = bytes;
    }
    return p->hash;
}
