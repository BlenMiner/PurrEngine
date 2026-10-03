#include "tide/draw.h"

#include <stdlib.h>
#include <string.h>

#include "tide/net.h"
#include "tide/page.h"

void tide_draw_reset(tide_draw_list *d)
{
    d->count = 0;
    d->text_used = 0;
    d->vertex_count = 0;
    d->index_count = 0;
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
    d->frame++;
}

void tide_draw_free(tide_draw_list *d)
{
    for (uint32_t i = 0; i < d->texture_count; i++) free(d->textures[i].pixels);
    free(d->commands);
    free(d->text);
    free(d->vertices);
    free(d->indices);
    free(d->textures);
    *d = (tide_draw_list){0};
}

void tide_draw_forget(tide_draw_list *d)
{
    for (uint32_t i = 0; i < d->texture_count; i++) free(d->textures[i].pixels);
    d->texture_count = 0;
    d->epoch++;
    // Commands recorded already would draw with textures that are gone
    for (uint32_t i = 0; i < d->count; i++) {
        if (d->commands[i].kind == TIDE_DRAW_MESH) d->commands[i].mesh.texture = 0;
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

void tide_draw_append(tide_draw_list *d, const tide_draw_list *from)
{
    reserve(d, from->count);
    const uint32_t base = tide_draw_vertices(d, from->vertices, from->vertex_count);
    for (uint32_t i = 0; i < from->count; i++) {
        const tide_draw_command *c = &from->commands[i];
        if (c->kind == TIDE_DRAW_TEXT) {
            tide_draw_text(d, from->text + c->text, c->a, c->b.x, c->color);
        } else if (c->kind == TIDE_DRAW_MESH) {
            uint32_t slot = 0;
            if (c->mesh.texture) {
                const tide_draw_texture *t = &from->textures[c->mesh.texture - 1u];
                uint8_t *pixels;
                slot = tide_draw_texture_slot(d, t->space, t->id, t->version, t->width, t->height, &pixels);
                if (pixels) memcpy(pixels, t->pixels, (size_t)t->width * (size_t)t->height * 4u);
            }
            // Its indices are places in `from`'s vertices, which are `d`'s from `base`
            tide_draw_triangles_with(d, base, from->indices + c->mesh.first, c->mesh.count, slot,
                                     (tide_filter)c->mesh.filter);
        } else {
            push(d, (tide_draw_kind)c->kind, c->a, c->b, c->color);
            if (c->kind == TIDE_DRAW_CLIP || c->kind == TIDE_DRAW_NO_CLIP) d->clipped = c->kind == TIDE_DRAW_CLIP;
        }
    }
}
