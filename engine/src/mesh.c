#include "tide/mesh.h"

#include <string.h>

#include "tide/net.h"

static uint8_t channel(const float v)
{
    if (!(v > 0.0f)) return 0; // Also NaN
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

// The grid's cells as pixels: its chunks' colors, a byte a channel, and clear
// where it has no chunk.
static void grid_pixels(const tide_grid_dir *dir, const tide_heap *heap, const tide_grid_shape *s, uint8_t *pixels)
{
    const int32_t width = dir->size[0], height = dir->size[1];
    memset(pixels, 0, (size_t)width * (size_t)height * 4u);
    const tide_grid_record *records = tide_grid_records(dir);
    const int32_t across = 1 << s->shift[0], down = 1 << s->shift[1];
    for (uint32_t r = 0; r < dir->records; r++) {
        const tide_color *cells = (const tide_color *)(const void *)tide_grid_chunk_at(heap, records[r].block);
        const int32_t x0 = records[r].chunk[0] * across, y0 = records[r].chunk[1] * down;
        for (int32_t y = 0; y < down && y0 + y < height; y++) {
            uint8_t *out = pixels + ((size_t)(y0 + y) * (size_t)width + (size_t)x0) * 4u;
            const tide_color *row = cells + (size_t)y * (size_t)across;
            for (int32_t x = 0; x < across && x0 + x < width; x++, out += 4) {
                out[0] = channel(row[x].r);
                out[1] = channel(row[x].g);
                out[2] = channel(row[x].b);
                out[3] = channel(row[x].a);
            }
        }
    }
}

// The texture a grid of colors is, for a mesh command, or 0 for none: a grid
// with no size on an axis.
static uint32_t grid_slot(tide_draw_list *d, const tide_grid g, const tide_grid_shape *s)
{
    const tide_grid_dir *dir = tide_grid_dir_of(g);
    tide_heap *heap = tide_heap_of(g.at >> 30);
    if (!dir || !heap || dir->dims != 2u || dir->size[0] <= 0 || dir->size[1] <= 0 || s->cell != sizeof(tide_color)) {
        return 0;
    }
    // What its cells are: its size, and each chunk's place and hash, which its
    // heap keeps until the chunk changes
    const bool whole_pages = tide_grid_chunk_bytes(s) == 1u << TIDE_HEAP_PAGE_SHIFT;
    const tide_grid_record *records = tide_grid_records(dir);
    uint64_t version = tide_hash_more(TIDE_HASH_START, dir->size, sizeof dir->size);
    for (uint32_t r = 0; r < dir->records; r++) {
        const uint64_t cells = whole_pages ? tide_heap_block_hash(heap, records[r].block)
                                           : tide_hash(tide_grid_chunk_at(heap, records[r].block), tide_grid_chunk_bytes(s));
        version = tide_hash_more(version, records[r].chunk, sizeof records[r].chunk);
        version = tide_hash_more(version, &cells, sizeof cells);
    }
    uint8_t *pixels;
    const uint32_t slot = tide_draw_texture_slot(d, TIDE_PIXELS_GRID, g.at, tide_hash_end(version), dir->size[0],
                                                 dir->size[1], &pixels);
    if (pixels) grid_pixels(dir, heap, s, pixels);
    return slot;
}

static void mesh_lists(tide_draw_list *d, const tide_list vertices, const tide_list indices, const uint32_t slot,
                       const tide_filter filter)
{
    const int32_t vertex_count = tide_list_count(vertices);
    const int32_t index_count = tide_list_count(indices);
    if (vertex_count <= 0 || index_count < 3) return;
    // A list's elements side by side, as they are unless it's a world's big one
    bool vertices_copied, indices_copied;
    tide_vertex *v = tide_list_flatten(vertices, sizeof(tide_vertex), false, &vertices_copied);
    uint32_t *i = tide_list_flatten(indices, sizeof(uint32_t), false, &indices_copied);
    tide_draw_triangles_with(d, tide_draw_vertices(d, v, (uint32_t)vertex_count), i, (uint32_t)index_count, slot, filter);
    tide_list_unflatten(indices, i, sizeof(uint32_t), false, indices_copied);
    tide_list_unflatten(vertices, v, sizeof(tide_vertex), false, vertices_copied);
}

void tide_draw_mesh_lists(tide_draw_list *d, const tide_list vertices, const tide_list indices)
{
    mesh_lists(d, vertices, indices, 0, TIDE_FILTER_BILINEAR);
}

void tide_draw_mesh_grid(tide_draw_list *d, const tide_list vertices, const tide_list indices, const tide_grid texture,
                         const tide_grid_shape *shape, const int32_t filter)
{
    mesh_lists(d, vertices, indices, grid_slot(d, texture, shape),
               filter == TIDE_FILTER_POINT ? TIDE_FILTER_POINT : TIDE_FILTER_BILINEAR);
}

// ---------------------------------------------------------------------------
// 3D

// What's in a list, as a version: its count, and what its elements are. A
// world's list says it without reading them: the hashes its heap keeps of
// its pages until they change. Elements side by side share their page with
// other blocks, whose changes count as theirs, but they're under a page, so
// copying them again costs little. One made along the way, in the scratch
// area, is read.
static uint64_t list_version(const tide_list l, const uint32_t size)
{
    const int32_t count = tide_list_count(l);
    uint64_t h = tide_hash_more(TIDE_HASH_START, &count, sizeof count);
    if (count <= 0) return tide_hash_end(h);
    if (l.at >> 30 == TIDE_IN_SCRATCH) {
        return tide_hash_end(tide_hash_more(h, tide_list_at(l, 0, size), (size_t)count * size));
    }
    tide_heap *heap = tide_heap_of(l.at >> 30);
    if (!heap) return tide_hash_end(h);
    const uint32_t block = l.at & 0x3FFFFFFFu;
    const tide_block *b = tide_heap_block(heap, block);
    if (!(b->b & TIDE_LIST_CHUNKED)) {
        const uint64_t page = tide_heap_block_hash(heap, block);
        return tide_hash_end(tide_hash_more(h, &page, sizeof page));
    }
    // In chunks, a page each: where they are, and what's in each
    const uint32_t chunks = b->b & ~TIDE_LIST_CHUNKED;
    const uint32_t *at = (const uint32_t *)(const void *)(b + 1);
    h = tide_hash_more(h, at, (size_t)chunks * sizeof(uint32_t));
    for (uint32_t i = 0; i < chunks; i++) {
        const uint64_t page = tide_heap_block_hash(heap, at[i]);
        h = tide_hash_more(h, &page, sizeof page);
    }
    return tide_hash_end(h);
}

static void mesh3_lists(tide_draw_list *d, const tide_list vertices, const tide_list indices,
                        const tide_float4x4 transform, const uint32_t texture, const tide_filter filter)
{
    const int32_t vertex_count = tide_list_count(vertices);
    const int32_t index_count = tide_list_count(indices);
    if (vertex_count <= 0 || index_count < 3) return;
    // A mesh is its two lists at their versions. A list made along the way
    // is what's in it, wherever it is in the scratch area: a view that runs
    // for each entity makes `[0, 1, 2]` in another place each time, and its
    // instances are still one mesh's
    uint64_t version = list_version(vertices, sizeof(tide_vertex3));
    const uint64_t index_version = list_version(indices, sizeof(uint32_t));
    version = tide_hash_end(tide_hash_more(version, &index_version, sizeof index_version));
    const uint64_t scratch = (uint64_t)TIDE_IN_SCRATCH << 32; // Past every list's place
    const uint64_t id = vertices.at >> 30 == TIDE_IN_SCRATCH ? scratch : vertices.at;
    const uint64_t other = indices.at >> 30 == TIDE_IN_SCRATCH ? scratch : indices.at;
    bool fill;
    const uint32_t mesh = tide_draw_mesh_slot(d, TIDE_MESH_LISTS, id, other, version, &fill);
    if (fill) {
        bool vertices_copied, indices_copied;
        tide_vertex3 *v = tide_list_flatten(vertices, sizeof(tide_vertex3), false, &vertices_copied);
        uint32_t *i = tide_list_flatten(indices, sizeof(uint32_t), false, &indices_copied);
        tide_draw_mesh_fill(d, mesh, v, (uint32_t)vertex_count, i, (uint32_t)index_count);
        tide_list_unflatten(indices, i, sizeof(uint32_t), false, indices_copied);
        tide_list_unflatten(vertices, v, sizeof(tide_vertex3), false, vertices_copied);
    }
    tide_draw_instance(d, mesh, transform, texture, filter);
}

void tide_draw_mesh3_lists(tide_draw_list *d, const tide_list vertices, const tide_list indices,
                           const tide_float4x4 transform)
{
    mesh3_lists(d, vertices, indices, transform, 0, TIDE_FILTER_BILINEAR);
}

void tide_draw_mesh3_grid(tide_draw_list *d, const tide_list vertices, const tide_list indices,
                          const tide_float4x4 transform, const tide_grid texture, const tide_grid_shape *shape,
                          const int32_t filter)
{
    mesh3_lists(d, vertices, indices, transform, grid_slot(d, texture, shape),
                filter == TIDE_FILTER_POINT ? TIDE_FILTER_POINT : TIDE_FILTER_BILINEAR);
}
