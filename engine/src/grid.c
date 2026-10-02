#include "tide/grid.h"

#include <stdlib.h>
#include <string.h>

#include "tide/jobs.h"

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

// A new chunk at (cx, cy, cz), in chunks, with `cells` in it (NULL: zero),
// made this tick. Returns its cells.
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
    tide_grid_records(d)[d->records] = (tide_grid_record){{cx, cy, cz}, chunk, heap->tick, heap->tick};
    insert(d, d->records++);
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
    if (i) {
        tide_grid_dir *w = dir_at(heap, OFFSET(g->at));
        tide_grid_records(w)[i - 1u].changed = heap->tick;
        return (uint8_t *)tide_heap_write(heap, tide_grid_records(w)[i - 1u].block) + tide_grid_place(s, x, y, z);
    }
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
// Chunk systems

uint32_t tide_grid_chunks(const tide_grid g)
{
    tide_heap *heap = tide_heap_of(g.at >> 30);
    if (!g.at || !heap) return 0;
    return dir_at(heap, OFFSET(g.at))->records;
}

bool tide_grid_view_start(tide_grid_view *v, const tide_grid g, const tide_entity entity, const uint32_t record,
                          const tide_grid_shape *s, const tide_grid_reach *reach, const uint32_t phase, const bool sleeps)
{
    tide_heap *heap = tide_heap_of(g.at >> 30);
    if (!g.at || !heap) return false;
    const tide_grid_dir *d = (const tide_grid_dir *)(const void *)(tide_heap_block(heap, OFFSET(g.at)) + 1);
    if (record >= d->records) return false;
    const tide_grid_record *r = &tide_grid_records(d)[record];
    if (r->born == heap->tick) return false; // Made this tick: from the next one on
    if (reach->phases > 1) {
        int64_t sum = 0;
        for (uint32_t i = 0; i < s->dims; i++) sum += (int64_t)reach->weight[i] * r->chunk[i];
        const int64_t phases = reach->phases;
        if ((uint32_t)(((sum % phases) + phases) % phases) != phase) return false;
    }

    memset(v, 0, sizeof *v);
    v->g = g;
    v->shape = s;
    v->heap = heap;
    v->tick = heap->tick;
    v->entity = entity;
    v->chunks = reach->chunks | 1u << 13; // Its own, always
    bool awake = !sleeps;
    for (uint32_t i = 0; i < 3; i++) {
        const bool axis = i < s->dims;
        // Never more than a chunk past its own, which tidec checks too
        const int32_t span = (int32_t)(1u << s->shift[i]);
        const int32_t before = reach->before[i] < span ? reach->before[i] : span;
        const int32_t after = reach->after[i] < span ? reach->after[i] : span;
        v->center[i] = r->chunk[i];
        v->min[i] = axis ? r->chunk[i] << s->shift[i] : 0;
        v->max[i] = axis ? v->min[i] + span : 1;
        v->low[i] = axis ? v->min[i] - before : 0;
        v->high[i] = axis ? v->max[i] + after : 1;
        if (axis && d->size[i]) { // Never past the grid's size
            if (v->low[i] < 0) v->low[i] = 0;
            if (v->high[i] > d->size[i]) v->high[i] = d->size[i];
            if (v->max[i] > d->size[i]) v->max[i] = d->size[i];
        }
    }
    for (uint32_t a = 0; a < 27; a++) {
        if (!(v->chunks >> a & 1u)) continue;
        const int32_t x = (int32_t)(a % 3u) - 1;
        const int32_t y = (int32_t)(a / 3u % 3u) - 1;
        const int32_t z = (int32_t)(a / 9u) - 1;
        const uint32_t i = tide_grid_find(d, r->chunk[0] + x, r->chunk[1] + y, r->chunk[2] + z);
        if (!i) continue;
        const tide_grid_record *n = &tide_grid_records(d)[i - 1u];
        v->record[a] = i;
        v->cells[a] = tide_grid_chunk_at(heap, n->block);
        if (n->changed + 1u >= v->tick) awake = true;
    }
    return awake;
}

uint8_t *tide_grid_view_take(tide_grid_view *v, const uint32_t around, const bool make)
{
    if (v->record[around]) {
        // The directory is this world's own already (tide_grid_chunks), and no
        // other task touches this record or this chunk
        if (!v->dir) v->dir = dir_at(v->heap, OFFSET(v->g.at));
        tide_grid_record *r = &tide_grid_records(v->dir)[v->record[around] - 1u];
        r->changed = v->tick;
        // Other tasks change other chunks, which can be in the same page
        v->mine[around] = (uint8_t *)tide_heap_write_parallel(v->heap, r->block, &v->copied[around]);
    } else {
        if (!make) return NULL;
        v->mine[around] = tide_alloc_zeroed(1, tide_grid_chunk_bytes(v->shape));
    }
    v->cells[around] = v->mine[around];
    return v->mine[around];
}

void tide_grid_view_end(tide_grid_view *v)
{
    void **data = tide_task_data();
    for (uint32_t a = 0; a < 27; a++) {
        const bool made_one = !v->record[a] && v->mine[a];
        if (!made_one && !v->copied[a]) continue;
        if (!data) tide_out_of_memory();
        tide_grid_made *made = tide_alloc(sizeof *made);
        const int32_t offset[3] = {(int32_t)(a % 3u) - 1, (int32_t)(a / 3u % 3u) - 1, (int32_t)(a / 9u) - 1};
        *made = (tide_grid_made){NULL, v->entity, {v->center[0] + offset[0], v->center[1] + offset[1], v->center[2] + offset[2]},
                                 made_one ? v->mine[a] : NULL, v->copied[a]};
        // At the end of the task's list, so they're made in the order the task made them
        tide_grid_made **last = (tide_grid_made **)data;
        while (*last) last = &(*last)->next;
        *last = made;
    }
}

void tide_grid_finish(void **data, const uint32_t tasks, tide_grid *(*field)(void *world, tide_entity entity), void *world,
                      const tide_grid_shape *s)
{
    for (uint32_t k = 0; k < tasks; k++) {
        tide_grid_made *made = data[k];
        while (made) {
            tide_grid_made *next = made->next;
            tide_page_release(made->copied, 1u); // No task reads it any more
            tide_grid *g = made->cells ? field(world, made->entity) : NULL;
            tide_heap *heap = g ? tide_heap_of(g->at >> 30) : NULL;
            if (heap && g->at) add_chunk(g, heap, made->chunk[0], made->chunk[1], made->chunk[2], made->cells, s);
            free(made->cells);
            free(made);
            made = next;
        }
        data[k] = NULL;
    }
}
