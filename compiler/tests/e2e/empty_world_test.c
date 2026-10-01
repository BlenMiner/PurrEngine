#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(empty_world_ticks_with_no_entities)
{
    tide_world_init(&world, 0.5f);
    TIDE_CHECK(tide_world_entity_count(&world) == 1); // Only the Main scene
    for (int i = 0; i < 3; i++) tide_world_tick(&world);
    TIDE_CHECK(tide_world_entity_count(&world) == 1);
    TIDE_CHECK(world.Time.tick == 3);
    TIDE_CHECK(world.Time.dt == 0.5f);
}
