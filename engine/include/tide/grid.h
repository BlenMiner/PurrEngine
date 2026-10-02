#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/entity.h"
#include "tide/heap.h"
#include "tide/text.h"

// Tide's `Grid2<T>` and `Grid3<T>`: cells at int2 or int3 positions, in a
// world's heap. Generated code calls the functions below with its grid type's
// shape (tide_grid_shape), which it knows when it compiles.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A grid keeps its cells in chunks, a heap block each, as many cells along
// each axis as a power of two: 4096 of them (64 by 64, or 16 by 16 by 16), or
// fewer for cells bigger than 4 bytes, so a chunk is never more than a page.
// A chunk only exists once a cell in it is set to something other than zero:
// reading anywhere else gives zero, so an open grid (no size on an axis)
// costs what's in it, not the space it spans. A world and its snapshots share
// pages until one of them changes one (see tide/page.h), so changing a cell
// copies and hashes its chunk's page, never the whole grid.
//
// The grid's own block, its directory, says where each chunk is: a table by
// chunk position (by its index, for a grid with a size on every axis that
// isn't too big, or else hashed), and each chunk's record: its position, its
// block, the tick it was made and the last tick a cell of it changed.
//
// A chunk system (`chunk mut Field.cells cells`) runs once for each chunk
// that existed when the tick began, on threads, in phases: with a reach (how
// far before and after its chunk it touches cells on each axis, and which of
// the chunks around its own it gets into, which tidec works out), a task
// touches its chunk and those, so tasks in one phase are placed never to
// touch the same chunk. Each task works through a view (tide_grid_view). Chunks it makes are its own
// until its phase is done, when the system makes them in the heap in task
// order (tide_grid_finish), so threads never change what's in the heap.
//
// Nothing fails: outside a grid's size, or past a chunk system's reach,
// reads give zero and writes do nothing.

typedef struct tide_grid {
    uint32_t at; // 0: an open grid with nothing in it. Otherwise its directory's offset, and where in the top two bits
} tide_grid;

// A grid type's cells, as generated code describes them.
typedef struct tide_grid_shape {
    uint32_t cell;     // Bytes each
    uint32_t dims;     // 2 or 3
    uint32_t shift[3]; // A chunk is 1 << shift[i] cells along axis i (shift[2] is 0 in 2D)
} tide_grid_shape;

// A chunk's cells, in bytes: its whole block (its block's header is cells
// too, while it's a chunk: see grid.c).
static inline uint32_t tide_grid_chunk_bytes(const tide_grid_shape *s)
{
    return s->cell << (s->shift[0] + s->shift[1] + s->shift[2]);
}

typedef struct tide_grid_record {
    int32_t chunk[3]; // Its position, in chunks
    uint32_t block;   // Its cells
    uint32_t born;    // The tick it was made
    uint32_t changed; // The last tick a cell of it changed
} tide_grid_record;

typedef struct tide_grid_dir {
    int32_t size[3];   // Cells along each axis, 0 where it's open
    uint32_t dims;
    uint32_t records;  // Chunks it has, in `record`
    uint32_t room;     // ...and room for
    uint32_t slots;    // In its table
    uint32_t flat;     // Its table is by chunk index: it has a size on every axis
    int32_t chunks[3]; // flat: chunks along each axis
    uint32_t unused;
    // Then tide_grid_record record[room], and uint32_t table[slots]: a
    // record's index plus one, 0 for none
} tide_grid_dir;

static inline tide_grid_record *tide_grid_records(const tide_grid_dir *d)
{
    return (tide_grid_record *)(uintptr_t)(d + 1);
}

static inline uint32_t *tide_grid_table(const tide_grid_dir *d)
{
    return (uint32_t *)(uintptr_t)(tide_grid_records(d) + d->room);
}

static inline uint32_t tide_grid_hash(const int32_t cx, const int32_t cy, const int32_t cz)
{
    uint32_t h = (uint32_t)cx * 0x9E3779B1u ^ (uint32_t)cy * 0x85EBCA77u ^ (uint32_t)cz * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    return h ^ (h >> 12);
}

// Whether a cell is within the grid's size, on the axes it has one.
static inline bool tide_grid_inside(const tide_grid_dir *d, const int32_t x, const int32_t y, const int32_t z)
{
    return (!d->size[0] || (x >= 0 && x < d->size[0])) && (!d->size[1] || (y >= 0 && y < d->size[1]))
        && (!d->size[2] || (z >= 0 && z < d->size[2]));
}

// The record of the chunk at (cx, cy, cz), in chunks, or 0; otherwise its index plus one.
static inline uint32_t tide_grid_find(const tide_grid_dir *d, const int32_t cx, const int32_t cy, const int32_t cz)
{
    const uint32_t *table = tide_grid_table(d);
    if (d->flat) {
        if ((uint32_t)cx >= (uint32_t)d->chunks[0] || (uint32_t)cy >= (uint32_t)d->chunks[1] || (uint32_t)cz >= (uint32_t)d->chunks[2]) {
            return 0;
        }
        return table[((uint32_t)cz * (uint32_t)d->chunks[1] + (uint32_t)cy) * (uint32_t)d->chunks[0] + (uint32_t)cx];
    }
    const tide_grid_record *record = tide_grid_records(d);
    for (uint32_t s = tide_grid_hash(cx, cy, cz) & (d->slots - 1u);; s = (s + 1u) & (d->slots - 1u)) {
        const uint32_t i = table[s];
        if (!i) return 0;
        const tide_grid_record *r = &record[i - 1u];
        if (r->chunk[0] == cx && r->chunk[1] == cy && r->chunk[2] == cz) return i;
    }
}

// A cell's place in its chunk, in bytes.
static inline uint32_t tide_grid_place(const tide_grid_shape *s, const int32_t x, const int32_t y, const int32_t z)
{
    const uint32_t lx = (uint32_t)x & ((1u << s->shift[0]) - 1u);
    const uint32_t ly = (uint32_t)y & ((1u << s->shift[1]) - 1u);
    const uint32_t lz = (uint32_t)z & ((1u << s->shift[2]) - 1u);
    return (lx | ly << s->shift[0] | lz << (s->shift[0] + s->shift[1])) * s->cell;
}

// A grid's directory, to read, or NULL for an empty one.
static inline const tide_grid_dir *tide_grid_dir_of(const tide_grid g)
{
    if (!g.at) return NULL;
    const tide_block *b = tide_block_at(g.at);
    return b ? (const tide_grid_dir *)(const void *)(b + 1) : NULL;
}

// A chunk's cells, to read: they start where its block does.
static inline const uint8_t *tide_grid_chunk_at(const tide_heap *heap, const uint32_t block)
{
    return (const uint8_t *)tide_heap_block(heap, block);
}

// A cell, to read, or NULL where the grid has none (zero).
static inline const void *tide_grid_peek(const tide_grid g, const int32_t x, const int32_t y, const int32_t z,
                                         const tide_grid_shape *s)
{
    const tide_grid_dir *d = tide_grid_dir_of(g);
    if (!d || !d->records || !tide_grid_inside(d, x, y, z)) return NULL;
    const uint32_t i = tide_grid_find(d, x >> s->shift[0], y >> s->shift[1], z >> s->shift[2]);
    if (!i) return NULL;
    return tide_grid_chunk_at(tide_heap_of(g.at >> 30), tide_grid_records(d)[i - 1u].block) + tide_grid_place(s, x, y, z);
}

// A cell, to change, made if `make` (a value that isn't zero) and its chunk
// isn't there yet; NULL outside the grid, in the scratch area, or for zero
// where there's no chunk. `g` is a field of the world at `where`, which an
// empty grid's first chunk gives a directory.
void *tide_grid_poke_slow(tide_grid *g, int32_t x, int32_t y, int32_t z, const tide_grid_shape *s, bool make, uint32_t where);

static inline void *tide_grid_poke(tide_grid *g, const int32_t x, const int32_t y, const int32_t z, const tide_grid_shape *s,
                                   const bool make, const uint32_t where)
{
    const tide_grid_dir *d = tide_grid_dir_of(*g);
    if (!d || !d->records || !tide_grid_inside(d, x, y, z) || g->at >> 30 != where) {
        return tide_grid_poke_slow(g, x, y, z, s, make, where);
    }
    const uint32_t i = tide_grid_find(d, x >> s->shift[0], y >> s->shift[1], z >> s->shift[2]);
    if (!i) return tide_grid_poke_slow(g, x, y, z, s, make, where);
    tide_heap *heap = tide_heap_of(g->at >> 30);
    const tide_grid_record *r = &tide_grid_records(d)[i - 1u];
    if (r->changed != heap->tick) { // Marked changed this tick, in the directory made this world's own
        tide_grid_dir *w = (tide_grid_dir *)(void *)(tide_heap_write(heap, g->at & 0x3FFFFFFFu) + 1);
        tide_grid_records(w)[i - 1u].changed = heap->tick;
        r = &tide_grid_records(w)[i - 1u];
    }
    return (uint8_t *)tide_heap_write(heap, r->block) + tide_grid_place(s, x, y, z);
}

// The chunk code last used in a grid, so a loop over its cells looks each
// chunk up once, not at every cell: generated code keeps one for each grid
// type in each function. It holds while the grid's heap hasn't moved or
// released a block since (tide_heap.moves), so whatever else the code calls,
// changing the grid too, it never reads or writes where it shouldn't.
typedef struct tide_grid_cache {
    uint32_t at;            // The grid (its tide_grid.at), 0 for none
    uint32_t moves;         // Its heap's moves when the chunk was looked up
    const tide_heap *heap;  // ...that heap, which stays the same while a function runs
    int32_t chunk[3];       // The chunk, in chunks
    int32_t size[3];        // The grid's size, 0 on open axes
    const uint8_t *read;    // The chunk's cells, or NULL where it has none (zeros)
    uint8_t *write;         // ...to change, once a write made them this world's own
} tide_grid_cache;

// Whether the cache holds the chunk of `g` at (cx, cy, cz), still where it was.
static inline bool tide_grid_cache_holds(const tide_grid_cache *c, const uint32_t at, const int32_t cx, const int32_t cy,
                                         const int32_t cz)
{
    return c->at == at && c->at && c->chunk[0] == cx && c->chunk[1] == cy && c->chunk[2] == cz
        && __atomic_load_n(&c->heap->moves, __ATOMIC_RELAXED) == c->moves;
}

static inline bool tide_grid_cache_inside(const int32_t size[3], const int32_t x, const int32_t y, const int32_t z)
{
    return (!size[0] || (x >= 0 && x < size[0])) && (!size[1] || (y >= 0 && y < size[1]))
        && (!size[2] || (z >= 0 && z < size[2]));
}

// tide_grid_peek, through a cache.
static inline const void *tide_grid_cached_peek(tide_grid_cache *c, const tide_grid g, const int32_t x, const int32_t y,
                                                const int32_t z, const tide_grid_shape *s)
{
    const int32_t cx = x >> s->shift[0], cy = y >> s->shift[1], cz = z >> s->shift[2];
    if (!tide_grid_cache_holds(c, g.at, cx, cy, cz)) {
        const tide_heap *heap = tide_heap_of(g.at >> 30);
        if (!heap || !g.at) return tide_grid_peek(g, x, y, z, s); // The scratch area's, or empty
        *c = (tide_grid_cache){.at = g.at, .moves = __atomic_load_n(&heap->moves, __ATOMIC_RELAXED), .heap = heap,
                               .chunk = {cx, cy, cz}};
        const tide_grid_dir *d = tide_grid_dir_of(g);
        if (!d || !d->records) return NULL;
        for (int i = 0; i < 3; i++) c->size[i] = d->size[i];
        const uint32_t i = tide_grid_find(d, cx, cy, cz);
        if (i) c->read = tide_grid_chunk_at(heap, tide_grid_records(d)[i - 1u].block);
    }
    if (!c->read || !tide_grid_cache_inside(c->size, x, y, z)) return NULL;
    return c->read + tide_grid_place(s, x, y, z);
}

// tide_grid_poke, through a cache.
static inline void *tide_grid_cached_poke(tide_grid_cache *c, tide_grid *g, const int32_t x, const int32_t y,
                                          const int32_t z, const tide_grid_shape *s, const bool make, const uint32_t where)
{
    const int32_t cx = x >> s->shift[0], cy = y >> s->shift[1], cz = z >> s->shift[2];
    if (c->write && tide_grid_cache_holds(c, g->at, cx, cy, cz) && tide_grid_cache_inside(c->size, x, y, z)) {
        return c->write + tide_grid_place(s, x, y, z);
    }
    void *cell = tide_grid_poke(g, x, y, z, s, make, where);
    tide_heap *heap = tide_heap_of(where);
    if (!cell || !heap || g->at >> 30 != where) {
        *c = (tide_grid_cache){0};
        return cell;
    }
    // The chunk is this world's own now, marked changed this tick
    const tide_grid_dir *d = tide_grid_dir_of(*g);
    *c = (tide_grid_cache){.at = g->at, .moves = __atomic_load_n(&heap->moves, __ATOMIC_RELAXED), .heap = heap,
                           .chunk = {cx, cy, cz}};
    for (int i = 0; i < 3; i++) c->size[i] = d->size[i];
    c->write = (uint8_t *)cell - tide_grid_place(s, x, y, z);
    c->read = c->write;
    return cell;
}

// A grid in the scratch area, as Grid2(...) and Grid3(...) make it: its size
// (0 where it's open), which a world's field takes when it owns it.
tide_grid tide_grid_new(uint32_t dims, int32_t x, int32_t y, int32_t z);

// A grid just copied into a world, like a spawn's component into the command
// queue: a directory of the world's own, with the same size and no chunks.
void tide_grid_own(tide_grid *g, const tide_grid_shape *s, uint32_t where);

// Assigning a new grid (Grid2(...)) to a world's field: the old one's chunks
// released, and the new one's size.
void tide_grid_set(tide_grid *to, tide_grid value, const tide_grid_shape *s, uint32_t where);

// A world's grid leaving it, its chunks and directory released.
void tide_grid_release(tide_grid *g, const tide_grid_shape *s, uint32_t where);

// Every cell back to zero: the chunks released, the size kept.
void tide_grid_clear(tide_grid *g, const tide_grid_shape *s, uint32_t where);

// The grid's size, 0 on axes where it's open.
static inline void tide_grid_size(const tide_grid g, int32_t size[3])
{
    const tide_grid_dir *d = tide_grid_dir_of(g);
    for (int i = 0; i < 3; i++) size[i] = d ? d->size[i] : 0;
}

// A world's grid, read from outside, like a host reading a component.
const void *tide_grid_read(const tide_heap *heap, tide_grid g, int32_t x, int32_t y, int32_t z, const tide_grid_shape *s);

// ---------------------------------------------------------------------------
// Chunk systems

// How many chunks a grid has, for a chunk system's tasks; its directory made
// this world's own, so its tasks can mark the chunks they change on threads.
uint32_t tide_grid_chunks(tide_grid g);

// What a chunk system's task works through: its chunk, and the chunks
// around it its reach touches, found as it starts.
typedef struct tide_grid_view {
    tide_grid g;
    const tide_grid_shape *shape;
    tide_heap *heap;
    tide_grid_dir *dir;  // The grid's directory, this world's own (tide_grid_chunks)
    uint32_t tick;
    tide_entity entity;  // Whose grid it is; null for a singleton's
    int32_t min[3];      // Its chunk's cells: what the system goes through
    int32_t max[3];
    int32_t low[3];      // ...and the cells it can touch: those and its reach
    int32_t high[3];
    int32_t center[3];   // Its chunk, in chunks
    uint32_t chunks;     // The chunks around it its reach gets into, a bit each by tide_grid_around
    // The chunks around it, by tide_grid_around: their records' index plus
    // one (0 for none), their cells to read (NULL for none), and to change,
    // once the task made one its own or made it where there was none
    uint32_t record[27];
    const uint8_t *cells[27];
    uint8_t *mine[27];
    // Pages it copied to change chunks in, which other tasks may still be
    // reading: let go of at the finish (tide_heap_write_parallel)
    tide_page *copied[27];
} tide_grid_view;

// How far a chunk system reaches past its chunk, in cells: before it (toward
// lower positions) and after it, on each axis, never more than a chunk; and
// which of the chunks around its own it gets into, a bit each by
// tide_grid_around (its own is 13). Its chunks run in `phases`: a chunk's is
// its position (in chunks) times `weight`, axis by axis, added up, modulo
// `phases`, which tidec picks so two chunks in a phase never touch the same
// chunk, with as few phases as it can.
typedef struct tide_grid_reach {
    int32_t before[3];
    int32_t after[3];
    uint32_t chunks;
    int32_t weight[3];
    uint32_t phases;
} tide_grid_reach;

// The chunk around the view's own at (cx, cy, cz), in chunks: 0 to 26, its own 13.
static inline uint32_t tide_grid_around(const tide_grid_view *v, const int32_t cx, const int32_t cy, const int32_t cz)
{
    return (uint32_t)(cx - v->center[0] + 1) + 3u * (uint32_t)(cy - v->center[1] + 1) + 9u * (uint32_t)(cz - v->center[2] + 1);
}

// Whether a cell is within the cells the view reaches on each axis. A cell
// there can still be in a chunk around its own that it doesn't get into.
static inline bool tide_grid_reaches(const tide_grid_view *v, const int32_t x, const int32_t y, const int32_t z)
{
    return x >= v->low[0] && x < v->high[0] && y >= v->low[1] && y < v->high[1] && z >= v->low[2] && z < v->high[2];
}

// Sets up a task's view of chunk `record` (an index) of `g`, with what
// `reach` says around it. False when the system doesn't run there: in another
// phase than `phase`, a chunk made this tick, or with `sleeps`, where nothing
// within its reach changed since the tick before.
bool tide_grid_view_start(tide_grid_view *v, tide_grid g, tide_entity entity, uint32_t record, const tide_grid_shape *s,
                          const tide_grid_reach *reach, uint32_t phase, bool sleeps);

// The task is done: the chunks it made go to its system's finish (tide_task_data).
void tide_grid_view_end(tide_grid_view *v);

static inline const void *tide_grid_view_peek(const tide_grid_view *v, const int32_t x, const int32_t y, const int32_t z,
                                              const tide_grid_shape *s)
{
    if (!tide_grid_reaches(v, x, y, z)) return NULL;
    const uint8_t *cells = v->cells[tide_grid_around(v, x >> s->shift[0], y >> s->shift[1], z >> s->shift[2])];
    return cells ? cells + tide_grid_place(s, x, y, z) : NULL; // NULL too in a chunk it doesn't get into
}

// The first change to a chunk around: made the task's own (a copy, if a
// snapshot shares it) and marked changed, or made where there was none if
// `make`. NULL when it isn't there and not `make`.
uint8_t *tide_grid_view_take(tide_grid_view *v, uint32_t around, bool make);

static inline void *tide_grid_view_poke(tide_grid_view *v, const int32_t x, const int32_t y, const int32_t z, const tide_grid_shape *s,
                                        const bool make)
{
    if (!tide_grid_reaches(v, x, y, z)) return NULL;
    const uint32_t a = tide_grid_around(v, x >> s->shift[0], y >> s->shift[1], z >> s->shift[2]);
    if (!(v->chunks >> a & 1u)) return NULL; // A chunk it doesn't get into: another task's
    uint8_t *cells = v->mine[a] ? v->mine[a] : tide_grid_view_take(v, a, make);
    return cells ? cells + tide_grid_place(s, x, y, z) : NULL;
}

// What a task made, as it leaves it for its system's finish: a chunk, or a
// page it copied (cells NULL).
typedef struct tide_grid_made {
    struct tide_grid_made *next;
    tide_entity entity;
    int32_t chunk[3];
    void *cells;
    tide_page *copied;
} tide_grid_made;

// A chunk system's finish: the chunks its tasks made, in task order, into
// the heap of `g`, whose field `field` returns for an entity (or the
// singleton's for null). The made ones are freed, and the pages copied let go of.
void tide_grid_finish(void **data, uint32_t tasks, tide_grid *(*field)(void *world, tide_entity entity), void *world,
                      const tide_grid_shape *s);
