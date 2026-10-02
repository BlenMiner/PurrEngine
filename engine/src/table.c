#include "tide/table.h"

#include <stdlib.h>
#include <string.h>

// See tide/table.h.

#define FIRST_ROOM 4u // Rows the first chunk starts with room for

static uint32_t full_room(const tide_columns *c)
{
    return 1u << c->shift;
}

static void make_room(tide_table *t, const tide_columns *c, const uint32_t chunks)
{
    if (chunks <= t->room) return;
    uint32_t room = t->room ? t->room * 2u : 4u;
    while (room < chunks) room *= 2u;
    t->pages = tide_realloc(t->pages, (size_t)t->room * c->count * sizeof *t->pages, (size_t)room * c->count * sizeof *t->pages);
    t->room = room;
}

// A byte count that has to fit in 32 bits: a world's packed size, and what's in a page.
static uint32_t bytes_of(const uint64_t n)
{
    if (n > UINT32_MAX) tide_out_of_memory();
    return (uint32_t)n;
}

void *tide_table_column_mut(tide_table *t, const tide_columns *c, const uint32_t chunk, const uint32_t column)
{
    tide_page **p = &t->pages[chunk * c->count + column];
    *p = tide_page_own(*p, 1, tide_table_rows(t, c->shift, chunk) * c->sizes[column]);
    return tide_page_data(*p);
}

void *tide_table_cell(tide_table *t, const tide_columns *c, const uint32_t row, const uint32_t column)
{
    uint8_t *rows = tide_table_column_mut(t, c, row >> c->shift, column);
    return rows + (size_t)(row & (full_room(c) - 1u)) * c->sizes[column];
}

uint32_t tide_table_add(tide_table *t, const tide_columns *c)
{
    if (t->count == UINT32_MAX) tide_out_of_memory();
    const uint32_t row = t->count;
    const uint32_t chunk = row >> c->shift;
    const uint32_t at = row & (full_room(c) - 1u);
    if (chunk == t->chunks) { // A new chunk: the first starts small
        make_room(t, c, chunk + 1u);
        const uint32_t room = chunk == 0 && FIRST_ROOM < full_room(c) ? FIRST_ROOM : full_room(c);
        for (uint32_t k = 0; k < c->count; k++) t->pages[chunk * c->count + k] = tide_page_new(bytes_of((uint64_t)room * c->sizes[k]));
        t->chunks++;
        t->last_room = room;
    } else if (at == t->last_room) { // The first chunk, full but not as big as the others: twice the room
        const uint32_t room = t->last_room * 2u;
        for (uint32_t k = 0; k < c->count; k++) {
            tide_page *old = t->pages[chunk * c->count + k];
            tide_page *grown = tide_page_new(bytes_of((uint64_t)room * c->sizes[k]));
            memcpy(tide_page_data(grown), tide_page_data(old), (size_t)at * c->sizes[k]);
            tide_page_release(old, 1);
            t->pages[chunk * c->count + k] = grown;
        }
        t->last_room = room;
    }
    t->count++;
    for (uint32_t k = 0; k < c->count; k++) memset(tide_table_cell(t, c, row, k), 0, c->sizes[k]);
    return row;
}

void tide_table_remove(tide_table *t, const tide_columns *c, const uint32_t row, tide_entities *entities,
                       const uint32_t archetype)
{
    const uint32_t last = t->count - 1u;
    if (row != last) {
        for (uint32_t k = 0; k < c->count; k++) {
            void *gap = tide_table_cell(t, c, row, k);
            memcpy(gap, tide_table_get(t, c, last, k), c->sizes[k]);
        }
        tide_entity moved;
        memcpy(&moved, tide_table_get(t, c, row, 0), sizeof moved);
        tide_entity_set_location(entities, moved, (tide_location){archetype, row});
    }
    t->count = last;
    // A last chunk left empty goes
    if (t->count == (t->chunks - 1u) << c->shift) {
        t->chunks--;
        for (uint32_t k = 0; k < c->count; k++) tide_page_release(t->pages[t->chunks * c->count + k], 1);
        t->last_room = t->chunks ? full_room(c) : 0u;
    }
}

void tide_table_remove_row(tide_table *t, const tide_columns *c, const uint32_t row)
{
    const uint32_t last = t->count - 1u;
    if (row != last) {
        for (uint32_t k = 0; k < c->count; k++) memcpy(tide_table_cell(t, c, row, k), tide_table_get(t, c, last, k), c->sizes[k]);
    }
    t->count = last;
    // A last chunk left empty goes
    if (t->count == (t->chunks - 1u) << c->shift) {
        t->chunks--;
        for (uint32_t k = 0; k < c->count; k++) tide_page_release(t->pages[t->chunks * c->count + k], 1);
        t->last_room = t->chunks ? full_room(c) : 0u;
    }
}

uint32_t tide_table_move(tide_table *from, const tide_columns *from_columns, const uint32_t row,
                         const uint32_t from_archetype, tide_table *to, const tide_columns *to_columns,
                         const uint32_t to_archetype, const int32_t *map, tide_entities *entities)
{
    const uint32_t dst = tide_table_add(to, to_columns);
    for (uint32_t k = 0; k < to_columns->count; k++) {
        if (map[k] < 0) continue;
        memcpy(tide_table_cell(to, to_columns, dst, k), tide_table_get(from, from_columns, row, (uint32_t)map[k]),
               to_columns->sizes[k]);
    }
    tide_entity e;
    memcpy(&e, tide_table_get(from, from_columns, row, 0), sizeof e);
    tide_entity_set_location(entities, e, (tide_location){to_archetype, dst});
    tide_table_remove(from, from_columns, row, entities, from_archetype);
    return dst;
}

void tide_table_copy(tide_table *to, const tide_table *from, const tide_columns *c)
{
    if (to == from) return;
    // Its pages first, then letting go of to's: they can be the same ones.
    const uint32_t pages = from->chunks * c->count;
    for (uint32_t i = 0; i < pages; i++) tide_page_retain(from->pages[i]);
    for (uint32_t i = 0; i < to->chunks * c->count; i++) tide_page_release(to->pages[i], 1);
    make_room(to, c, from->chunks);
    if (pages) memcpy(to->pages, from->pages, pages * sizeof *to->pages);
    to->count = from->count;
    to->chunks = from->chunks;
    to->last_room = from->last_room;
}

uint64_t tide_table_hash(uint64_t h, const tide_table *t, const tide_columns *c)
{
    h = tide_hash_more(h, &t->count, sizeof t->count);
    for (uint32_t chunk = 0; chunk < t->chunks; chunk++) {
        const uint32_t rows = tide_table_rows(t, c->shift, chunk);
        for (uint32_t k = 0; k < c->count; k++) {
            const uint64_t page = tide_page_hash(t->pages[chunk * c->count + k], rows * c->sizes[k]);
            h = tide_hash_more(h, &page, sizeof page);
        }
    }
    return h;
}

void tide_table_free(tide_table *t, const tide_columns *c)
{
    for (uint32_t i = 0; i < t->chunks * c->count; i++) tide_page_release(t->pages[i], 1);
    free(t->pages);
    memset(t, 0, sizeof *t);
}

uint32_t tide_table_packed_size(const tide_table *t, const tide_columns *c)
{
    uint64_t row = 0;
    for (uint32_t k = 0; k < c->count; k++) row += c->sizes[k];
    return bytes_of(4u + row * t->count);
}

void tide_table_pack(const tide_table *t, const tide_columns *c, tide_writer *w)
{
    tide_write_u32(w, t->count);
    for (uint32_t k = 0; k < c->count; k++) {
        for (uint32_t chunk = 0; chunk < t->chunks; chunk++) {
            tide_write_bytes(w, tide_table_column(t, c, chunk, k), tide_table_rows(t, c->shift, chunk) * c->sizes[k]);
        }
    }
}

// The room `count` rows' chunks have, as adding them one by one would have
// made: all but the first, alone, have room for 1 << shift.
static uint32_t room_for(const tide_columns *c, const uint32_t count)
{
    if (count > full_room(c)) return full_room(c);
    uint32_t room = FIRST_ROOM < full_room(c) ? FIRST_ROOM : full_room(c);
    while (room < count) room *= 2u;
    return room;
}

bool tide_table_unpack(tide_table *t, const tide_columns *c, tide_reader *r)
{
    const uint32_t count = tide_read_u32(r);
    uint64_t row = 0;
    for (uint32_t k = 0; k < c->count; k++) row += c->sizes[k];
    if (r->failed || (uint64_t)count * row > r->size - r->at) return false;
    const uint32_t chunks = (uint32_t)(((uint64_t)count + full_room(c) - 1u) >> c->shift);
    make_room(t, c, chunks);
    const uint32_t room = room_for(c, count);
    for (uint32_t chunk = 0; chunk < chunks; chunk++) {
        for (uint32_t k = 0; k < c->count; k++) {
            t->pages[chunk * c->count + k] = tide_page_new(bytes_of((uint64_t)room * c->sizes[k]));
        }
    }
    t->count = count;
    t->chunks = chunks;
    t->last_room = chunks ? room : 0u;
    for (uint32_t k = 0; k < c->count; k++) {
        for (uint32_t chunk = 0; chunk < chunks; chunk++) {
            const uint32_t bytes = tide_table_rows(t, c->shift, chunk) * c->sizes[k];
            memcpy(tide_page_data(t->pages[chunk * c->count + k]), tide_read_bytes(r, bytes), bytes);
        }
    }
    return !r->failed;
}

// Packed as part of a delta: its count, then each chunk's columns as regions,
// in the order of `pages`.

void tide_table_pack_delta(const tide_table *t, const tide_table *base, const tide_columns *c, tide_delta_writer *d)
{
    tide_delta_number(d, t->count);
    for (uint32_t chunk = 0; chunk < t->chunks; chunk++) {
        const bool based = base && chunk < base->chunks;
        for (uint32_t k = 0; k < c->count; k++) {
            const tide_page *p = t->pages[chunk * c->count + k];
            const tide_page *b = based ? base->pages[chunk * c->count + k] : NULL;
            tide_delta_region(d, tide_page_data(p), tide_table_rows(t, c->shift, chunk) * c->sizes[k],
                              b ? tide_page_data(b) : NULL, b ? tide_table_rows(base, c->shift, chunk) * c->sizes[k] : 0u,
                              b == p);
        }
    }
    tide_delta_close(d);
}

bool tide_table_unpack_delta(tide_table *t, const tide_table *base, const tide_columns *c, tide_delta_reader *d)
{
    const uint32_t count = tide_delta_get_number(d);
    if (d->bytes.failed) return false;
    // Each page is the base's, or bytes of the delta
    const uint32_t chunks = (uint32_t)(((uint64_t)count + full_room(c) - 1u) >> c->shift);
    const uint64_t pages = (uint64_t)chunks * c->count;
    if (pages > (uint64_t)(base ? base->chunks * c->count : 0u) + (d->bytes.size - d->bytes.at)) return false;
    make_room(t, c, chunks);
    const uint32_t room = room_for(c, count);
    t->count = count; // For tide_table_rows: its chunks follow
    t->last_room = chunks ? room : 0u;
    for (uint32_t chunk = 0; chunk < chunks; chunk++) {
        const bool based = base && chunk < base->chunks;
        for (uint32_t k = 0; k < c->count; k++) {
            tide_page *b = based ? base->pages[chunk * c->count + k] : NULL;
            const uint32_t page_size = bytes_of((uint64_t)room * c->sizes[k]);
            tide_page *p = tide_delta_page(d, tide_table_rows(t, c->shift, chunk) * c->sizes[k], page_size, 1, b,
                                           b ? tide_table_rows(base, c->shift, chunk) * c->sizes[k] : 0u,
                                           b && b->size == page_size);
            if (!p) { // Its chunks so far stay, for tide_table_free
                for (uint32_t m = 0; m < k; m++) tide_page_release(t->pages[chunk * c->count + m], 1);
                return false;
            }
            t->pages[chunk * c->count + k] = p;
        }
        t->chunks = chunk + 1u;
    }
    return tide_delta_closed(d);
}

void tide_table_hash_pages(const tide_table *t, const tide_columns *c, tide_writer *w)
{
    tide_write_varint(w, t->chunks * c->count);
    for (uint32_t chunk = 0; chunk < t->chunks; chunk++) {
        const uint32_t rows = tide_table_rows(t, c->shift, chunk);
        for (uint32_t k = 0; k < c->count; k++) {
            tide_write_u64(w, tide_page_hash(t->pages[chunk * c->count + k], rows * c->sizes[k]));
        }
    }
}

void tide_table_need_pages(const tide_table *base, const tide_columns *c, tide_needs *n)
{
    const uint32_t count = tide_needs_count(n);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t chunk = i / c->count;
        const uint32_t k = i % c->count;
        const bool have = chunk < base->chunks;
        const uint32_t bytes = have ? tide_table_rows(base, c->shift, chunk) * c->sizes[k] : 0u;
        tide_needs_put(n, have, have ? tide_page_hash(base->pages[chunk * c->count + k], bytes) : 0u);
    }
}

// ---------------------------------------------------------------------------
// The queue

void *tide_queue_push(tide_queue *q, const uint32_t size)
{
    if (q->count == UINT32_MAX) tide_out_of_memory();
    if (q->count / TIDE_QUEUE_PAGE == q->pages) {
        q->page = tide_realloc(q->page, q->pages * sizeof *q->page, (q->pages + 1u) * sizeof *q->page);
        q->page[q->pages] = tide_alloc_zeroed(TIDE_QUEUE_PAGE, size);
        q->pages++;
    }
    void *item = tide_queue_at(q, q->count++, size);
    memset(item, 0, size);
    return item;
}

// Items in use on page `p`.
static uint32_t queued_on(const tide_queue *q, const uint32_t p)
{
    const uint32_t left = q->count - p * TIDE_QUEUE_PAGE;
    return left < TIDE_QUEUE_PAGE ? left : TIDE_QUEUE_PAGE;
}

static uint32_t queue_pages(const tide_queue *q)
{
    return (q->count + TIDE_QUEUE_PAGE - 1u) / TIDE_QUEUE_PAGE;
}

void tide_queue_clear(tide_queue *q, const uint32_t size)
{
    // It keeps the pages this tick used, for the next, and lets go of the
    // rest: what a burst of changes needed goes once it's over.
    const uint32_t used = queue_pages(q);
    const uint32_t keep = used ? used : 1u;
    for (uint32_t p = 0; p < used; p++) memset(q->page[p], 0, (size_t)queued_on(q, p) * size);
    for (uint32_t p = keep; p < q->pages; p++) free(q->page[p]);
    if (q->pages > keep) q->pages = keep;
    q->count = 0;
}

void tide_queue_copy(tide_queue *to, const tide_queue *from, const uint32_t size)
{
    if (to == from) return;
    tide_queue_clear(to, size);
    for (uint32_t i = 0; i < from->count; i++) memcpy(tide_queue_push(to, size), tide_queue_at(from, i, size), size);
}

uint64_t tide_queue_hash(uint64_t h, const tide_queue *q, const uint32_t size)
{
    h = tide_hash_more(h, &q->count, sizeof q->count);
    for (uint32_t p = 0; p < queue_pages(q); p++) h = tide_hash_more(h, q->page[p], (size_t)queued_on(q, p) * size);
    return h;
}

void tide_queue_free(tide_queue *q)
{
    for (uint32_t p = 0; p < q->pages; p++) free(q->page[p]);
    free(q->page);
    memset(q, 0, sizeof *q);
}

uint32_t tide_queue_packed_size(const tide_queue *q, const uint32_t size)
{
    return bytes_of(4u + (uint64_t)q->count * size);
}

void tide_queue_pack(const tide_queue *q, const uint32_t size, tide_writer *w)
{
    tide_write_u32(w, q->count);
    for (uint32_t p = 0; p < queue_pages(q); p++) tide_write_bytes(w, q->page[p], queued_on(q, p) * size);
}

bool tide_queue_unpack(tide_queue *q, const uint32_t size, tide_reader *r)
{
    const uint32_t count = tide_read_u32(r);
    if (r->failed || (uint64_t)count * size > r->size - r->at) return false;
    for (uint32_t i = 0; i < count; i++) memcpy(tide_queue_push(q, size), tide_read_bytes(r, size), size);
    return !r->failed;
}
