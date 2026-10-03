#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static void frame(const tide_world *w)
{
    tide_draw_reset(&draw);
    tide_frame(w, NULL, 1.0f, &local, &draw, &gui);
}

static const tide_draw_command *command(const uint32_t i)
{
    return &draw.commands[i];
}

// The list's texture of that size.
static tide_draw_texture *texture(const int32_t width, const int32_t height)
{
    for (uint32_t i = 0; i < draw.texture_count; i++) {
        if (draw.textures[i].width == width && draw.textures[i].height == height) return &draw.textures[i];
    }
    return NULL;
}

static bool pixel_is(const tide_draw_texture *t, const int x, const int y, const uint8_t r, const uint8_t g,
                     const uint8_t b, const uint8_t a)
{
    const uint8_t *p = t->pixels + ((size_t)y * (size_t)t->width + (size_t)x) * 4u;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static bool is_mesh(const uint32_t i, const uint32_t count, const tide_draw_texture *t, const tide_filter filter)
{
    const tide_draw_command *c = command(i);
    const uint32_t slot = t ? (uint32_t)(t - draw.textures) + 1u : 0u;
    return c->kind == TIDE_DRAW_MESH && c->mesh.count == count && c->mesh.texture == slot && c->mesh.filter == (uint32_t)filter;
}

TIDE_TEST(meshes_draw_in_order)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    tide_local_init(&local);
    frame(&world);
    TIDE_REQUIRE(draw.count == 17 && draw.texture_count == 3);
    const tide_draw_texture *theirs = texture(2, 1), *checker = texture(4, 2), *canvas = texture(3, 3);
    TIDE_REQUIRE(theirs && checker && canvas);
    TIDE_CHECK(theirs->space == TIDE_PIXELS_MEMORY && checker->space == TIDE_PIXELS_GRID);

    // A function that draws, and the C it hands the list
    TIDE_CHECK(command(0)->kind == TIDE_DRAW_CLEAR);
    TIDE_CHECK(is_mesh(1, 6, NULL, TIDE_FILTER_BILINEAR));
    TIDE_CHECK(command(2)->kind == TIDE_DRAW_CLIP && command(2)->b.x == 1.0f);
    TIDE_CHECK(is_mesh(3, 3, theirs, TIDE_FILTER_POINT));
    TIDE_CHECK(command(4)->kind == TIDE_DRAW_NO_CLIP);
    const tide_vertex *corners = draw.vertices + draw.indices[command(1)->mesh.first];
    TIDE_CHECK(corners[0].position.x == 10.0f && corners[0].position.y == 20.0f);
    TIDE_CHECK(corners[0].color.r == 1.0f && corners[0].color.g == 1.0f && corners[0].color.a == 1.0f); // White, left out
    TIDE_CHECK(corners[1].position.x == 12.0f && corners[1].uv.x == 1.0f && corners[1].uv.y == 0.0f);
    TIDE_CHECK(corners[1].color.r == 0.0f && corners[1].color.g == 1.0f);
    TIDE_CHECK(draw.indices[command(1)->mesh.first + 5] == draw.indices[command(1)->mesh.first] + 3);

    // The view's own: a clip, a grid as the texture with each filter, and one
    // with no size, which is no texture
    TIDE_CHECK(command(5)->kind == TIDE_DRAW_CLIP);
    TIDE_CHECK(command(5)->a.x == 1.0f && command(5)->a.y == 2.0f && command(5)->b.x == 3.0f && command(5)->b.y == 4.0f);
    TIDE_CHECK(is_mesh(6, 3, checker, TIDE_FILTER_BILINEAR));
    TIDE_CHECK(is_mesh(7, 3, checker, TIDE_FILTER_POINT)); // Its triangle with an index past the list is left out
    TIDE_CHECK(is_mesh(8, 3, NULL, TIDE_FILTER_BILINEAR));
    TIDE_CHECK(command(9)->kind == TIDE_DRAW_NO_CLIP);
    const tide_vertex *kept = draw.vertices + draw.indices[command(6)->mesh.first];
    TIDE_CHECK(kept[1].position.x == 3.0f && kept[1].uv.x == 1.0f && kept[1].color.b == 0.0f && kept[1].color.g == 1.0f);
    TIDE_CHECK(draw.vertices[draw.indices[command(7)->mesh.first]].position.x == 5.0f); // 2, 1, 0
    TIDE_CHECK(pixel_is(checker, 0, 0, 255, 0, 0, 255));
    TIDE_CHECK(pixel_is(checker, 3, 1, 0, 0, 255, 128));
    TIDE_CHECK(pixel_is(checker, 1, 0, 0, 0, 0, 0));

    // On the screen, C again, which says how many commands the list has
    TIDE_CHECK(command(10)->kind == TIDE_DRAW_SCREEN);
    TIDE_CHECK(command(11)->kind == TIDE_DRAW_CLIP && is_mesh(12, 3, theirs, TIDE_FILTER_POINT));
    TIDE_CHECK(command(13)->kind == TIDE_DRAW_NO_CLIP);
    TIDE_CHECK(command(14)->kind == TIDE_DRAW_TEXT && strcmp(draw.text + command(14)->text, "3") == 0);
    TIDE_CHECK(draw.vertices[draw.indices[command(12)->mesh.first]].color.g == 1.0f); // Cyan

    // The match's grid, as the tick left it
    TIDE_CHECK(command(15)->kind == TIDE_DRAW_CAMERA);
    TIDE_CHECK(is_mesh(16, 3, canvas, TIDE_FILTER_BILINEAR));
    TIDE_CHECK(pixel_is(canvas, 0, 0, 0, 255, 0, 255) && pixel_is(canvas, 1, 0, 0, 0, 0, 0));
}

TIDE_TEST(meshes_grids_are_copied_when_they_change)
{
    // A frame where nothing changed copies nothing
    texture(4, 2)->pixels[4] = 77;
    texture(3, 3)->pixels[4] = 77;
    frame(&world);
    TIDE_REQUIRE(draw.count == 19 && draw.texture_count == 3); // Another blade: a clip and a mesh more
    TIDE_CHECK(strcmp(draw.text + command(16)->text, "5") == 0);
    TIDE_CHECK(texture(4, 2)->pixels[4] == 77 && texture(3, 3)->pixels[4] == 77);

    // The view changed a cell, and a tick another
    tide_world_tick(&world);
    frame(&world);
    TIDE_REQUIRE(draw.texture_count == 3);
    TIDE_CHECK(pixel_is(texture(4, 2), 1, 0, 255, 255, 255, 255) && pixel_is(texture(4, 2), 0, 0, 255, 0, 0, 255));
    TIDE_CHECK(pixel_is(texture(3, 3), 1, 0, 0, 255, 0, 255) && pixel_is(texture(3, 3), 0, 0, 0, 255, 0, 255));

    // Outside a match, its views don't run, and their texture is let go
    frame(NULL);
    TIDE_CHECK(command(draw.count - 1)->kind == TIDE_DRAW_TEXT);
    frame(NULL);
    TIDE_CHECK(draw.texture_count == 2 && !texture(3, 3));
}
