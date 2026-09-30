#include <stdio.h>
#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_local local;
static purr_draw_list draw;
static purr_gui gui;

// The Main scene is slot 0, and its Setup spawns the unit first.
static const purr_entity UNIT = {1, 1};

PURR_TEST(default_value_all_checks_pass)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    const Results *r = purr_get_Results(&world, UNIT);
    PURR_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    PURR_CHECK(r->checks == 23);
    PURR_CHECK(r->passed == r->checks);
    PURR_CHECK(world.heap.failed == 0);
}

PURR_TEST(default_value_field_defaults)
{
    purr_world_init(&world, 1.0f);
    const Unit *u = purr_get_Unit(&world, UNIT);
    PURR_REQUIRE(u != NULL);
    PURR_CHECK(u->level == 5);
    PURR_CHECK(u->reset == 0);
    PURR_CHECK(purr_entity_is_null(u->target));
    PURR_CHECK(u->stats.armor == 3);
    PURR_CHECK(u->stats.reach.hi == 1.0f);
}

PURR_TEST(default_value_draw_arguments)
{
    purr_world_init(&world, 1.0f);
    purr_local_init(&local);
    purr_draw_reset(&draw);
    purr_frame(&world, NULL, 1.0f, &local, &draw, &gui);
    PURR_REQUIRE(draw.count == 2);
    const purr_draw_command *circle = &draw.commands[0];
    PURR_CHECK(circle->kind == PURR_DRAW_CIRCLE);
    PURR_CHECK(circle->a.x == 0.0f && circle->a.y == 0.0f);
    PURR_CHECK(circle->b.x == 5.0f);
    const purr_draw_command *text = &draw.commands[1];
    PURR_CHECK(text->kind == PURR_DRAW_TEXT);
    PURR_CHECK(strcmp(draw.text + text->text, "") == 0);
    PURR_CHECK(text->color.a == 0.0f);
}
