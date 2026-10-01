#include <stdio.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

// The Main scene is slot 0, and its Setup spawns the unit first.
static const tide_entity UNIT = {1, 1};

TIDE_TEST(default_value_all_checks_pass)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const Results *r = tide_get_Results(&world, UNIT);
    TIDE_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    TIDE_CHECK(r->checks == 23);
    TIDE_CHECK(r->passed == r->checks);
    TIDE_CHECK(world.heap.failed == 0);
}

TIDE_TEST(default_value_field_defaults)
{
    tide_world_init(&world, 1.0f);
    const Unit *u = tide_get_Unit(&world, UNIT);
    TIDE_REQUIRE(u != NULL);
    TIDE_CHECK(u->level == 5);
    TIDE_CHECK(u->reset == 0);
    TIDE_CHECK(tide_entity_is_null(u->target));
    TIDE_CHECK(u->stats.armor == 3);
    TIDE_CHECK(u->stats.reach.hi == 1.0f);
}

TIDE_TEST(default_value_draw_arguments)
{
    tide_world_init(&world, 1.0f);
    tide_local_init(&local);
    tide_draw_reset(&draw);
    tide_frame(&world, NULL, 1.0f, &local, &draw, &gui);
    TIDE_REQUIRE(draw.count == 2);
    const tide_draw_command *circle = &draw.commands[0];
    TIDE_CHECK(circle->kind == TIDE_DRAW_CIRCLE);
    TIDE_CHECK(circle->a.x == 0.0f && circle->a.y == 0.0f);
    TIDE_CHECK(circle->b.x == 5.0f);
    const tide_draw_command *text = &draw.commands[1];
    TIDE_CHECK(text->kind == TIDE_DRAW_TEXT);
    TIDE_CHECK(strcmp(draw.text + text->text, "") == 0);
    TIDE_CHECK(text->color.a == 0.0f);
}
