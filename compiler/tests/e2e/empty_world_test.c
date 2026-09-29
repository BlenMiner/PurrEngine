#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(empty_world_ticks_with_no_entities)
{
    purr_world_init(&world, 0.5f);
    PURR_CHECK(purr_world_entity_count(&world) == 1); // Only the Main scene
    for (int i = 0; i < 3; i++) purr_world_tick(&world);
    PURR_CHECK(purr_world_entity_count(&world) == 1);
    PURR_CHECK(world.Time.tick == 3);
    PURR_CHECK(world.Time.dt == 0.5f);
}
