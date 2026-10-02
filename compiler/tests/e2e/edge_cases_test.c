#include "game.h"
#include "tide_test.h"

static tide_world world;

// The Main scene is slot 0, and its Setup spawns these first, so they get slots 1 and 2.
static const tide_entity FIRST = {1, 1};
static const tide_entity EVERYTHING = {2, 1};

TIDE_TEST(edge_cases_every_field_type_round_trips)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(tide_world_entity_count(&world) == 4); // The Main scene and three from its Setup

    Everything e = tide_get_Everything(&world, EVERYTHING);
    TIDE_REQUIRE(tide_has_Everything(&world, EVERYTHING));
    TIDE_CHECK(e.flag);
    TIDE_CHECK(e.count == 1);
    TIDE_CHECK(e.amount == 0.5f);
    TIDE_CHECK(e.offset.x == 1.0f && e.offset.y == 2.0f && e.offset.z == 3.0f);
    TIDE_CHECK(tide_entity_equal(e.other, FIRST));
    TIDE_CHECK(e.double_ == 2);     // C keywords get a trailing underscore.
    TIDE_CHECK(e.static_ == 0.0f);
}

TIDE_TEST(edge_cases_singleton_systems_run_once_per_tick)
{
    tide_world_init(&world, 1.0f);
    for (int i = 0; i < 3; i++) tide_world_tick(&world);
    TIDE_CHECK(world.Counter.ticks == 3);
}

TIDE_TEST(edge_cases_keyword_locals)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    Everything e = tide_get_Everything(&world, EVERYTHING);
    TIDE_REQUIRE(tide_has_Everything(&world, EVERYTHING));
    TIDE_CHECK(e.double_ == 3); // 1 * 2 + 1
    TIDE_CHECK(e.count == 3);
    tide_world_tick(&world);
    TIDE_CHECK(tide_get_Everything(&world, EVERYTHING).double_ == 7); // 3 * 2 + 1
}

TIDE_TEST(edge_cases_destroy_without_add_or_remove)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    TIDE_CHECK(world.Counter.destroyed == 1);
    TIDE_CHECK(tide_world_entity_count(&world) == 3);
    tide_world_tick(&world);
    TIDE_CHECK(world.Counter.destroyed == 1);
}
