#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

// A world's bytes (tide_world_pack), to see later whether it changed.
static uint8_t *world_bytes(const tide_world *w, uint32_t *size)
{
    *size = tide_world_pack(w, NULL, 0);
    uint8_t *bytes = malloc(*size);
    tide_world_pack(w, bytes, *size);
    return bytes;
}

static bool world_is(const tide_world *w, uint8_t *bytes, const uint32_t size)
{
    uint32_t now_size;
    uint8_t *now = world_bytes(w, &now_size);
    const bool same = now_size == size && memcmp(now, bytes, size) == 0;
    free(now);
    free(bytes);
    return same;
}

static bool same_color(const tide_color a, const tide_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool at(const tide_float2 v, const float x, const float y)
{
    return v.x == x && v.y == y;
}

static const tide_draw_command *command(const uint32_t i)
{
    return &draw.commands[i];
}

static void draw_frame(void)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    tide_local_init(&local);
    tide_draw_reset(&draw);
    tide_frame(&world, NULL, 1.0f, &local, &draw, &gui);
}

TIDE_TEST(views_run_in_declaration_order)
{
    draw_frame();
    TIDE_REQUIRE(draw.count == 11);
    TIDE_CHECK(draw.dropped == 0);
    TIDE_CHECK(command(0)->kind == TIDE_DRAW_CLEAR);
    TIDE_CHECK(command(3)->kind == TIDE_DRAW_CIRCLE);
    TIDE_CHECK(command(7)->kind == TIDE_DRAW_LINE);
}

TIDE_TEST(views_once_per_frame)
{
    draw_frame();
    TIDE_CHECK(same_color(command(0)->color, (tide_color){0.1f, 0.2f, 0.3f, 1.0f}));
    TIDE_CHECK(command(1)->kind == TIDE_DRAW_CAMERA);
    TIDE_CHECK(at(command(1)->a, 1.0f, 0.0f)); // Time.tick after one tick
    TIDE_CHECK(command(1)->b.x == 10.0f);
    TIDE_CHECK(command(2)->kind == TIDE_DRAW_TEXT);
    TIDE_CHECK(strcmp(draw.text + command(2)->text, "tick") == 0);
    TIDE_CHECK(same_color(command(2)->color, TIDE_COLOR_WHITE));
}

TIDE_TEST(views_per_entity_with_filters)
{
    draw_frame();
    // First body: Tint's default color, Color.red.
    TIDE_CHECK(at(command(3)->a, 1.0f, 2.0f) && command(3)->b.x == 1.0f);
    TIDE_CHECK(same_color(command(3)->color, TIDE_COLOR_RED));
    TIDE_CHECK(command(4)->kind == TIDE_DRAW_WIRE_RECT && at(command(4)->b, 2.0f, 2.0f));
    // Second body: its own color, and a local copy with alpha changed.
    TIDE_CHECK(at(command(5)->a, 3.0f, 4.0f) && command(5)->b.x == 2.0f);
    TIDE_CHECK(same_color(command(5)->color, (tide_color){0.0f, 0.5f, 1.0f, 0.25f}));
    TIDE_CHECK(same_color(command(6)->color, (tide_color){0.0f, 0.5f, 1.0f, 1.0f}));
    // The hidden body only shows in Hints.
    TIDE_CHECK(at(command(7)->a, 5.0f, 6.0f) && at(command(7)->b, 6.0f, 6.0f));
    TIDE_CHECK(same_color(command(7)->color, TIDE_COLOR_YELLOW));
    TIDE_CHECK(command(8)->kind == TIDE_DRAW_WIRE_CIRCLE && same_color(command(8)->color, TIDE_COLOR_WHITE));
    TIDE_CHECK(command(9)->kind == TIDE_DRAW_RECT && at(command(9)->b, 2.0f, 1.0f));
    TIDE_CHECK(command(10)->kind == TIDE_DRAW_TEXT);
    TIDE_CHECK(strcmp(draw.text + command(10)->text, "say \"hi\"?") == 0);
}

TIDE_TEST(views_leave_the_world_alone)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    uint32_t size;
    uint8_t *before = world_bytes(&world, &size);
    tide_local_init(&local);
    tide_draw_reset(&draw);
    tide_frame(&world, NULL, 1.0f, &local, &draw, &gui);
    tide_frame(&world, NULL, 1.0f, &local, &draw, &gui);
    TIDE_CHECK(world_is(&world, before, size));
    TIDE_CHECK(draw.count == 22); // Drawing twice without a reset adds up
}

TIDE_TEST(views_colors_in_the_world)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(same_color(world.Palette.background, (tide_color){0.1f, 0.2f, 0.3f, 1.0f}));
    const Tint *tint = tide_get_Tint(&world, (tide_entity){1, 1});
    TIDE_REQUIRE(tint != NULL);
    TIDE_CHECK(same_color(tint->color, TIDE_COLOR_RED));
}
