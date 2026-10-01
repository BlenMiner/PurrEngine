#include "game.h"
#include "purr_test.h"

static purr_world world;

static void ticks(int n)
{
    for (int i = 0; i < n; i++) purr_world_tick(&world);
}

PURR_TEST(scenes_fallback_main_loads_when_the_last_scene_unloads)
{
    purr_world_init(&world, 1.0f);
    // Main loaded the arena and left.
    PURR_CHECK(world.Stats.mainLoads == 1);
    PURR_CHECK(world.Stats.arenaLoads == 1);
    PURR_CHECK(purr_world_entity_count(&world) == 1);
    ticks(1);
    PURR_CHECK(purr_world_entity_count(&world) == 2); // The arena and the wall
    ticks(1);
    // The arena unloaded, so Main loaded again, and loaded another one. The
    // wall is in no scene, so it didn't count.
    PURR_CHECK(world.Stats.mainLoads == 2);
    PURR_CHECK(world.Stats.arenaLoads == 2);
    PURR_CHECK(purr_world_entity_count(&world) == 2);
}

PURR_TEST(scenes_fallback_main_loads_once_a_tick)
{
    purr_world_init(&world, 1.0f);
    ticks(4);
    // The third Main left nothing loaded.
    PURR_CHECK(world.Stats.mainLoads == 3);
    PURR_CHECK(world.Stats.arenaLoads == 2);
    PURR_CHECK(purr_world_entity_count(&world) == 1); // The wall
    ticks(1);
    PURR_CHECK(world.Stats.mainLoads == 4);
    ticks(2);
    PURR_CHECK(world.Stats.mainLoads == 6);
    PURR_CHECK(purr_world_entity_count(&world) == 1);
}
