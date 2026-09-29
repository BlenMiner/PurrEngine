#include "game.h"
#include "purr_test.h"

static purr_world world;

// The Main scene is slot 0, and its Setup spawns these first, so they get slots 1 and 2.
static const purr_entity FIRST = {1, 1};
static const purr_entity EVERYTHING = {2, 1};

PURR_TEST(edge_cases_every_field_type_round_trips)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(purr_world_entity_count(&world) == 4); // The Main scene and three from its Setup

    Everything *e = purr_get_Everything(&world, EVERYTHING);
    PURR_REQUIRE(e != NULL);
    PURR_CHECK(e->flag);
    PURR_CHECK(e->count == 1);
    PURR_CHECK(e->amount == 0.5f);
    PURR_CHECK(e->offset.x == 1.0f && e->offset.y == 2.0f && e->offset.z == 3.0f);
    PURR_CHECK(purr_entity_equal(e->other, FIRST));
    PURR_CHECK(e->double_ == 2);     // C keywords get a trailing underscore.
    PURR_CHECK(e->static_ == 0.0f);
}

PURR_TEST(edge_cases_singleton_systems_run_once_per_tick)
{
    purr_world_init(&world, 1.0f);
    for (int i = 0; i < 3; i++) purr_world_tick(&world);
    PURR_CHECK(world.Counter.ticks == 3);
}

PURR_TEST(edge_cases_keyword_locals)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    Everything *e = purr_get_Everything(&world, EVERYTHING);
    PURR_REQUIRE(e != NULL);
    PURR_CHECK(e->double_ == 3); // 1 * 2 + 1
    PURR_CHECK(e->count == 3);
    purr_world_tick(&world);
    PURR_CHECK(e->double_ == 7); // 3 * 2 + 1
}

PURR_TEST(edge_cases_destroy_without_add_or_remove)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    PURR_CHECK(world.Counter.destroyed == 1);
    PURR_CHECK(purr_world_entity_count(&world) == 3);
    purr_world_tick(&world);
    PURR_CHECK(world.Counter.destroyed == 1);
}
