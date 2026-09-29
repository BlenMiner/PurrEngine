#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_world before;
static purr_local local;
static purr_draw_list draw;

static bool same_color(const purr_color a, const purr_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static bool at(const purr_float2 v, const float x, const float y)
{
    return v.x == x && v.y == y;
}

static const purr_draw_command *command(const uint32_t i)
{
    return &draw.commands[i];
}

static void draw_frame(void)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    purr_local_init(&local);
    purr_draw_reset(&draw);
    purr_frame(&world, &local, &draw);
}

PURR_TEST(views_run_in_declaration_order)
{
    draw_frame();
    PURR_REQUIRE(draw.count == 11);
    PURR_CHECK(draw.dropped == 0);
    PURR_CHECK(command(0)->kind == PURR_DRAW_CLEAR);
    PURR_CHECK(command(3)->kind == PURR_DRAW_CIRCLE);
    PURR_CHECK(command(7)->kind == PURR_DRAW_LINE);
}

PURR_TEST(views_once_per_frame)
{
    draw_frame();
    PURR_CHECK(same_color(command(0)->color, (purr_color){0.1f, 0.2f, 0.3f, 1.0f}));
    PURR_CHECK(command(1)->kind == PURR_DRAW_CAMERA);
    PURR_CHECK(at(command(1)->a, 1.0f, 0.0f)); // Time.tick after one tick
    PURR_CHECK(command(1)->b.x == 10.0f);
    PURR_CHECK(command(2)->kind == PURR_DRAW_TEXT);
    PURR_CHECK(strcmp(draw.text + command(2)->text, "tick") == 0);
    PURR_CHECK(same_color(command(2)->color, PURR_COLOR_WHITE));
}

PURR_TEST(views_per_entity_with_filters)
{
    draw_frame();
    // First body: Tint's default color, Color.red.
    PURR_CHECK(at(command(3)->a, 1.0f, 2.0f) && command(3)->b.x == 1.0f);
    PURR_CHECK(same_color(command(3)->color, PURR_COLOR_RED));
    PURR_CHECK(command(4)->kind == PURR_DRAW_WIRE_RECT && at(command(4)->b, 2.0f, 2.0f));
    // Second body: its own color, and a local copy with alpha changed.
    PURR_CHECK(at(command(5)->a, 3.0f, 4.0f) && command(5)->b.x == 2.0f);
    PURR_CHECK(same_color(command(5)->color, (purr_color){0.0f, 0.5f, 1.0f, 0.25f}));
    PURR_CHECK(same_color(command(6)->color, (purr_color){0.0f, 0.5f, 1.0f, 1.0f}));
    // The hidden body only shows in Hints.
    PURR_CHECK(at(command(7)->a, 5.0f, 6.0f) && at(command(7)->b, 6.0f, 6.0f));
    PURR_CHECK(same_color(command(7)->color, PURR_COLOR_YELLOW));
    PURR_CHECK(command(8)->kind == PURR_DRAW_WIRE_CIRCLE && same_color(command(8)->color, PURR_COLOR_WHITE));
    PURR_CHECK(command(9)->kind == PURR_DRAW_RECT && at(command(9)->b, 2.0f, 1.0f));
    PURR_CHECK(command(10)->kind == PURR_DRAW_TEXT);
    PURR_CHECK(strcmp(draw.text + command(10)->text, "say \"hi\"?") == 0);
}

PURR_TEST(views_leave_the_world_alone)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    memcpy(&before, &world, sizeof world);
    purr_local_init(&local);
    purr_draw_reset(&draw);
    purr_frame(&world, &local, &draw);
    purr_frame(&world, &local, &draw);
    PURR_CHECK(memcmp(&before, &world, sizeof world) == 0);
    PURR_CHECK(draw.count == 22); // Drawing twice without a reset adds up
}

PURR_TEST(views_colors_in_the_world)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(same_color(world.Palette.background, (purr_color){0.1f, 0.2f, 0.3f, 1.0f}));
    const Tint *tint = purr_get_Tint(&world, (purr_entity){1, 1});
    PURR_REQUIRE(tint != NULL);
    PURR_CHECK(same_color(tint->color, PURR_COLOR_RED));
}
