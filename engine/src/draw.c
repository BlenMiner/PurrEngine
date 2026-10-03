#include "tide/draw.h"

#include <stdlib.h>
#include <string.h>

#include "tide/net.h"
#include "tide/page.h"

static void reindex_meshes(tide_draw_list *d);

void tide_draw_reset(tide_draw_list *d)
{
    d->count = 0;
    d->text_used = 0;
    d->vertex_count = 0;
    d->index_count = 0;
    d->matrix_count = 0;
    d->clipped = 0;
    // The textures the frame drew with stay, for the next to find its pixels
    // here already
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->texture_count; i++) {
        tide_draw_texture *t = &d->textures[i];
        if (t->frame != d->frame) {
            free(t->pixels);
            continue;
        }
        d->textures[kept++] = *t;
    }
    d->texture_count = kept;
    // ...and so do its 3D meshes, each in its place
    bool freed = false;
    for (uint32_t i = 0; i < d->mesh_count; i++) {
        tide_draw_mesh_data *m = &d->meshes[i];
        if (m->space == TIDE_MESH_NONE || m->frame == d->frame) continue;
        free(m->vertices);
        free(m->indices);
        *m = (tide_draw_mesh_data){.next_free = d->mesh_free};
        d->mesh_free = i + 1u;
        freed = true;
    }
    if (freed) reindex_meshes(d);
    d->frame++;
}

static void free_meshes(tide_draw_list *d)
{
    for (uint32_t i = 0; i < d->mesh_count; i++) {
        free(d->meshes[i].vertices);
        free(d->meshes[i].indices);
    }
    free(d->meshes);
    free(d->mesh_index);
    d->meshes = NULL;
    d->mesh_count = d->mesh_capacity = d->mesh_free = 0;
    d->mesh_index = NULL;
    d->mesh_index_capacity = 0;
}

void tide_draw_free(tide_draw_list *d)
{
    for (uint32_t i = 0; i < d->texture_count; i++) free(d->textures[i].pixels);
    free_meshes(d);
    free(d->commands);
    free(d->text);
    free(d->vertices);
    free(d->indices);
    free(d->textures);
    free(d->matrices);
    *d = (tide_draw_list){0};
}

void tide_draw_forget(tide_draw_list *d)
{
    for (uint32_t i = 0; i < d->texture_count; i++) free(d->textures[i].pixels);
    d->texture_count = 0;
    free_meshes(d);
    d->epoch++;
    // Commands recorded already would draw with textures and meshes that are gone
    for (uint32_t i = 0; i < d->count; i++) {
        tide_draw_command *c = &d->commands[i];
        if (c->kind == TIDE_DRAW_MESH) c->mesh.texture = 0;
        if (c->kind == TIDE_DRAW_MESH_3D) c->mesh3.mesh = c->mesh3.texture = 0;
    }
}

// Room for `more` items of `size` bytes after the `count` there are, the
// array doubling as it needs.
static void *grow(void *items, const uint32_t count, uint32_t *capacity, const uint64_t more, const size_t size,
                  const uint32_t first)
{
    if (more <= *capacity - count) return items;
    uint64_t room = *capacity ? *capacity : first;
    while (room < count + more) room *= 2u;
    if (room > UINT32_MAX / size) tide_out_of_memory();
    items = tide_realloc(items, *capacity * size, (size_t)room * size);
    *capacity = (uint32_t)room;
    return items;
}

static void reserve(tide_draw_list *d, const uint32_t more)
{
    d->commands = grow(d->commands, d->count, &d->capacity, more, sizeof(tide_draw_command), 256u);
}

static tide_draw_command *push(tide_draw_list *d, const tide_draw_kind kind, const tide_float2 a, const tide_float2 b,
                               const tide_color color)
{
    reserve(d, 1);
    tide_draw_command *c = &d->commands[d->count++];
    *c = (tide_draw_command){.kind = (uint32_t)kind, .a = a, .b = b, .color = color};
    return c;
}

void tide_draw_clear(tide_draw_list *d, const tide_color color)
{
    push(d, TIDE_DRAW_CLEAR, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), color);
}

void tide_draw_camera(tide_draw_list *d, const tide_float2 center, const float size)
{
    push(d, TIDE_DRAW_CAMERA, center, tide_f2(size, 0.0f), TIDE_COLOR_CLEAR);
}

void tide_draw_circle(tide_draw_list *d, const tide_float2 center, const float radius, const tide_color color)
{
    push(d, TIDE_DRAW_CIRCLE, center, tide_f2(radius, 0.0f), color);
}

void tide_draw_wire_circle(tide_draw_list *d, const tide_float2 center, const float radius, const tide_color color)
{
    push(d, TIDE_DRAW_WIRE_CIRCLE, center, tide_f2(radius, 0.0f), color);
}

void tide_draw_rect(tide_draw_list *d, const tide_float2 center, const tide_float2 size, const tide_color color)
{
    push(d, TIDE_DRAW_RECT, center, size, color);
}

void tide_draw_wire_rect(tide_draw_list *d, const tide_float2 center, const tide_float2 size, const tide_color color)
{
    push(d, TIDE_DRAW_WIRE_RECT, center, size, color);
}

void tide_draw_line(tide_draw_list *d, const tide_float2 from, const tide_float2 to, const tide_color color)
{
    push(d, TIDE_DRAW_LINE, from, to, color);
}

void tide_draw_screen(tide_draw_list *d)
{
    push(d, TIDE_DRAW_SCREEN, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
}

void tide_draw_clip(tide_draw_list *d, const tide_rect rect)
{
    push(d, TIDE_DRAW_CLIP, tide_f2(rect.x, rect.y), tide_f2(rect.width, rect.height), TIDE_COLOR_CLEAR);
    d->clipped = 1;
}

void tide_draw_no_clip(tide_draw_list *d)
{
    push(d, TIDE_DRAW_NO_CLIP, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
    d->clipped = 0;
}

void tide_draw_text(tide_draw_list *d, const char *text, const tide_float2 position, const float size,
                    const tide_color color)
{
    const size_t bytes = strlen(text) + 1;
    d->text = grow(d->text, d->text_used, &d->text_capacity, bytes, 1, 4096u);
    tide_draw_command *c = push(d, TIDE_DRAW_TEXT, position, tide_f2(size, 0.0f), color);
    c->text = d->text_used;
    memcpy(d->text + d->text_used, text, bytes);
    d->text_used += (uint32_t)bytes;
}

// ---------------------------------------------------------------------------
// Meshes

uint32_t tide_draw_texture_slot(tide_draw_list *d, const uint32_t space, const uint64_t id, const uint64_t version,
                                const int32_t width, const int32_t height, uint8_t **pixels)
{
    *pixels = NULL;
    if (width <= 0 || height <= 0) return 0;
    const uint64_t bytes = (uint64_t)width * (uint64_t)height * 4u;
    if (bytes > UINT32_MAX) tide_out_of_memory();
    tide_draw_texture *t = NULL;
    for (uint32_t i = 0; i < d->texture_count && !t; i++) {
        if (d->textures[i].id == id && d->textures[i].space == space) t = &d->textures[i];
    }
    if (!t) {
        d->textures = grow(d->textures, d->texture_count, &d->texture_capacity, 1, sizeof(tide_draw_texture), 8u);
        t = &d->textures[d->texture_count++];
        *t = (tide_draw_texture){.id = id, .space = space, .epoch = d->epoch};
    } else if (t->version == version && t->width == width && t->height == height) {
        t->frame = d->frame;
        return (uint32_t)(t - d->textures) + 1u;
    }
    if (bytes > t->capacity) {
        t->pixels = tide_realloc(t->pixels, t->capacity, (size_t)bytes);
        t->capacity = (uint32_t)bytes;
    }
    t->version = version;
    t->width = width;
    t->height = height;
    t->frame = d->frame;
    *pixels = t->pixels;
    return (uint32_t)(t - d->textures) + 1u;
}

uint64_t tide_texture_version(const void *pixels, const int32_t width, const int32_t height)
{
    if (!pixels || width <= 0 || height <= 0) return 0;
    return tide_hash(pixels, (size_t)width * (size_t)height * 4u);
}

static uint32_t texture_slot(tide_draw_list *d, const tide_texture *texture)
{
    if (!texture || !texture->pixels) return 0;
    uint8_t *pixels;
    const uint32_t slot = tide_draw_texture_slot(d, TIDE_PIXELS_MEMORY, (uint64_t)(uintptr_t)texture->pixels,
                                                 texture->version, texture->width, texture->height, &pixels);
    if (pixels) memcpy(pixels, texture->pixels, (size_t)texture->width * (size_t)texture->height * 4u);
    return slot;
}

uint32_t tide_draw_vertices(tide_draw_list *d, const tide_vertex *vertices, const uint32_t count)
{
    const uint32_t base = d->vertex_count;
    if (count == 0) return base;
    d->vertices = grow(d->vertices, d->vertex_count, &d->vertex_capacity, count, sizeof(tide_vertex), 1024u);
    memcpy(d->vertices + base, vertices, (size_t)count * sizeof(tide_vertex));
    d->vertex_count += count;
    return base;
}

void tide_draw_triangles_with(tide_draw_list *d, const uint32_t base, const uint32_t *indices, const uint32_t count,
                              const uint32_t slot, const tide_filter filter)
{
    if (count < 3u || base > d->vertex_count || slot > d->texture_count) return;
    const uint32_t within = d->vertex_count - base;
    d->indices = grow(d->indices, d->index_count, &d->index_capacity, count, sizeof(uint32_t), 2048u);
    const uint32_t first = d->index_count;
    for (uint32_t i = 0; i + 3u <= count; i += 3u) {
        const uint32_t a = indices[i], b = indices[i + 1u], c = indices[i + 2u];
        if (a >= within || b >= within || c >= within) continue;
        uint32_t *out = &d->indices[d->index_count];
        out[0] = base + a;
        out[1] = base + b;
        out[2] = base + c;
        d->index_count += 3u;
    }
    if (d->index_count == first) return;
    // More of the mesh before it, when nothing came between them
    tide_draw_command *last = d->count ? &d->commands[d->count - 1u] : NULL;
    if (last && last->kind == TIDE_DRAW_MESH && last->mesh.texture == slot && last->mesh.filter == (uint32_t)filter
        && last->mesh.first + last->mesh.count == first) {
        last->mesh.count += d->index_count - first;
        return;
    }
    tide_draw_command *c = push(d, TIDE_DRAW_MESH, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
    c->mesh.first = first;
    c->mesh.count = d->index_count - first;
    c->mesh.texture = slot;
    c->mesh.filter = (uint32_t)filter;
}

void tide_draw_triangles(tide_draw_list *d, const uint32_t base, const uint32_t *indices, const uint32_t count,
                         const tide_texture *texture, const tide_filter filter)
{
    tide_draw_triangles_with(d, base, indices, count, texture_slot(d, texture), filter);
}

void tide_draw_mesh(tide_draw_list *d, const tide_vertex *vertices, const uint32_t vertex_count, const uint32_t *indices,
                    const uint32_t index_count, const tide_texture *texture, const tide_filter filter)
{
    if (vertex_count == 0 || index_count < 3u) return;
    tide_draw_triangles(d, tide_draw_vertices(d, vertices, vertex_count), indices, index_count, texture, filter);
}

// ---------------------------------------------------------------------------
// 3D

static uint32_t push_matrix(tide_draw_list *d, const tide_float4x4 m)
{
    d->matrices = grow(d->matrices, d->matrix_count, &d->matrix_capacity, 1, sizeof(tide_float4x4), 256u);
    d->matrices[d->matrix_count] = m;
    return d->matrix_count++;
}

// OpenGL's view looks down -z, and the world's cameras along +z: z turned
// around, after `m`.
static tide_float4x4 turn_z(tide_float4x4 m)
{
    m.c0.z = -m.c0.z;
    m.c1.z = -m.c1.z;
    m.c2.z = -m.c2.z;
    m.c3.z = -m.c3.z;
    return m;
}

static void camera_3d(tide_draw_list *d, const tide_float4x4 world_to_clip, const bool fit)
{
    const uint32_t matrix = push_matrix(d, world_to_clip);
    tide_draw_command *c = push(d, TIDE_DRAW_CAMERA_3D, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
    c->camera.matrix = matrix;
    c->camera.fit = fit;
}

tide_float4x4 tide_draw_camera_3d_matrix(const tide_float3 position, const tide_quaternion rotation,
                                         const float field_of_view)
{
    // The world as the camera sees it: moved and turned the other way
    const tide_quaternion back = tide_conjugate_q(tide_normalizesafe_q(rotation));
    const tide_float4x4 view = tide_f4x4_from_f3x3_f3(tide_f3x3_from_q(back), tide_neg_f3(tide_rotate_q(back, position)));
    // A perspective for a square screen, from 0.3 units on, as Unity's
    // cameras, with no far side: precision hardly depends on it
    const float near_z = 0.3f;
    const float f = 1.0f / tide_tan_f(tide_radians_f(tide_clamp_f(field_of_view, 0.00001f, 179.0f)) * 0.5f);
    const tide_float4x4 projection = {{f, 0.0f, 0.0f, 0.0f},
                                      {0.0f, f, 0.0f, 0.0f},
                                      {0.0f, 0.0f, -1.0f, -1.0f},
                                      {0.0f, 0.0f, -2.0f * near_z, 0.0f}};
    return tide_mul_f4x4(projection, turn_z(view));
}

void tide_draw_camera_3d(tide_draw_list *d, const tide_float3 position, const tide_quaternion rotation,
                         const float field_of_view)
{
    camera_3d(d, tide_draw_camera_3d_matrix(position, rotation, field_of_view), true);
}

void tide_draw_camera_matrices(tide_draw_list *d, const tide_float4x4 transform, const tide_float4x4 projection)
{
    camera_3d(d, tide_mul_f4x4(projection, turn_z(tide_inverse_f4x4(transform))), false);
}

// The meshes are found by whose they are and their version, in a table of
// places that's at most half full.
static uint32_t mesh_hash(const uint32_t space, const uint64_t id, const uint64_t other, const uint64_t version)
{
    uint64_t h = id * 0x9E3779B97F4A7C15ull ^ other * 0xC2B2AE3D27D4EB4Full ^ version * 0x165667B19E3779F9ull ^ space;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return (uint32_t)h;
}

static void index_mesh(tide_draw_list *d, const uint32_t place)
{
    const tide_draw_mesh_data *m = &d->meshes[place];
    const uint32_t mask = d->mesh_index_capacity - 1u;
    uint32_t i = mesh_hash(m->space, m->id, m->other, m->version) & mask;
    while (d->mesh_index[i]) i = (i + 1u) & mask;
    d->mesh_index[i] = place + 1u;
}

static void reindex_meshes(tide_draw_list *d)
{
    uint32_t capacity = d->mesh_index_capacity ? d->mesh_index_capacity : 64u;
    while (capacity / 2u <= d->mesh_count) {
        if (capacity > UINT32_MAX / 2u / sizeof(uint32_t)) tide_out_of_memory();
        capacity *= 2u;
    }
    if (capacity != d->mesh_index_capacity) {
        free(d->mesh_index);
        d->mesh_index = tide_alloc_zeroed(capacity, sizeof(uint32_t));
        d->mesh_index_capacity = capacity;
    } else {
        memset(d->mesh_index, 0, (size_t)capacity * sizeof(uint32_t));
    }
    for (uint32_t i = 0; i < d->mesh_count; i++) {
        if (d->meshes[i].space != TIDE_MESH_NONE) index_mesh(d, i);
    }
}

uint32_t tide_draw_mesh_slot(tide_draw_list *d, const uint32_t space, const uint64_t id, const uint64_t other,
                             const uint64_t version, bool *fill)
{
    *fill = false;
    if (space == TIDE_MESH_NONE) return 0;
    if (d->mesh_index_capacity) {
        const uint32_t mask = d->mesh_index_capacity - 1u;
        for (uint32_t i = mesh_hash(space, id, other, version) & mask; d->mesh_index[i]; i = (i + 1u) & mask) {
            tide_draw_mesh_data *m = &d->meshes[d->mesh_index[i] - 1u];
            if (m->space == space && m->id == id && m->other == other && m->version == version) {
                m->frame = d->frame;
                return d->mesh_index[i];
            }
        }
    }
    uint32_t place;
    if (d->mesh_free) {
        place = d->mesh_free - 1u;
        d->mesh_free = d->meshes[place].next_free;
    } else {
        d->meshes = grow(d->meshes, d->mesh_count, &d->mesh_capacity, 1, sizeof(tide_draw_mesh_data), 16u);
        place = d->mesh_count++;
    }
    d->meshes[place] = (tide_draw_mesh_data){
        .id = id, .other = other, .version = version, .space = space, .frame = d->frame, .epoch = d->epoch};
    if (d->mesh_count >= d->mesh_index_capacity / 2u) reindex_meshes(d);
    else index_mesh(d, place);
    *fill = true;
    return place + 1u;
}

void tide_draw_mesh_fill(tide_draw_list *d, const uint32_t mesh, const tide_vertex3 *vertices,
                         const uint32_t vertex_count, const uint32_t *indices, const uint32_t index_count)
{
    if (mesh == 0 || mesh > d->mesh_count || d->meshes[mesh - 1u].space == TIDE_MESH_NONE) return;
    tide_draw_mesh_data *m = &d->meshes[mesh - 1u];
    m->vertex_count = m->index_count = 0;
    if (vertex_count == 0 || index_count < 3u) return;
    m->vertices = grow(m->vertices, 0, &m->vertex_capacity, vertex_count, sizeof(tide_vertex3), 64u);
    memcpy(m->vertices, vertices, (size_t)vertex_count * sizeof(tide_vertex3));
    m->vertex_count = vertex_count;
    // A triangle with an index past the vertices is left out
    m->indices = grow(m->indices, 0, &m->index_capacity, index_count, sizeof(uint32_t), 192u);
    for (uint32_t i = 0; i + 3u <= index_count; i += 3u) {
        const uint32_t a = indices[i], b = indices[i + 1u], c = indices[i + 2u];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        uint32_t *out = &m->indices[m->index_count];
        out[0] = a;
        out[1] = b;
        out[2] = c;
        m->index_count += 3u;
    }
}

void tide_draw_instance(tide_draw_list *d, const uint32_t mesh, const tide_float4x4 transform, const uint32_t texture,
                        const tide_filter filter)
{
    if (mesh == 0 || mesh > d->mesh_count || d->meshes[mesh - 1u].index_count == 0 || texture > d->texture_count) return;
    const uint32_t at = push_matrix(d, transform);
    // Another instance of the mesh before it, when nothing came between them
    tide_draw_command *last = d->count ? &d->commands[d->count - 1u] : NULL;
    if (last && last->kind == TIDE_DRAW_MESH_3D && last->mesh3.mesh == mesh && last->mesh3.texture == texture
        && last->mesh3.filter == (uint32_t)filter && last->mesh3.first + last->mesh3.count == at) {
        last->mesh3.count++;
        return;
    }
    tide_draw_command *c = push(d, TIDE_DRAW_MESH_3D, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
    c->mesh3.mesh = mesh;
    c->mesh3.first = at;
    c->mesh3.count = 1;
    c->mesh3.texture = texture;
    c->mesh3.filter = (uint32_t)filter;
}

uint64_t tide_mesh_version(const tide_mesh *mesh)
{
    if (!mesh || !mesh->vertices || !mesh->indices) return 0;
    uint64_t h = tide_hash_more(TIDE_HASH_START, &mesh->vertex_count, sizeof mesh->vertex_count);
    h = tide_hash_more(h, &mesh->index_count, sizeof mesh->index_count);
    h = tide_hash_more(h, mesh->vertices, (size_t)mesh->vertex_count * sizeof(tide_vertex3));
    return tide_hash_end(tide_hash_more(h, mesh->indices, (size_t)mesh->index_count * sizeof(uint32_t)));
}

void tide_draw_mesh_3d(tide_draw_list *d, const tide_mesh *mesh, const tide_float4x4 transform,
                       const tide_texture *texture, const tide_filter filter)
{
    if (!mesh || !mesh->vertices || !mesh->indices) return;
    bool fill;
    const uint32_t slot = tide_draw_mesh_slot(d, TIDE_MESH_MEMORY, (uint64_t)(uintptr_t)mesh->vertices,
                                              (uint64_t)(uintptr_t)mesh->indices, mesh->version, &fill);
    if (fill) tide_draw_mesh_fill(d, slot, mesh->vertices, mesh->vertex_count, mesh->indices, mesh->index_count);
    tide_draw_instance(d, slot, transform, texture_slot(d, texture), filter);
}

// The texture of `from`'s mesh command, as `d` has it.
static uint32_t append_texture(tide_draw_list *d, const tide_draw_list *from, const uint32_t texture)
{
    if (!texture) return 0;
    const tide_draw_texture *t = &from->textures[texture - 1u];
    uint8_t *pixels;
    const uint32_t slot = tide_draw_texture_slot(d, t->space, t->id, t->version, t->width, t->height, &pixels);
    if (pixels) memcpy(pixels, t->pixels, (size_t)t->width * (size_t)t->height * 4u);
    return slot;
}

void tide_draw_append(tide_draw_list *d, const tide_draw_list *from)
{
    reserve(d, from->count);
    const uint32_t base = tide_draw_vertices(d, from->vertices, from->vertex_count);
    for (uint32_t i = 0; i < from->count; i++) {
        const tide_draw_command *c = &from->commands[i];
        if (c->kind == TIDE_DRAW_TEXT) {
            tide_draw_text(d, from->text + c->text, c->a, c->b.x, c->color);
        } else if (c->kind == TIDE_DRAW_MESH) {
            // Its indices are places in `from`'s vertices, which are `d`'s from `base`
            tide_draw_triangles_with(d, base, from->indices + c->mesh.first, c->mesh.count,
                                     append_texture(d, from, c->mesh.texture), (tide_filter)c->mesh.filter);
        } else if (c->kind == TIDE_DRAW_CAMERA_3D) {
            camera_3d(d, from->matrices[c->camera.matrix], c->camera.fit != 0);
        } else if (c->kind == TIDE_DRAW_MESH_3D) {
            if (!c->mesh3.mesh) continue;
            const tide_draw_mesh_data *m = &from->meshes[c->mesh3.mesh - 1u];
            bool fill;
            const uint32_t slot = tide_draw_mesh_slot(d, m->space, m->id, m->other, m->version, &fill);
            if (fill) tide_draw_mesh_fill(d, slot, m->vertices, m->vertex_count, m->indices, m->index_count);
            const uint32_t texture = append_texture(d, from, c->mesh3.texture);
            for (uint32_t k = 0; k < c->mesh3.count; k++) {
                tide_draw_instance(d, slot, from->matrices[c->mesh3.first + k], texture, (tide_filter)c->mesh3.filter);
            }
        } else {
            push(d, (tide_draw_kind)c->kind, c->a, c->b, c->color);
            if (c->kind == TIDE_DRAW_CLIP || c->kind == TIDE_DRAW_NO_CLIP) d->clipped = c->kind == TIDE_DRAW_CLIP;
        }
    }
}
