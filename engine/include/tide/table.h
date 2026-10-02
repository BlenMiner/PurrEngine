#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tide/entity.h"
#include "tide/net.h"
#include "tide/page.h"

// Archetype storage and the queue of structural changes, as worlds keep them.
// Generated code calls these with each archetype's tide_columns.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A table holds the entities with one combination of components, as columns:
// the entities themselves, the scene each is in (in a world with scenes),
// then each component. Its rows are in chunks of 1 << shift, and a chunk's
// rows of one column are one page, which the world's snapshots share until
// one of them changes it (see tide/page.h): changing a column of a chunk
// copies that page alone. The first chunk starts small and grows, so an
// archetype with a few entities takes little memory. Tables grow with no limit
// but memory.

typedef struct tide_columns {
    uint32_t count;        // Columns
    uint32_t shift;        // Each chunk has room for 1 << shift rows
    const uint32_t *sizes; // Each column's element size
} tide_columns;

typedef struct tide_table {
    uint32_t count;     // Rows in use
    uint32_t chunks;    // Chunks with pages: every one has a row in use
    uint32_t last_room; // Rows the last chunk has room for: only the first chunk, alone, has room for fewer than 1 << shift
    uint32_t room;      // Chunks `pages` has room for
    tide_page **pages;  // Chunk c's column k: pages[c * columns + k]
} tide_table;

// Rows in use in a chunk.
static inline uint32_t tide_table_rows(const tide_table *t, const uint32_t shift, const uint32_t chunk)
{
    const uint32_t left = t->count - (chunk << shift);
    return left < (1u << shift) ? left : 1u << shift;
}

// A chunk's rows of a column, to read.
static inline const void *tide_table_column(const tide_table *t, const tide_columns *c, const uint32_t chunk,
                                            const uint32_t column)
{
    return tide_page_data(t->pages[chunk * c->count + column]);
}

// A row's value in a column, to read.
static inline const void *tide_table_get(const tide_table *t, const tide_columns *c, const uint32_t row,
                                         const uint32_t column)
{
    const uint8_t *rows = tide_table_column(t, c, row >> c->shift, column);
    return rows + (size_t)(row & ((1u << c->shift) - 1u)) * c->sizes[column];
}

// A component in an archetype is in lanes: its bytes `width` at a time, each a
// column of its own, so a loop over a chunk's rows reads and writes each field
// side by side, as SIMD wants them. The width is 4 bytes, or 2 or 1 for a
// component whose size isn't a multiple of 4 (or 2). Generated code knows
// every component's lanes; this is what hosts and tools that read a world's
// bytes go by.
static inline uint32_t tide_lane_width(const uint32_t component_size)
{
    return component_size % 4u == 0 ? 4u : component_size % 2u == 0 ? 2u : 1u;
}

// A row's component, from its `lanes` columns of `width` bytes starting at
// column `first`, into `value`; and to change, from `value` into them, their
// pages made this table's own.
void tide_table_gather(const tide_table *t, const tide_columns *c, uint32_t row, uint32_t first, uint32_t lanes, uint32_t width,
                       void *value);
void tide_table_scatter(tide_table *t, const tide_columns *c, uint32_t row, uint32_t first, uint32_t lanes, uint32_t width,
                        const void *value);

// The same to change: the page is made this table's own.
static inline void *tide_table_column_mut(tide_table *t, const tide_columns *c, const uint32_t chunk, const uint32_t column)
{
    tide_page **p = &t->pages[chunk * c->count + column];
    *p = tide_page_own(*p, 1, tide_table_rows(t, c->shift, chunk) * c->sizes[column]);
    return tide_page_data(*p);
}
void *tide_table_cell(tide_table *t, const tide_columns *c, uint32_t row, uint32_t column);

// A new row at the end, zeroed: its number.
uint32_t tide_table_add(tide_table *t, const tide_columns *c);

// Takes a row out, moving the last row into its place, and tells the entity
// table where the moved one went. Release its text and lists first.
void tide_table_remove(tide_table *t, const tide_columns *c, uint32_t row, tide_entities *entities, uint32_t archetype);

// Takes a row out of a table with no entities, like a world's tasks, moving
// the last row into its place.
void tide_table_remove_row(tide_table *t, const tide_columns *c, uint32_t row);

// Moves a row to another table, as an entity gains or loses a component:
// `map` gives, for each of `to`'s columns, `from`'s column with its values, or
// -1 to leave it zero. Returns its row in `to`, where the entity table finds it.
uint32_t tide_table_move(tide_table *from, const tide_columns *from_columns, uint32_t row, uint32_t from_archetype,
                         tide_table *to, const tide_columns *to_columns, uint32_t to_archetype, const int32_t *map,
                         tide_entities *entities);

// For snapshots: `to` becomes `from`, sharing its pages. The hash covers the
// rows in use, the same for the same rows however the table got there.
void tide_table_copy(tide_table *to, const tide_table *from, const tide_columns *c);
uint64_t tide_table_hash(uint64_t h, const tide_table *t, const tide_columns *c);

// Lets go of its pages, leaving it empty.
void tide_table_free(tide_table *t, const tide_columns *c);

// The table as bytes, for sending a world and carrying it over: its count,
// then each column's rows in use, one column after another. Reading them back
// into an empty table is false when the bytes aren't a table.
uint32_t tide_table_packed_size(const tide_table *t, const tide_columns *c);
void tide_table_pack(const tide_table *t, const tide_columns *c, tide_writer *w);
bool tide_table_unpack(tide_table *t, const tide_columns *c, tide_reader *r);

// The same as part of a delta (tide/delta.h), from `base` (NULL for none):
// its count, then each chunk's columns as regions. Unpacking `t` (empty)
// shares the pages that are the same as the base's.
void tide_table_pack_delta(const tide_table *t, const tide_table *base, const tide_columns *c, tide_delta_writer *d);
bool tide_table_unpack_delta(tide_table *t, const tide_table *base, const tide_columns *c, tide_delta_reader *d);
// Its pages' hashes, and which of them `base` lacks (tide_needs).
void tide_table_hash_pages(const tide_table *t, const tide_columns *c, tide_writer *w);
void tide_table_need_pages(const tide_table *base, const tide_columns *c, tide_needs *n);

// ---------------------------------------------------------------------------
// The queue of structural changes and events a tick or frame records, applied
// at its end. Items stay where they are as more are added, so the one being
// applied can be read while what it does adds more. Snapshots copy it: it's
// empty between ticks but for the players joining and leaving.

#define TIDE_QUEUE_PAGE 64u // Items in each of its pages

typedef struct tide_queue {
    uint32_t count;
    uint32_t pages; // Pages it has: clearing keeps the ones it used
    void **page;
} tide_queue;

static inline void *tide_queue_at(const tide_queue *q, const uint32_t i, const uint32_t size)
{
    return (uint8_t *)q->page[i / TIDE_QUEUE_PAGE] + (size_t)(i % TIDE_QUEUE_PAGE) * size;
}

// A new item at the end, zeroed, on a new page.
void *tide_queue_push_page(tide_queue *q, uint32_t size);

// A new item at the end, zeroed.
static inline void *tide_queue_push(tide_queue *q, const uint32_t size)
{
    if (q->count / TIDE_QUEUE_PAGE == q->pages || q->count == UINT32_MAX) return tide_queue_push_page(q, size);
    void *item = tide_queue_at(q, q->count++, size);
    memset(item, 0, size);
    return item;
}

// Empties it. It keeps the pages it used, for the next tick, and lets go of
// the rest. Their items stay until they're pushed again, zeroed.
void tide_queue_clear(tide_queue *q, uint32_t size);

void tide_queue_copy(tide_queue *to, const tide_queue *from, uint32_t size);

// Puts `from`'s items at the end of `to`, in order.
void tide_queue_append(tide_queue *to, const tide_queue *from, uint32_t size);
uint64_t tide_queue_hash(uint64_t h, const tide_queue *q, uint32_t size);
void tide_queue_free(tide_queue *q);

uint32_t tide_queue_packed_size(const tide_queue *q, uint32_t size);
void tide_queue_pack(const tide_queue *q, uint32_t size, tide_writer *w);
bool tide_queue_unpack(tide_queue *q, uint32_t size, tide_reader *r);
