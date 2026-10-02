#include "tide/heap.h"

#include <stdlib.h>
#include <string.h>

// See tide/heap.h.

#define PAGE (1u << TIDE_HEAP_PAGE_SHIFT)
#define FIRST 16u          // Offset 0 means no block, so blocks start after it
#define LIMIT 0x40000000u  // Offsets have 30 bits: the top two say whose heap a block is in (see tide/text.h)

// The references a heap has to `p`: one for each of its places.
static uint32_t places(const tide_page *p)
{
    return p->size > PAGE ? p->size >> TIDE_HEAP_PAGE_SHIFT : 1u;
}

// Bytes of `p` in use: as far as `used` goes.
static uint32_t in_use(const tide_heap *h, const tide_page *p)
{
    const uint32_t left = h->used - (p->first << TIDE_HEAP_PAGE_SHIFT);
    return left < p->size ? left : p->size;
}

// A bigger page table, which takes the old one's place as a whole. The old
// one stays, linked from page[-1], for code on other threads still reading it,
// until tide_heap_settle.
static void make_room(tide_heap *h, const uint32_t pages)
{
    if (pages <= h->room) return;
    uint32_t room = h->room ? h->room : 4u;
    while (room < pages) room *= 2u;
    tide_page **base = tide_alloc((room + 1u) * sizeof *base);
    tide_page **grown = base + 1;
    if (h->pages) memcpy(grown, h->page, h->pages * sizeof *grown);
    base[0] = h->page ? (tide_page *)(void *)(h->page - 1) : NULL;
    __atomic_store_n(&h->page, grown, __ATOMIC_RELEASE);
    h->room = room;
}

void tide_heap_settle(tide_heap *h)
{
    if (!h->page) return;
    void *old = h->page[-1];
    while (old) {
        tide_page **base = old;
        old = base[0];
        free(base);
    }
    h->page[-1] = NULL;
}

// A page for the heap's next `count` places.
static void add_page(tide_heap *h, const uint32_t count)
{
    make_room(h, h->pages + count);
    tide_page *p = tide_page_new(count << TIDE_HEAP_PAGE_SHIFT);
    p->first = h->pages;
    p->refs = count;
    for (uint32_t k = 0; k < count; k++) h->page[h->pages + k] = p;
    h->pages += count;
}

tide_block *tide_heap_write_shared(tide_heap *h, const uint32_t block)
{
    tide_page *p = h->page[block >> TIDE_HEAP_PAGE_SHIFT];
    tide_page *own = tide_page_own(p, places(p), in_use(h, p));
    if (own != p) { // Code reading the old page meanwhile reads the same bytes
        for (uint32_t k = 0; k < places(own); k++) __atomic_store_n(&h->page[own->first + k], own, __ATOMIC_RELEASE);
        __atomic_add_fetch(&h->moves, 1u, __ATOMIC_RELAXED);
    }
    return tide_heap_block(h, block);
}

// A chunk task's copy leaves `moves` alone: the threads that change blocks
// at once only change their own chunks, and code that keeps an address meanwhile
// (on another thread) only reads, and reads the same bytes in the old page.
tide_block *tide_heap_write_parallel(tide_heap *h, const uint32_t block, tide_page **copied)
{
    *copied = NULL;
    tide_page **slot = &h->page[block >> TIDE_HEAP_PAGE_SHIFT];
    tide_page *p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    // A page replaced meanwhile still has this heap's reference, so it reads as shared
    if (__atomic_load_n(&p->refs, __ATOMIC_RELAXED) != 1u) {
        tide_page *own = tide_page_copy(p, 1u, in_use(h, p));
        if (__atomic_compare_exchange_n(slot, &p, own, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *copied = p;
            p = own;
        } else {
            free(own); // Another thread's copy got there first: p is that one now
            tide_memory_sync(); // ...which this one may not see yet (see tide/page.h)
        }
    }
    __atomic_store_n(&p->hashed, UINT32_MAX, __ATOMIC_RELAXED);
    return (tide_block *)(uintptr_t)((uint8_t *)tide_page_data(p) + (block - (p->first << TIDE_HEAP_PAGE_SHIFT)));
}

uint32_t tide_heap_alloc(tide_heap *h, const uint32_t bytes)
{
    const uint64_t size = (uint64_t)sizeof(tide_block) + bytes;
    uint32_t c = 0;
    while (c + 1 < TIDE_HEAP_CLASSES && ((uint64_t)16 << c) < size) c++;
    const uint64_t block_size = (uint64_t)16 << c;
    if (block_size < size) tide_out_of_memory(); // Bigger than any block
    if (h->free[c]) {
        const uint32_t block = h->free[c];
        tide_block *b = tide_heap_write(h, block);
        h->free[c] = b->next;
        b->next = 0;
        return block;
    }
    if (h->used < FIRST) h->used = FIRST;
    // A block that would cross into the next page starts there instead
    uint32_t at = h->used;
    if ((at & (PAGE - 1u)) + block_size > PAGE) at = (at + PAGE - 1u) & ~(PAGE - 1u);
    if ((uint64_t)at + block_size > LIMIT) tide_out_of_memory();
    if (at >> TIDE_HEAP_PAGE_SHIFT == h->pages) {
        add_page(h, block_size > PAGE ? (uint32_t)(block_size >> TIDE_HEAP_PAGE_SHIFT) : 1u);
    }
    h->used = at + (uint32_t)block_size;
    tide_block *b = tide_heap_write(h, at);
    b->size_class = c;
    b->next = 0;
    return at;
}

void tide_heap_release(tide_heap *h, const uint32_t block)
{
    if (!block) return;
    tide_heap_write(h, block)->next = h->pending;
    h->pending = block;
    __atomic_add_fetch(&h->moves, 1u, __ATOMIC_RELAXED);
}

void tide_heap_flush(tide_heap *h)
{
    while (h->pending) {
        const uint32_t block = h->pending;
        tide_block *b = tide_heap_write(h, block);
        h->pending = b->next;
        const uint32_t c = b->size_class;
        memset(b, 0, (size_t)16u << c); // Freed memory is zero, so it hashes the same everywhere
        b->size_class = c;
        b->next = h->free[c];
        h->free[c] = block;
    }
}

void tide_heap_copy(tide_heap *to, const tide_heap *from)
{
    if (to == from) return;
    // Its pages first, then letting go of to's: they can be the same ones.
    for (uint32_t i = 0; i < from->pages; i++) tide_page_retain(from->page[i]);
    for (uint32_t i = 0; i < to->pages; i++) tide_page_release(to->page[i], 1);
    make_room(to, from->pages);
    tide_heap_settle(to); // Snapshots are taken while nothing else runs on it
    if (from->pages) memcpy(to->page, from->page, from->pages * sizeof *to->page);
    to->pages = from->pages;
    to->used = from->used;
    memcpy(to->free, from->free, sizeof to->free);
    to->pending = from->pending;
}

uint64_t tide_heap_hash(uint64_t h, const tide_heap *heap)
{
    h = tide_hash_more(h, &heap->used, sizeof heap->used);
    h = tide_hash_more(h, heap->free, sizeof heap->free);
    h = tide_hash_more(h, &heap->pending, sizeof heap->pending);
    for (uint32_t i = 0; i < heap->pages; i += places(heap->page[i])) {
        const uint64_t page = tide_page_hash(heap->page[i], in_use(heap, heap->page[i]));
        h = tide_hash_more(h, &page, sizeof page);
    }
    return h;
}

void tide_heap_free(tide_heap *h)
{
    for (uint32_t i = 0; i < h->pages; i++) tide_page_release(h->page[i], 1);
    tide_heap_settle(h);
    if (h->page) free(h->page - 1);
    memset(h, 0, sizeof *h);
}

// Packed: what it's handed out, then each of its pages: how many places it
// takes and the bytes of it in use.

uint32_t tide_heap_packed_size(const tide_heap *h)
{
    uint32_t size = (uint32_t)(sizeof h->used + sizeof h->free + sizeof h->pending);
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) size += 4u + in_use(h, h->page[i]);
    return size;
}

void tide_heap_pack(const tide_heap *h, tide_writer *w)
{
    tide_write_u32(w, h->used);
    for (uint32_t c = 0; c < TIDE_HEAP_CLASSES; c++) tide_write_u32(w, h->free[c]);
    tide_write_u32(w, h->pending);
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) {
        const tide_page *p = h->page[i];
        tide_write_u32(w, places(p));
        tide_write_bytes(w, tide_page_data(p), in_use(h, p));
    }
}

// What it's handed out holds together: true of any heap, so one that came as
// bytes isn't one without it.
static bool numbers_hold(const tide_heap *h)
{
    if (h->used > LIMIT || (h->used && h->used < FIRST)) return false;
    for (uint32_t c = 0; c < TIDE_HEAP_CLASSES; c++) {
        if (h->free[c] && (h->free[c] < FIRST || h->free[c] >= h->used)) return false;
    }
    return !(h->pending && (h->pending < FIRST || h->pending >= h->used));
}

// Whether a page `pages` places in can take `count` of them: one place, or a
// block's own, a power of two of them, within the heap's limit.
static bool places_hold(const uint32_t pages, const uint32_t count)
{
    return count != 0 && !(count & (count - 1u)) && count <= (LIMIT >> TIDE_HEAP_PAGE_SHIFT) - pages;
}

bool tide_heap_unpack(tide_heap *h, tide_reader *r)
{
    h->used = tide_read_u32(r);
    for (uint32_t c = 0; c < TIDE_HEAP_CLASSES; c++) h->free[c] = tide_read_u32(r);
    h->pending = tide_read_u32(r);
    if (!numbers_hold(h)) r->failed = true;
    while (!r->failed && (h->pages << TIDE_HEAP_PAGE_SHIFT) < h->used) {
        const uint32_t count = tide_read_u32(r);
        if (!places_hold(h->pages, count)) {
            r->failed = true;
            break;
        }
        add_page(h, count);
        tide_page *p = h->page[h->pages - 1u];
        const uint32_t bytes = in_use(h, p);
        const uint8_t *data = tide_read_bytes(r, bytes);
        if (data) memcpy(tide_page_data(p), data, bytes);
    }
    tide_heap_settle(h);
    return !r->failed;
}

// Packed as part of a delta: its numbers, each page's places as runs of pages
// that take as many, then its pages as regions.

// The base's page that starts at place `at`, or NULL.
static tide_page *base_page(const tide_heap *base, const uint32_t at)
{
    if (!base || at >= base->pages) return NULL;
    tide_page *b = base->page[at];
    return b->first == at ? b : NULL;
}

void tide_heap_pack_delta(const tide_heap *h, const tide_heap *base, tide_delta_writer *d)
{
    tide_delta_number(d, h->used);
    for (uint32_t c = 0; c < TIDE_HEAP_CLASSES; c++) tide_delta_number(d, h->free[c]);
    tide_delta_number(d, h->pending);
    uint32_t run = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) {
        if (run && places(h->page[i]) == count) {
            run++;
            continue;
        }
        if (run) {
            tide_delta_number(d, run);
            tide_delta_number(d, count);
        }
        run = 1;
        count = places(h->page[i]);
    }
    if (run) {
        tide_delta_number(d, run);
        tide_delta_number(d, count);
    }
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) {
        const tide_page *p = h->page[i];
        const tide_page *b = base_page(base, i);
        tide_delta_region(d, tide_page_data(p), in_use(h, p), b ? tide_page_data(b) : NULL, b ? in_use(base, b) : 0u,
                          b == p, 0u);
    }
    tide_delta_close(d);
}

bool tide_heap_unpack_delta(tide_heap *h, const tide_heap *base, tide_delta_reader *d)
{
    h->used = tide_delta_get_number(d);
    for (uint32_t c = 0; c < TIDE_HEAP_CLASSES; c++) h->free[c] = tide_delta_get_number(d);
    h->pending = tide_delta_get_number(d);
    if (d->bytes.failed || !numbers_hold(h)) return false;
    // The runs of places, read once to get past them, then again with the pages
    tide_reader runs = d->bytes;
    uint32_t run = 0;
    uint32_t count = 0;
    for (uint32_t pages = 0; (pages << TIDE_HEAP_PAGE_SHIFT) < h->used; pages += count, run--) {
        if (run == 0) {
            run = tide_delta_get_number(d);
            count = tide_delta_get_number(d);
        }
        if (d->bytes.failed || run == 0 || !places_hold(pages, count)) return false;
    }
    if (run) return false; // More pages than it's handed out
    while ((h->pages << TIDE_HEAP_PAGE_SHIFT) < h->used) {
        if (run == 0) {
            run = tide_read_varint(&runs);
            count = tide_read_varint(&runs);
        }
        run--;
        make_room(h, h->pages + count);
        tide_page *b = base_page(base, h->pages);
        const uint32_t size = count << TIDE_HEAP_PAGE_SHIFT;
        const uint32_t left = h->used - (h->pages << TIDE_HEAP_PAGE_SHIFT);
        tide_page *p = tide_delta_page(d, left < size ? left : size, size, count, b, b ? in_use(base, b) : 0u,
                                       b && b->size == size);
        if (!p) {
            tide_heap_settle(h);
            return false;
        }
        if (p != b) p->first = h->pages;
        for (uint32_t k = 0; k < count; k++) h->page[h->pages + k] = p;
        h->pages += count;
    }
    tide_heap_settle(h);
    return tide_delta_closed(d);
}

void tide_heap_hash_pages(const tide_heap *h, tide_writer *w)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) count++;
    tide_write_varint(w, count);
    for (uint32_t i = 0; i < h->pages; i += places(h->page[i])) {
        tide_write_varint(w, places(h->page[i]));
        tide_write_u64(w, tide_page_hash(h->page[i], in_use(h, h->page[i])));
    }
}

void tide_heap_need_pages(const tide_heap *base, tide_needs *n)
{
    const uint32_t count = tide_needs_count(n);
    uint32_t at = 0; // The place their page starts at
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t taking = tide_read_varint(&n->list);
        if (!places_hold(at, taking)) n->list.failed = true;
        tide_page *b = n->list.failed ? NULL : base_page(base, at);
        tide_needs_put(n, b != NULL, b ? tide_page_hash(b, in_use(base, b)) : 0u);
        if (!n->list.failed) at += taking;
    }
}
