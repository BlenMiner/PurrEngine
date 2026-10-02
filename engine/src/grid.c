#include "tide/grid.h"

#include <stdlib.h>
#include <string.h>

// See tide/grid.h.
//
// A chunk is a heap block whose header is cells too while it's a chunk: the
// heap only reads a header to free a block, so a chunk gets its header back
// just before it's released.

#define OFFSET(at) ((at) & 0x3FFFFFFFu)
#define FLAT_MOST (1u << 16) // Chunks a grid with a size on every axis lists by index, at most

// The heap's class for a chunk's block: 16 << class bytes, as many as its cells or more.
static uint32_t chunk_class(const tide_grid_shape *s)
{
    uint32_t c = 0;
    while ((16u << c) < tide_grid_chunk_bytes(s)) c++;
    return c;
}

static tide_grid_dir *dir_at(tide_heap *heap, const uint32_t block)
{
    return (tide_grid_dir *)(void *)(tide_heap_write(heap, block) + 1);
}

static uint32_t dir_bytes(const uint32_t room, const uint32_t slots)
{
    return (uint32_t)(sizeof(tide_grid_dir) + room * sizeof(tide_grid_record) + slots * sizeof(uint32_t));
}

static int32_t chunks_along(const int32_t size, const uint32_t shift)
{
    return (int32_t)(((uint32_t)size + (1u << shift) - 1u) >> shift);
}

// A grid's directory in `heap`, with its size and no chunks: its block.
static uint32_t new_dir(tide_heap *heap, const int32_t size[3], const tide_grid_shape *s)
{
    tide_grid_dir head = {{size[0], size[1], s->dims == 3 ? size[2] : 0}, s->dims, 0, 8u, 16u, 0, {1, 1, 1}, 0};
    const bool sized = size[0] > 0 && size[1] > 0 && (s->dims == 2 || size[2] > 0);
    if (sized) {
        for (uint32_t i = 0; i < s->dims; i++) head.chunks[i] = chunks_along(size[i], s->shift[i]);
        const uint64_t total = (uint64_t)head.chunks[0] * (uint64_t)head.chunks[1] * (uint64_t)head.chunks[2];
        if (total <= FLAT_MOST) {
            head.flat = 1;
            head.slots = (uint32_t)total;
        }
    }
    const uint32_t block = tide_heap_alloc(heap, dir_bytes(head.room, head.slots));
    tide_grid_dir *d = dir_at(heap, block);
    *d = head;
    return block;
}

static void insert(tide_grid_dir *d, const uint32_t index)
{
    uint32_t *table = tide_grid_table(d);
    const tide_grid_record *r = &tide_grid_records(d)[index];
    if (d->flat) {
        table[((uint32_t)r->chunk[2] * (uint32_t)d->chunks[1] + (uint32_t)r->chunk[1]) * (uint32_t)d->chunks[0]
              + (uint32_t)r->chunk[0]] = index + 1u;
        return;
    }
    uint32_t s = tide_grid_hash(r->chunk[0], r->chunk[1], r->chunk[2]) & (d->slots - 1u);
    while (table[s]) s = (s + 1u) & (d->slots - 1u);
    table[s] = index + 1u;
}

// Room for one more chunk: the directory grows into a new block, its old
// one released. Returns the directory's block.
static uint32_t make_room(tide_grid *g, tide_heap *heap)
{
    const uint32_t block = OFFSET(g->at);
    const tide_grid_dir *d = (const tide_grid_dir *)(const void *)(tide_heap_block(heap, block) + 1);
    if (d->records < d->room) return block;
    const uint32_t room = d->room * 2u;
    uint32_t slots = d->slots;
    if (!d->flat) {
        while (slots < room * 2u) slots *= 2u;
    }
    const uint32_t grown = tide_heap_alloc(heap, dir_bytes(room, slots));
    tide_grid_dir *to = dir_at(heap, grown);
    d = (const tide_grid_dir *)(const void *)(tide_heap_block(heap, block) + 1); // The heap may have moved on to another page
    *to = *d;
    to->room = room;
    to->slots = slots;
    memcpy(tide_grid_records(to), tide_grid_records(d), d->records * sizeof(tide_grid_record));
    for (uint32_t i = 0; i < to->records; i++) insert(to, i);
    tide_heap_release(heap, block);
    g->at = (g->at & ~0x3FFFFFFFu) | grown;
    return grown;
}

// A new chunk at (cx, cy, cz), in chunks, with `cells` in it (NULL: zero).
// Returns its cells.
static uint8_t *add_chunk(tide_grid *g, tide_heap *heap, const int32_t cx, const int32_t cy, const int32_t cz,
                          const void *cells, const tide_grid_shape *s)
{
    const uint32_t bytes = tide_grid_chunk_bytes(s);
    const uint32_t chunk = tide_heap_alloc(heap, bytes - (uint32_t)sizeof(tide_block));
    uint8_t *to = (uint8_t *)tide_heap_write(heap, chunk);
    if (cells) memcpy(to, cells, bytes);
    else memset(to, 0, sizeof(tide_block)); // Its header is cells now; the rest was zero
    const uint32_t block = make_room(g, heap);
    tide_grid_dir *d = dir_at(heap, block);
    tide_grid_records(d)[d->records] = (tide_grid_record){{cx, cy, cz}, chunk};
    insert(d, d->records++);
    __atomic_add_fetch(&heap->moves, 1u, __ATOMIC_RELAXED); // A cache that found no chunk here is out of date
    return (uint8_t *)tide_heap_write(heap, chunk); // Making room can give the heap another page
}

void *tide_grid_poke_slow(tide_grid *g, const int32_t x, const int32_t y, const int32_t z, const tide_grid_shape *s,
                          const bool make, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap || (g->at && g->at >> 30 != where)) return NULL; // The scratch area's, or another world's
    if (!g->at) {
        if (!make) return NULL;
        const int32_t open[3] = {0, 0, 0};
        g->at = where << 30 | new_dir(heap, open, s);
    }
    const tide_grid_dir *d = (const tide_grid_dir *)(const void *)(tide_heap_block(heap, OFFSET(g->at)) + 1);
    if (!tide_grid_inside(d, x, y, s->dims == 3 ? z : 0)) return NULL;
    const int32_t cx = x >> s->shift[0];
    const int32_t cy = y >> s->shift[1];
    const int32_t cz = s->dims == 3 ? z >> s->shift[2] : 0;
    const uint32_t i = d->records ? tide_grid_find(d, cx, cy, cz) : 0u;
    if (i) return (uint8_t *)tide_heap_write(heap, tide_grid_records(d)[i - 1u].block) + tide_grid_place(s, x, y, z);
    if (!make) return NULL;
    return add_chunk(g, heap, cx, cy, cz, NULL, s) + tide_grid_place(s, x, y, z);
}

tide_grid tide_grid_new(const uint32_t dims, const int32_t x, const int32_t y, const int32_t z)
{
    uint32_t at = 0;
    tide_block *b = tide_scratch_block((uint32_t)sizeof(tide_grid_dir), &at);
    if (!b) return (tide_grid){0};
    tide_grid_dir *d = (tide_grid_dir *)(void *)(b + 1);
    *d = (tide_grid_dir){{x > 0 ? x : 0, y > 0 ? y : 0, dims == 3 && z > 0 ? z : 0}, dims, 0, 0, 0, 0, {1, 1, 1}, 0};
    return (tide_grid){at};
}

void tide_grid_own(tide_grid *g, const tide_grid_shape *s, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap || !g->at) return;
    if (g->at >> 30 != TIDE_IN_SCRATCH) { // Only made grids are copied in; anything else starts over, empty
        g->at = 0;
        return;
    }
    int32_t size[3];
    tide_grid_size(*g, size);
    g->at = where << 30 | new_dir(heap, size, s);
}

void tide_grid_release(tide_grid *g, const tide_grid_shape *s, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (heap && g->at && g->at >> 30 == where) {
        tide_grid_clear(g, s, where);
        tide_heap_release(heap, OFFSET(g->at));
    }
    g->at = 0;
}

void tide_grid_set(tide_grid *to, const tide_grid value, const tide_grid_shape *s, const uint32_t where)
{
    if (!tide_heap_of(where)) {
        *to = value;
        return;
    }
    tide_grid_release(to, s, where);
    *to = value;
    tide_grid_own(to, s, where);
}

void tide_grid_clear(tide_grid *g, const tide_grid_shape *s, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap || !g->at || g->at >> 30 != where) return;
    tide_grid_dir *d = dir_at(heap, OFFSET(g->at));
    const uint32_t c = chunk_class(s);
    for (uint32_t i = 0; i < d->records; i++) {
        const uint32_t chunk = tide_grid_records(d)[i].block;
        tide_block *b = tide_heap_write(heap, chunk); // Its header back, for the heap to free it
        *b = (tide_block){c, 0, 0, 0};
        tide_heap_release(heap, chunk);
        d = dir_at(heap, OFFSET(g->at));
    }
    memset(tide_grid_records(d), 0, d->records * sizeof(tide_grid_record));
    memset(tide_grid_table(d), 0, d->slots * sizeof(uint32_t));
    d->records = 0;
}

const void *tide_grid_read(const tide_heap *heap, const tide_grid g, const int32_t x, const int32_t y, const int32_t z,
                           const tide_grid_shape *s)
{
    if (!g.at || g.at >> 30 == TIDE_IN_SCRATCH) return NULL;
    const tide_grid_dir *d = (const tide_grid_dir *)(const void *)(tide_heap_block(heap, OFFSET(g.at)) + 1);
    if (!d->records || !tide_grid_inside(d, x, y, s->dims == 3 ? z : 0)) return NULL;
    const uint32_t i = tide_grid_find(d, x >> s->shift[0], y >> s->shift[1], s->dims == 3 ? z >> s->shift[2] : 0);
    if (!i) return NULL;
    return tide_grid_chunk_at(heap, tide_grid_records(d)[i - 1u].block) + tide_grid_place(s, x, y, z);
}

// ---------------------------------------------------------------------------
// Loops over a grid's cells (see tide/grid.h)

static int32_t floor_div(const int32_t a, const int32_t b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0);
}

static const tide_grid_dir *dir_of(const tide_heap *heap, const tide_grid g)
{
    return (const tide_grid_dir *)(const void *)(tide_heap_block(heap, OFFSET(g.at)) + 1);
}

// Chunk places, 3 ints each, in order: by z, then y, then x.
static int compare_places(const void *a, const void *b)
{
    const int32_t *p = a;
    const int32_t *q = b;
    for (int i = 2; i >= 0; i--) {
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    }
    return 0;
}

// Puts `count` places in order and drops the repeats: how many are left.
static uint32_t order_places(int32_t *places, const uint32_t count)
{
    if (!count) return 0;
    qsort(places, count, 3 * sizeof(int32_t), compare_places);
    uint32_t kept = 1;
    for (uint32_t i = 1; i < count; i++) {
        if (compare_places(&places[3u * i], &places[3u * (kept - 1u)]) == 0) continue;
        memcpy(&places[3u * kept++], &places[3u * i], 3 * sizeof(int32_t));
    }
    return kept;
}

// Chunks past a tile its blocks get into: as many tiles before a chunk have
// blocks that get into it.
static int32_t block_reach(const int32_t side, const int32_t block)
{
    return (side + block - 2) / side;
}

// The blocks that start in tile `t`, within the size, by index on each axis.
static void tile_blocks(const tide_par_loop *l, const int32_t t[3], int32_t from[3], int32_t to[3])
{
    for (uint32_t a = 0; a < 3; a++) {
        const int32_t side = (int32_t)(1u << l->shape->shift[a]);
        const int32_t start = t[a] * side; // Its first cell
        const int32_t o = l->offset[a], b = l->block[a];
        from[a] = -floor_div(o - start, b); // The first block that starts in it...
        to[a] = floor_div(start + side - 1 - o, b) + 1; // ...and past the last
        if (from[a] < l->lo[a]) from[a] = l->lo[a];
        if (to[a] > l->hi[a]) to[a] = l->hi[a];
    }
}

void tide_par_begin(tide_par_loop *l, tide_grid *g, const tide_grid_shape *s, const uint32_t where, const int32_t block[3],
                    const int32_t offset[3])
{
    *l = (tide_par_loop){.grid = g, .shape = s, .where = where};
    if (!g->at || g->at >> 30 != where) return; // Not a world's: nothing to go through
    const tide_heap *heap = tide_heap_of(where);
    const tide_grid_dir *d = dir_of(heap, *g);
    int32_t side[3], reach[3];
    uint64_t per_tile = 1; // Blocks in a tile, about
    for (uint32_t a = 0; a < 3; a++) {
        const bool axis = a < s->dims;
        const int32_t b = axis && block[a] > 0 ? block[a] : 1;
        const int32_t o = axis ? offset[a] : 0;
        l->block[a] = b;
        l->offset[a] = o;
        side[a] = (int32_t)(1u << s->shift[a]);
        reach[a] = block_reach(side[a], b);
        per_tile *= (uint64_t)((side[a] + b - 1) / b);
        if (axis && !d->size[a]) {
            l->open = true;
            l->lo[a] = INT32_MIN;
            l->hi[a] = INT32_MAX;
        } else {
            const int32_t size = axis ? d->size[a] : 1;
            l->lo[a] = -floor_div(o, b);              // The first block from 0 on...
            l->hi[a] = floor_div(size - o - b, b) + 1; // ...and past the last one wholly inside
            if (l->lo[a] >= l->hi[a]) return;
        }
    }

    // Its tiles: every chunk place that sized blocks start in, or with an
    // open axis, the places of chunks the grid has and of those before them
    // whose blocks get into them
    uint32_t count = 0;
    if (!l->open) {
        int32_t first[3], last[3];
        uint64_t total = 1;
        for (uint32_t a = 0; a < 3; a++) {
            first[a] = floor_div(l->offset[a] + l->lo[a] * l->block[a], side[a]);
            last[a] = floor_div(l->offset[a] + (l->hi[a] - 1) * l->block[a], side[a]);
            total *= (uint64_t)(last[a] - first[a] + 1);
        }
        l->tiles = tide_alloc((size_t)total * 3u * sizeof(int32_t));
        for (int32_t z = first[2]; z <= last[2]; z++) {
            for (int32_t y = first[1]; y <= last[1]; y++) {
                for (int32_t x = first[0]; x <= last[0]; x++) {
                    int32_t *t = &l->tiles[3u * count++];
                    t[0] = x;
                    t[1] = y;
                    t[2] = z;
                }
            }
        }
    } else {
        const uint32_t around = (uint32_t)((reach[0] + 1) * (reach[1] + 1) * (reach[2] + 1));
        l->tiles = tide_alloc(((size_t)d->records * around + 1u) * 3u * sizeof(int32_t));
        for (uint32_t i = 0; i < d->records; i++) {
            const int32_t *c = tide_grid_records(d)[i].chunk;
            for (int32_t dz = 0; dz <= reach[2]; dz++) {
                for (int32_t dy = 0; dy <= reach[1]; dy++) {
                    for (int32_t dx = 0; dx <= reach[0]; dx++) {
                        int32_t *t = &l->tiles[3u * count];
                        t[0] = c[0] - dx;
                        t[1] = c[1] - dy;
                        t[2] = c[2] - dz;
                        int32_t from[3], to[3];
                        tile_blocks(l, t, from, to);
                        if (from[0] < to[0] && from[1] < to[1] && from[2] < to[2]) count++; // Blocks within its size start there
                    }
                }
            }
        }
        count = order_places(l->tiles, count);
    }
    l->tasks = count;
    l->steps = per_tile * count;

    // The chunks its blocks can write in: each tile's, and those its reach gets into
    const uint32_t around = (uint32_t)((reach[0] + 1) * (reach[1] + 1) * (reach[2] + 1));
    l->chunk = tide_alloc(((size_t)count * around + 1u) * 3u * sizeof(int32_t));
    uint32_t chunks = 0;
    for (uint32_t i = 0; i < count; i++) {
        const int32_t *t = &l->tiles[3u * i];
        for (int32_t dz = 0; dz <= reach[2]; dz++) {
            for (int32_t dy = 0; dy <= reach[1]; dy++) {
                for (int32_t dx = 0; dx <= reach[0]; dx++) {
                    int32_t *c = &l->chunk[3u * chunks++];
                    c[0] = t[0] + dx;
                    c[1] = t[1] + dy;
                    c[2] = t[2] + dz;
                }
            }
        }
    }
    l->chunks = order_places(l->chunk, chunks);
    l->buffers = tide_alloc_zeroed(l->chunks + 1u, sizeof(uint8_t *));
}

bool tide_par_tile(const tide_par_loop *l, const uint32_t task, int32_t from[3], int32_t to[3])
{
    const int32_t *t = &l->tiles[3u * task];
    tile_blocks(l, t, from, to);
    if (!l->open) return false;
    const tide_grid_dir *d = dir_of(tide_heap_of(l->where), *l->grid);
    return !tide_grid_find(d, t[0], t[1], t[2]);
}

bool tide_par_live(const tide_par_loop *l, const int32_t x, const int32_t y, const int32_t z)
{
    const tide_grid_shape *s = l->shape;
    const tide_grid_dir *d = dir_of(tide_heap_of(l->where), *l->grid);
    const int32_t at[3] = {x, y, z};
    int32_t first[3], last[3];
    for (uint32_t a = 0; a < 3; a++) {
        first[a] = at[a] >> s->shift[a];
        last[a] = (at[a] + l->block[a] - 1) >> s->shift[a];
    }
    for (int32_t cz = first[2]; cz <= last[2]; cz++) {
        for (int32_t cy = first[1]; cy <= last[1]; cy++) {
            for (int32_t cx = first[0]; cx <= last[0]; cx++) {
                if (tide_grid_find(d, cx, cy, cz)) return true;
            }
        }
    }
    return false;
}

// The chunk at (cx, cy, cz) as it is, or NULL for none.
static const uint8_t *chunk_now(const tide_par_loop *l, const int32_t cx, const int32_t cy, const int32_t cz)
{
    const tide_heap *heap = tide_heap_of(l->where);
    const tide_grid_dir *d = dir_of(heap, *l->grid);
    if (!d->records) return NULL;
    const uint32_t i = tide_grid_find(d, cx, cy, cz);
    return i ? tide_grid_chunk_at(heap, tide_grid_records(d)[i - 1u].block) : NULL;
}

// Buffers for parallel loops' chunks, kept for the next loop rather than
// freed, as allocating and freeing a block this size takes about as long as a
// step through all its cells. A loop's tasks take one for each chunk they
// write in, on any thread, and its end gives them back.
#define POOL_SIZES 8          // Chunk sizes it keeps buffers of
#define POOL_MOST (32u << 20) // Bytes it keeps, at most

typedef struct pooled {
    struct pooled *next;
} pooled;

static struct {
    uint32_t bytes; // 0 for a size not taken yet
    pooled *first;
} pools[POOL_SIZES];
static uint32_t pool_kept; // Bytes
static uint32_t pool_lock;

// The pool's lock: held for a few instructions, so waiting spins (as the web's
// main thread has to).
static void pool_enter(void)
{
    while (__atomic_exchange_n(&pool_lock, 1u, __ATOMIC_ACQUIRE)) {
    }
}

static void pool_leave(void)
{
    __atomic_store_n(&pool_lock, 0u, __ATOMIC_RELEASE);
}

static uint8_t *pool_take(const uint32_t bytes)
{
    pooled *got = NULL;
    pool_enter();
    for (uint32_t i = 0; i < POOL_SIZES && !got; i++) {
        if (pools[i].bytes != bytes || !pools[i].first) continue;
        got = pools[i].first;
        pools[i].first = got->next;
        pool_kept -= bytes;
    }
    pool_leave();
    if (!got) return tide_alloc(bytes);
    tide_memory_sync(); // Given back by another thread, maybe
    return (uint8_t *)got;
}

static void pool_give(uint8_t *buffer, const uint32_t bytes)
{
    bool kept = false;
    pool_enter();
    for (uint32_t i = 0; i < POOL_SIZES && !kept && pool_kept + bytes <= POOL_MOST; i++) {
        if (pools[i].bytes && pools[i].bytes != bytes) continue;
        pools[i].bytes = bytes;
        ((pooled *)(void *)buffer)->next = pools[i].first;
        pools[i].first = (pooled *)(void *)buffer;
        pool_kept += bytes;
        kept = true;
    }
    pool_leave();
    if (!kept) free(buffer);
}

uint8_t *tide_par_buffer(tide_par_loop *l, const int32_t cx, const int32_t cy, const int32_t cz)
{
    // Where it is among the chunks the loop's blocks can write in, which is
    // every chunk its steps write in
    const int32_t place[3] = {cx, cy, cz};
    uint32_t low = 0, high = l->chunks;
    while (low + 1u < high) {
        const uint32_t middle = (low + high) / 2u;
        if (compare_places(&l->chunk[3u * middle], place) <= 0) low = middle;
        else high = middle;
    }
    uint8_t *made = __atomic_load_n(&l->buffers[low], __ATOMIC_ACQUIRE);
    if (made) {
        tide_memory_sync(); // Made on another thread, maybe
        return made;
    }
    const uint32_t bytes = tide_grid_chunk_bytes(l->shape);
    uint8_t *buffer = pool_take(bytes);
    const uint8_t *now = chunk_now(l, cx, cy, cz);
    if (now) memcpy(buffer, now, bytes);
    else memset(buffer, 0, bytes);
    if (__atomic_compare_exchange_n(&l->buffers[low], &made, buffer, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return buffer;
    }
    pool_give(buffer, bytes); // Another task's got there first: `made` is that one
    tide_memory_sync();
    return made;
}

// The chunks a window takes in, each looked up once: a few around a tile, or
// one at a time for a window too big for that.
#define WINDOW_CHUNKS 27

typedef struct window_chunks {
    int32_t first[3]; // The first chunk, in chunks
    int32_t count[3]; // Chunks along each axis
    bool listed;      // count fits in `cells`: each looked up already
    const uint8_t *cells[WINDOW_CHUNKS]; // By z, then y, then x; NULL where the grid has none
} window_chunks;

static void window_chunks_find(const tide_par_loop *l, window_chunks *w, const int32_t lo[3], const int32_t hi[3])
{
    const tide_grid_shape *s = l->shape;
    int64_t total = 1;
    for (uint32_t a = 0; a < 3; a++) {
        w->first[a] = lo[a] >> s->shift[a];
        w->count[a] = (hi[a] >> s->shift[a]) - w->first[a] + 1;
        total *= w->count[a];
    }
    w->listed = total <= WINDOW_CHUNKS;
    for (int32_t i = 0; w->listed && i < (int32_t)total; i++) {
        const int32_t cx = w->first[0] + i % w->count[0];
        const int32_t cy = w->first[1] + i / w->count[0] % w->count[1];
        const int32_t cz = w->first[2] + i / (w->count[0] * w->count[1]);
        w->cells[i] = chunk_now(l, cx, cy, cz);
    }
}

static const uint8_t *window_chunk(const tide_par_loop *l, const window_chunks *w, const int32_t cx, const int32_t cy,
                                   const int32_t cz)
{
    if (!w->listed) return chunk_now(l, cx, cy, cz);
    return w->cells[((cz - w->first[2]) * w->count[1] + (cy - w->first[1])) * w->count[0] + (cx - w->first[0])];
}

// memcpy and memset for the few cells along a window's edges, which a call to
// them costs more than: two moves of a fixed size that overlap as they need.
static void copy_cells(uint8_t *to, const uint8_t *from, const size_t bytes)
{
    if (bytes > 16u) {
        memcpy(to, from, bytes);
    } else if (bytes >= 8u) {
        memcpy(to, from, 8u);
        memcpy(to + bytes - 8u, from + bytes - 8u, 8u);
    } else if (bytes >= 4u) {
        memcpy(to, from, 4u);
        memcpy(to + bytes - 4u, from + bytes - 4u, 4u);
    } else if (bytes >= 2u) {
        memcpy(to, from, 2u);
        memcpy(to + bytes - 2u, from + bytes - 2u, 2u);
    } else if (bytes) {
        *to = *from;
    }
}

static void zero_cells(uint8_t *to, const size_t bytes)
{
    static const uint8_t zeros[16];
    if (bytes > 16u) memset(to, 0, bytes);
    else copy_cells(to, zeros, bytes);
}

void tide_par_window(const tide_par_loop *l, const int32_t lo[3], const int32_t hi[3], void *cells)
{
    const tide_grid_shape *s = l->shape;
    const tide_grid_dir *d = dir_of(tide_heap_of(l->where), *l->grid);
    const size_t cell = s->cell;
    const size_t row = (size_t)((int64_t)hi[0] - lo[0] + 1) * cell;
    uint8_t *out = cells;
    window_chunks cache;
    window_chunks_find(l, &cache, lo, hi);
    for (int32_t z = lo[2]; z <= hi[2]; z++) {
        for (int32_t y = lo[1]; y <= hi[1]; y++, out += row) {
            // The row's cells within the grid's size, from x0 to x1: zero around them
            int64_t x0 = lo[0], x1 = hi[0];
            if (d->size[0]) {
                if (x0 < 0) x0 = 0;
                if (x1 > (int64_t)d->size[0] - 1) x1 = (int64_t)d->size[0] - 1;
            }
            const bool inside = (!d->size[1] || (y >= 0 && y < d->size[1])) && (!d->size[2] || (z >= 0 && z < d->size[2]));
            if (!inside || !d->records || x0 > x1) {
                zero_cells(out, row);
                continue;
            }
            if (x0 > lo[0]) zero_cells(out, (size_t)(x0 - lo[0]) * cell);
            if (x1 < hi[0]) zero_cells(out + (size_t)(x1 + 1 - lo[0]) * cell, (size_t)(hi[0] - x1) * cell);
            for (int64_t x = x0; x <= x1;) { // Chunk by chunk
                const int32_t cx = (int32_t)(x >> s->shift[0]);
                const int64_t last = ((int64_t)cx + 1) * ((int64_t)1 << s->shift[0]) - 1;
                const int64_t end = last < x1 ? last : x1;
                const size_t bytes = (size_t)(end - x + 1) * cell;
                uint8_t *to = out + (size_t)(x - lo[0]) * cell;
                const uint8_t *chunk = window_chunk(l, &cache, cx, y >> s->shift[1], z >> s->shift[2]);
                if (chunk) copy_cells(to, chunk + tide_grid_place(s, (int32_t)x, y, z), bytes);
                else zero_cells(to, bytes);
                x = end + 1;
            }
        }
    }
}

void tide_par_window_put(tide_par_loop *l, const int32_t lo[3], const int32_t hi[3], const int32_t from[3],
                         const int32_t to[3], const void *cells, const void *was)
{
    const tide_grid_shape *s = l->shape;
    const size_t cell = s->cell;
    const size_t width = (size_t)((int64_t)hi[0] - lo[0] + 1), height = (size_t)((int64_t)hi[1] - lo[1] + 1);
    const uint8_t *now = cells, *before = was;
    tide_par_cache cache = {{0, 0, 0}, NULL};
    for (int32_t z = from[2]; z <= to[2]; z++) {
        for (int32_t y = from[1]; y <= to[1]; y++) {
            const size_t row = ((size_t)(z - lo[2]) * height + (size_t)(y - lo[1])) * width;
            for (int64_t x = from[0]; x <= to[0];) { // Chunk by chunk
                const int64_t last = (((int64_t)x >> s->shift[0]) + 1) * ((int64_t)1 << s->shift[0]) - 1;
                const int64_t end = last < to[0] ? last : to[0];
                const size_t at = (row + (size_t)(x - lo[0])) * cell, bytes = (size_t)(end - x + 1) * cell;
                if (memcmp(now + at, before + at, bytes) != 0) memcpy(tide_par_cell(l, &cache, (int32_t)x, y, z), now + at, bytes);
                x = end + 1;
            }
        }
    }
}

void tide_par_end(tide_par_loop *l)
{
    tide_memory_sync(); // What the steps made on other threads
    const tide_grid_shape *s = l->shape;
    tide_heap *heap = l->buffers ? tide_heap_of(l->where) : NULL;
    for (uint32_t i = 0; i < l->chunks && l->buffers; i++) {
        uint8_t *buffer = l->buffers[i];
        if (!buffer) continue;
        const uint32_t bytes = tide_grid_chunk_bytes(s);
        const int32_t *c = &l->chunk[3u * i];
        const uint8_t *now = chunk_now(l, c[0], c[1], c[2]);
        bool same = now && memcmp(now, buffer, bytes) == 0;
        if (!now) { // Nothing there before: only a chunk if something isn't zero
            same = true;
            for (uint32_t k = 0; k < bytes && same; k++) same = buffer[k] == 0;
        }
        if (!same) {
            const tide_grid_dir *d = dir_of(heap, *l->grid);
            const uint32_t found = d->records ? tide_grid_find(d, c[0], c[1], c[2]) : 0u;
            if (found) memcpy(tide_heap_write(heap, tide_grid_records(d)[found - 1u].block), buffer, bytes);
            else add_chunk(l->grid, heap, c[0], c[1], c[2], buffer, s);
        }
        pool_give(buffer, bytes);
    }
    free(l->buffers);
    free(l->chunk);
    free(l->tiles);
    l->buffers = NULL;
    l->chunk = NULL;
    l->tiles = NULL;
}

// In order, a row at a time

// The cells of chunk place `c` along axis `a`, within the grid's size there:
// false when there are none.
static bool cells_along(const tide_grid_rows *r, const uint32_t a, const int32_t c, int32_t *from, int32_t *to)
{
    *from = c * r->side[a];
    *to = *from + r->side[a];
    if (r->size[a]) {
        if (*from < 0) *from = 0;
        if (*to > r->size[a]) *to = r->size[a];
    }
    return *from < *to;
}

// Past the chunks from `i` on that share its place on axes `a` and up.
static uint32_t same_until(const tide_grid_rows *r, const uint32_t i, const uint32_t a)
{
    uint32_t k = i + 1u;
    while (k < r->count && memcmp(&r->chunk[3u * k + a], &r->chunk[3u * i + a], (3u - a) * sizeof(int32_t)) == 0) k++;
    return k;
}

// The first row of chunks from `i` on in the current layer that has cells
// within the size, from its first row of cells. False when there's none.
static bool enter_row(tide_grid_rows *r, uint32_t i)
{
    for (; i < r->layer_end; i = r->row_end) {
        r->row = i;
        r->row_end = same_until(r, i, 1);
        r->next = i;
        if (cells_along(r, 1, r->chunk[3u * i + 1u], &r->y, &r->y_end)) return true;
    }
    return false;
}

// The first layer of chunks from `i` on that has cells within the size, from
// its first row. False when there's none.
static bool enter_layer(tide_grid_rows *r, uint32_t i)
{
    for (; i < r->count; i = r->layer_end) {
        r->layer = i;
        r->layer_end = same_until(r, i, 2);
        if (cells_along(r, 2, r->chunk[3u * i + 2u], &r->z, &r->z_end) && enter_row(r, i)) return true;
    }
    return false;
}

void tide_grid_rows_begin(tide_grid_rows *r, const tide_grid g, const tide_grid_shape *s)
{
    *r = (tide_grid_rows){.done = true};
    const tide_heap *heap = tide_heap_of(g.at >> 30);
    if (!heap || !g.at) return;
    const tide_grid_dir *d = dir_of(heap, g);
    bool open = false;
    for (uint32_t a = 0; a < 3; a++) {
        r->size[a] = a < s->dims ? d->size[a] : 1;
        r->side[a] = (int32_t)(1u << s->shift[a]);
        open |= r->size[a] == 0;
    }
    if (!open) { // Every cell within its size
        r->y_end = r->size[1];
        r->z_end = r->size[2];
        r->done = false;
        return;
    }
    if (!d->records) return;
    int32_t *places = tide_scratch_memory((size_t)d->records * 3u * sizeof(int32_t));
    for (uint32_t i = 0; i < d->records; i++) memcpy(&places[3u * i], tide_grid_records(d)[i].chunk, 3 * sizeof(int32_t));
    r->count = order_places(places, d->records);
    r->chunk = places;
    r->done = !enter_layer(r, 0);
}

bool tide_grid_rows_next(tide_grid_rows *r, int32_t *x0, int32_t *x1, int32_t *y, int32_t *z)
{
    if (r->done) return false;
    if (!r->chunk) { // Sized: a row at a time, each whole
        if (r->y >= r->y_end) {
            r->y = 0;
            if (++r->z >= r->z_end || r->y_end == 0) {
                r->done = true;
                return false;
            }
        }
        *x0 = 0;
        *x1 = r->size[0];
        *y = r->y++;
        *z = r->z;
        return true;
    }
    for (;;) {
        if (r->next < r->row_end) { // Along the row: the chunks side by side from the next, as one run
            const uint32_t first = r->next++;
            while (r->next < r->row_end && r->chunk[3u * r->next] == r->chunk[3u * (r->next - 1u)] + 1) r->next++;
            int32_t from, to, unused;
            const bool starts = cells_along(r, 0, r->chunk[3u * first], &from, &unused);
            const bool ends = cells_along(r, 0, r->chunk[3u * (r->next - 1u)], &unused, &to);
            if (!starts || !ends || from >= to) continue;
            *x0 = from;
            *x1 = to;
            *y = r->y;
            *z = r->z;
            return true;
        }
        if (r->y + 1 < r->y_end) { // The next row of cells in these chunks
            r->y++;
            r->next = r->row;
            continue;
        }
        if (enter_row(r, r->row_end)) continue; // The next row of chunks in the layer
        if (r->z + 1 < r->z_end && enter_row(r, r->layer)) { // The layer's next z, from its first row of chunks
            r->z++;
            continue;
        }
        if (enter_layer(r, r->layer_end)) continue;
        r->done = true;
        return false;
    }
}
