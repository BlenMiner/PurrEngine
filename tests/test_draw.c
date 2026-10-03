#include <string.h>

#include "tide/draw.h"
#include "tide/mesh.h"
#include "tide/text.h"
#include "tide_test.h"

static tide_draw_list list;

TIDE_TEST(draw_records_in_order)
{
    tide_draw_reset(&list);
    tide_draw_clear(&list, TIDE_COLOR_BLACK);
    tide_draw_circle(&list, tide_f2(1.0f, 2.0f), 3.0f, TIDE_COLOR_RED);
    tide_draw_text(&list, "one", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_text(&list, "two", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    TIDE_REQUIRE(list.count == 4);
    TIDE_CHECK(list.commands[0].kind == TIDE_DRAW_CLEAR);
    TIDE_CHECK(list.commands[1].kind == TIDE_DRAW_CIRCLE && list.commands[1].b.x == 3.0f);
    TIDE_CHECK(strcmp(list.text + list.commands[2].text, "one") == 0);
    TIDE_CHECK(strcmp(list.text + list.commands[3].text, "two") == 0);

    tide_draw_reset(&list);
    TIDE_CHECK(list.count == 0 && list.text_used == 0);
}

// It grows as it needs: nothing is dropped, however much a frame draws.
TIDE_TEST(draw_list_grows)
{
    tide_draw_list grown = {0};
    for (uint32_t i = 0; i < 100000u; i++) {
        tide_draw_rect(&grown, tide_f2((float)i, 0.0f), tide_f2(1.0f, 1.0f), TIDE_COLOR_WHITE);
    }
    static char big[100000];
    memset(big, 'a', sizeof big - 1);
    tide_draw_text(&grown, big, tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_text(&grown, "end", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    TIDE_REQUIRE(grown.count == 100002u);
    TIDE_CHECK(grown.commands[99999].a.x == 99999.0f);
    TIDE_CHECK(strlen(grown.text + grown.commands[100000].text) == sizeof big - 1);
    TIDE_CHECK(strcmp(grown.text + grown.commands[100001].text, "end") == 0);

    // Appended, as the GUI's are over the world's, text and all
    tide_draw_list world = {0};
    tide_draw_text(&world, "world", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_append(&world, &grown);
    TIDE_REQUIRE(world.count == 100003u);
    TIDE_CHECK(strcmp(world.text + world.commands[0].text, "world") == 0);
    TIDE_CHECK(strcmp(world.text + world.commands[100002].text, "end") == 0);
    tide_draw_free(&world);
    tide_draw_free(&grown);
    TIDE_CHECK(grown.count == 0 && grown.commands == NULL);
}

// ---------------------------------------------------------------------------
// Meshes

static const tide_vertex quad[4] = {
    {{0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{8.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{8.0f, 8.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    {{0.0f, 8.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};
static const uint32_t quad_indices[6] = {0, 1, 2, 0, 2, 3};

TIDE_TEST(draw_meshes_keep_their_triangles)
{
    tide_draw_list d = {0};
    tide_draw_rect(&d, tide_f2(0.0f, 0.0f), tide_f2(1.0f, 1.0f), TIDE_COLOR_WHITE);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, NULL, TIDE_FILTER_BILINEAR);
    TIDE_REQUIRE(d.count == 2 && d.vertex_count == 4 && d.index_count == 6);
    TIDE_CHECK(d.commands[1].kind == TIDE_DRAW_MESH);
    TIDE_CHECK(d.commands[1].mesh.first == 0 && d.commands[1].mesh.count == 6 && d.commands[1].mesh.texture == 0);
    TIDE_CHECK(d.vertices[2].uv.x == 1.0f && d.vertices[2].color.b == 1.0f);

    // Another right after it is more of the same command, its indices places
    // in the list's vertices
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, NULL, TIDE_FILTER_BILINEAR);
    TIDE_REQUIRE(d.count == 2 && d.vertex_count == 8 && d.index_count == 12);
    TIDE_CHECK(d.commands[1].mesh.count == 12);
    TIDE_CHECK(d.indices[6] == 4 && d.indices[8] == 6 && d.indices[11] == 7);

    // Something between them, or another filter, starts another
    tide_draw_clip(&d, (tide_rect){1.0f, 2.0f, 3.0f, 4.0f});
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, NULL, TIDE_FILTER_BILINEAR);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, NULL, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.count == 5);
    TIDE_CHECK(d.commands[2].kind == TIDE_DRAW_CLIP && d.commands[2].a.y == 2.0f && d.commands[2].b.x == 3.0f);
    TIDE_CHECK(d.commands[3].mesh.first == 12 && d.commands[3].mesh.count == 6);
    TIDE_CHECK(d.commands[4].mesh.first == 18 && d.commands[4].mesh.filter == TIDE_FILTER_POINT);

    // A triangle with an index past the vertices is left out, and so is what's
    // left over after the last whole one
    static const uint32_t off[8] = {0, 1, 2, 0, 4, 1, 3, 2};
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, off, 8, NULL, TIDE_FILTER_BILINEAR);
    TIDE_REQUIRE(d.count == 1 && d.index_count == 3);
    TIDE_CHECK(d.indices[0] == 0 && d.indices[2] == 2);
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, off + 3, 3, NULL, TIDE_FILTER_BILINEAR); // Nothing of it is left
    TIDE_CHECK(d.count == 0 && d.index_count == 0);

    // Vertices several meshes share go in once
    tide_draw_reset(&d);
    tide_draw_rect(&d, tide_f2(0.0f, 0.0f), tide_f2(1.0f, 1.0f), TIDE_COLOR_WHITE);
    tide_draw_vertices(&d, quad, 4);
    const uint32_t base = tide_draw_vertices(&d, quad, 4);
    TIDE_CHECK(base == 4);
    tide_draw_triangles(&d, base, quad_indices, 3, NULL, TIDE_FILTER_BILINEAR);
    tide_draw_no_clip(&d);
    tide_draw_triangles(&d, base, quad_indices + 3, 3, NULL, TIDE_FILTER_BILINEAR);
    TIDE_REQUIRE(d.count == 4 && d.vertex_count == 8 && d.index_count == 6);
    TIDE_CHECK(d.indices[0] == 4 && d.indices[5] == 7);
    TIDE_CHECK(d.commands[3].mesh.first == 3 && d.commands[3].mesh.count == 3);
    tide_draw_free(&d);
    TIDE_CHECK(d.vertices == NULL && d.indices == NULL && d.vertex_count == 0);
}

TIDE_TEST(draw_textures_are_copied_when_they_change)
{
    tide_draw_list d = {0};
    uint8_t pixels[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    tide_texture texture = {pixels, 2, 2, 1};
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.count == 1 && d.texture_count == 1);
    TIDE_CHECK(d.commands[0].mesh.texture == 1);
    const tide_draw_texture *t = &d.textures[0];
    TIDE_CHECK(t->width == 2 && t->height == 2 && t->version == 1 && t->space == TIDE_PIXELS_MEMORY);
    TIDE_CHECK(t->pixels != pixels && memcmp(t->pixels, pixels, sizeof pixels) == 0);

    // The same pixels at the same version aren't copied again, in this frame
    // or the next
    pixels[0] = 7;
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_CHECK(d.texture_count == 1 && d.textures[0].pixels[0] == 255);
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_CHECK(d.texture_count == 1 && d.textures[0].pixels[0] == 255);

    // Another version is, and so is another size
    texture.version = tide_texture_version(pixels, 2, 2);
    TIDE_CHECK(texture.version != 1 && texture.version == tide_texture_version(pixels, 2, 2));
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_CHECK(d.texture_count == 1 && d.textures[0].pixels[0] == 7);
    pixels[0] = 9;
    texture.height = 1;
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_CHECK(d.texture_count == 1 && d.textures[0].height == 1 && d.textures[0].pixels[0] == 9);

    // Other pixels are another texture, and no pixels are none
    static const uint8_t white[4] = {255, 255, 255, 255};
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &(tide_texture){white, 1, 1, 0}, TIDE_FILTER_BILINEAR);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &(tide_texture){NULL, 1, 1, 0}, TIDE_FILTER_BILINEAR);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &(tide_texture){white, 0, 1, 0}, TIDE_FILTER_BILINEAR);
    TIDE_REQUIRE(d.texture_count == 2);
    TIDE_CHECK(d.commands[d.count - 2].mesh.texture == 2);
    TIDE_CHECK(d.commands[d.count - 1].mesh.texture == 0);

    // Forgotten, as when the game's code was swapped, it's copied again though
    // its version is the same
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.texture_count == 2 && d.textures[0].pixels[0] == 9 && d.textures[0].epoch == 0);
    pixels[0] = 11;
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_CHECK(d.textures[0].pixels[0] == 9);
    tide_draw_forget(&d);
    TIDE_CHECK(d.texture_count == 0 && d.commands[0].mesh.texture == 0);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.texture_count == 1);
    TIDE_CHECK(d.textures[0].pixels[0] == 11 && d.textures[0].epoch == 1);
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &texture, TIDE_FILTER_POINT);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &(tide_texture){white, 1, 1, 0}, TIDE_FILTER_BILINEAR);

    // A texture no command of a frame drew with is let go when the next starts
    tide_draw_reset(&d);
    tide_draw_mesh(&d, quad, 4, quad_indices, 6, &(tide_texture){white, 1, 1, 0}, TIDE_FILTER_BILINEAR);
    TIDE_CHECK(d.texture_count == 2);
    tide_draw_reset(&d);
    TIDE_REQUIRE(d.texture_count == 1);
    TIDE_CHECK(d.textures[0].id == (uint64_t)(uintptr_t)white);
    tide_draw_reset(&d);
    TIDE_CHECK(d.texture_count == 0);
    tide_draw_free(&d);
}

// Appended to another list, as the GUI's are, meshes keep their triangles and
// their textures.
TIDE_TEST(draw_meshes_append)
{
    static const uint8_t pixel[4] = {1, 2, 3, 4};
    tide_draw_list world = {0}, over = {0};
    tide_draw_mesh(&world, quad, 4, quad_indices, 6, NULL, TIDE_FILTER_BILINEAR);
    tide_draw_clip(&world, (tide_rect){0.0f, 0.0f, 4.0f, 4.0f});
    TIDE_CHECK(world.clipped);
    tide_draw_vertices(&over, quad, 2); // Before its mesh's, so its indices don't start at 0
    tide_draw_mesh(&over, quad, 4, quad_indices, 6, &(tide_texture){pixel, 1, 1, 5}, TIDE_FILTER_POINT);
    tide_draw_no_clip(&over);
    tide_draw_append(&world, &over);
    TIDE_REQUIRE(world.count == 4 && world.vertex_count == 10 && world.index_count == 12 && world.texture_count == 1);
    TIDE_CHECK(!world.clipped);
    const tide_draw_command *c = &world.commands[2];
    TIDE_CHECK(c->kind == TIDE_DRAW_MESH && c->mesh.first == 6 && c->mesh.count == 6 && c->mesh.texture == 1);
    TIDE_CHECK(c->mesh.filter == TIDE_FILTER_POINT);
    TIDE_CHECK(world.indices[6] == 6 && world.indices[11] == 9);
    TIDE_CHECK(world.textures[0].version == 5 && memcmp(world.textures[0].pixels, pixel, 4) == 0);
    tide_draw_free(&world);
    tide_draw_free(&over);
}

// Tide's Draw.Mesh: lists, and a grid of colors as the texture, which is only
// copied when its cells changed.
TIDE_TEST(draw_grids_are_textures)
{
    static const tide_grid_shape colors = {sizeof(tide_color), 2u, {5u, 5u, 0u}}; // As tidec makes Grid2<Color>'s
    tide_heap heap = {0};
    tide_text_use(NULL, &heap);
    const uint32_t mark = tide_scratch_mark();
    tide_grid grid = {0};
    tide_grid_set(&grid, tide_grid_new(2, 40, 33, 0), &colors, TIDE_IN_LOCAL);
    const tide_color red = {1.0f, 0.0f, 0.0f, 1.0f}, half = {0.5f, 2.0f, -1.0f, 0.25f};
    *(tide_color *)tide_grid_poke(&grid, 0, 0, 0, &colors, true, TIDE_IN_LOCAL) = red;
    *(tide_color *)tide_grid_poke(&grid, 39, 32, 0, &colors, true, TIDE_IN_LOCAL) = half;

    static const int32_t indices[6] = {0, 1, 2, 0, 2, 3};
    const tide_list vertices = tide_list_from(quad, 4, sizeof(tide_vertex));
    const tide_list triangles = tide_list_from(indices, 6, sizeof(int32_t));
    tide_draw_list d = {0};
    tide_draw_mesh_lists(&d, vertices, triangles);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.count == 2 && d.vertex_count == 8 && d.index_count == 12 && d.texture_count == 1);
    TIDE_CHECK(d.commands[0].mesh.texture == 0 && d.commands[0].mesh.filter == TIDE_FILTER_BILINEAR);
    TIDE_CHECK(d.commands[1].mesh.texture == 1 && d.commands[1].mesh.filter == TIDE_FILTER_POINT);
    TIDE_CHECK(d.indices[6] == 4 && d.indices[11] == 7);
    const tide_draw_texture *t = &d.textures[0];
    TIDE_REQUIRE(t->width == 40 && t->height == 33 && t->space == TIDE_PIXELS_GRID);
    static const uint8_t first[4] = {255, 0, 0, 255}, last[4] = {128, 255, 0, 64}, none[4] = {0, 0, 0, 0};
    TIDE_CHECK(memcmp(t->pixels, first, 4) == 0);
    TIDE_CHECK(memcmp(t->pixels + (32 * 40 + 39) * 4, last, 4) == 0);
    TIDE_CHECK(memcmp(t->pixels + 4, none, 4) == 0);          // A cell nothing set
    TIDE_CHECK(memcmp(t->pixels + (10 * 40 + 35) * 4, none, 4) == 0); // ...and one where the grid has no chunk

    // Cells that stayed the same aren't copied again
    d.textures[0].pixels[4] = 77;
    tide_draw_reset(&d);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_CHECK(d.texture_count == 1 && d.textures[0].pixels[4] == 77);

    // One that changed is, even changed through a cache that had its chunk
    // from before the list looked
    tide_grid_cache cache = {0};
    *(tide_color *)tide_grid_cached_poke(&cache, &grid, 1, 0, 0, &colors, true, TIDE_IN_LOCAL) = red;
    tide_draw_reset(&d);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_CHECK(memcmp(d.textures[0].pixels + 4, first, 4) == 0);
    *(tide_color *)tide_grid_cached_poke(&cache, &grid, 2, 0, 0, &colors, true, TIDE_IN_LOCAL) = red;
    tide_draw_reset(&d);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_CHECK(memcmp(d.textures[0].pixels + 8, first, 4) == 0);

    // A cleared grid is clear pixels, and one with an open axis is no texture
    tide_grid_clear(&grid, &colors, TIDE_IN_LOCAL);
    tide_draw_reset(&d);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_REQUIRE(d.texture_count == 1);
    TIDE_CHECK(memcmp(d.textures[0].pixels, none, 4) == 0);
    tide_grid_set(&grid, tide_grid_new(2, 40, 0, 0), &colors, TIDE_IN_LOCAL);
    *(tide_color *)tide_grid_poke(&grid, 0, 0, 0, &colors, true, TIDE_IN_LOCAL) = red;
    tide_draw_reset(&d);
    tide_draw_mesh_grid(&d, vertices, triangles, grid, &colors, TIDE_FILTER_POINT);
    TIDE_CHECK(d.count == 1 && d.commands[0].mesh.texture == 0);
    tide_draw_mesh_grid(&d, vertices, triangles, (tide_grid){0}, &colors, TIDE_FILTER_POINT);
    TIDE_CHECK(d.commands[0].mesh.count == 12 && d.commands[0].mesh.texture == 0);

    tide_draw_free(&d);
    tide_grid_release(&grid, &colors, TIDE_IN_LOCAL);
    tide_scratch_reset(mark);
    tide_text_use(NULL, NULL);
    tide_heap_free(&heap);
}
