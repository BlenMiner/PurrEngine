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
// isn't too big, or else hashed), and each chunk's record: its position and
// its block.
//
// Nothing fails: outside a grid's size, reads give zero and writes do
// nothing.

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
    return (uint8_t *)tide_heap_write(heap, tide_grid_records(d)[i - 1u].block) + tide_grid_place(s, x, y, z);
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
    // The chunk is this world's own now
    const tide_grid_dir *d = tide_grid_dir_of(*g);
    *c = (tide_grid_cache){.at = g->at, .moves = __atomic_load_n(&heap->moves, __ATOMIC_RELAXED), .heap = heap,
                           .chunk = {cx, cy, cz}};
    for (int i = 0; i < 3; i++) c->size[i] = d->size[i];
    c->write = (uint8_t *)cell - tide_grid_place(s, x, y, z);
    c->read = c->write;
    return cell;
}

// ---------------------------------------------------------------------------
// Loops over a grid's cells (see docs/spec.md, Grids)
//
// A grid with a size on every axis has every cell within it. A grid with an
// open axis goes on forever, so a loop takes in the chunks it has: their
// cells, or with blocks, every block that takes in part of one.
//
// Parallel loops: `parallel (var at in cells by 2 offset o)`, and a foreach
// whose steps touch only their own cell. Their steps run at once, on threads
// (tide_parallel_for): each reads the grid as the loop found it, and writes
// only its own cell or block, into a buffer for its chunk, made from the chunk
// as it was. The loop's end puts the buffers into the grid, chunk by chunk in
// order, so the result is the same however the steps were shared out, and the
// grid itself doesn't change while they run. Along a sized axis, its blocks
// are those wholly inside the size, as the grid ends there.
//
// Its tasks are tiles: the blocks that start in a chunk's cells, each tile a
// chunk's place, whether the grid has that chunk or not.

typedef struct tide_par_loop {
    tide_grid *grid;
    const tide_grid_shape *shape;
    uint32_t where;
    int32_t block[3];  // Each step's block, in cells: 1 for a cell
    int32_t offset[3]; // Where the blocks start
    int32_t lo[3];     // Blocks within its size, by index: from lo up to hi (open axes: any)
    int32_t hi[3];
    uint32_t tasks;    // Tiles
    int32_t *tiles;    // Each tile's chunk place, 3 each, in order (by z, then y, then x)
    uint32_t chunks;   // The chunks its blocks can write in...
    int32_t *chunk;    // ...their places, 3 each, in order
    uint8_t **buffers; // ...and each one's buffer, once a step writes it
    uint64_t steps;    // Blocks in all, about: how much work it is
    bool open;         // The grid has an open axis
} tide_par_loop;

// The chunk a task last wrote in, and its buffer.
typedef struct tide_par_cache {
    int32_t chunk[3];
    uint8_t *buffer; // NULL for none yet
} tide_par_cache;

// Sets `l` up to go over `g`'s blocks of `block` cells starting at `offset`
// (on the axes the grid has).
void tide_par_begin(tide_par_loop *l, tide_grid *g, const tide_grid_shape *s, uint32_t where, const int32_t block[3],
                    const int32_t offset[3]);

// Task `task`'s blocks, by index on each axis: from[i] up to to[i]. True when
// the grid has no chunk at its tile's place, so a block there runs only if it
// takes in part of a chunk the grid has (tide_par_live).
bool tide_par_tile(const tide_par_loop *l, uint32_t task, int32_t from[3], int32_t to[3]);

// Whether the block at `at` takes in part of a chunk the grid has.
bool tide_par_live(const tide_par_loop *l, int32_t x, int32_t y, int32_t z);

// The buffer of the chunk at (cx, cy, cz), made as the chunk was.
uint8_t *tide_par_buffer(tide_par_loop *l, int32_t cx, int32_t cy, int32_t cz);

// The cell at (x, y, z), to write: in its chunk's buffer.
static inline void *tide_par_cell(tide_par_loop *l, tide_par_cache *c, const int32_t x, const int32_t y, const int32_t z)
{
    const tide_grid_shape *s = l->shape;
    const int32_t cx = x >> s->shift[0], cy = y >> s->shift[1], cz = z >> s->shift[2];
    if (!c->buffer || c->chunk[0] != cx || c->chunk[1] != cy || c->chunk[2] != cz) {
        c->chunk[0] = cx;
        c->chunk[1] = cy;
        c->chunk[2] = cz;
        c->buffer = tide_par_buffer(l, cx, cy, cz);
    }
    return c->buffer + tide_grid_place(s, x, y, z);
}

// Puts what the steps wrote into the grid and lets the buffers go.
void tide_par_end(tide_par_loop *l);

// Going through a grid's cells in order, as a foreach that isn't a parallel
// loop does: rows from the first (by z, then y), and each from its lowest x.
// Its memory is the scratch area's (tide/text.h).
typedef struct tide_grid_rows {
    int32_t size[3];      // The grid's, 0 where it's open
    int32_t side[3];      // A chunk's cells along each axis
    const int32_t *chunk; // Open: the chunks it has, 3 each, in order (by z, then y, then x)
    uint32_t count;
    uint32_t layer;       // Open: the chunks of the current layer (one cz) from here...
    uint32_t layer_end;   // ...to here
    uint32_t row;         // ...of the current row of chunks (one cy) from here...
    uint32_t row_end;     // ...to here
    uint32_t next;        // ...and the next of them along the current row of cells
    int32_t y, y_end;     // The current row of cells, and past the last in its chunks
    int32_t z, z_end;
    bool done;
} tide_grid_rows;

void tide_grid_rows_begin(tide_grid_rows *r, tide_grid g, const tide_grid_shape *s);

// The next run of cells along a row: x from *x0 up to *x1, at (*y, *z). False
// once there are none.
bool tide_grid_rows_next(tide_grid_rows *r, int32_t *x0, int32_t *x1, int32_t *y, int32_t *z);

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
