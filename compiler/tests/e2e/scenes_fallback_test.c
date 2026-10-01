#include "game.h"
#include "tide_test.h"

static tide_world world;

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

TIDE_TEST(scenes_fallback_main_loads_when_the_last_scene_unloads)
{
    tide_world_init(&world, 1.0f);
    // Main loaded the arena and left.
    TIDE_CHECK(world.Stats.mainLoads == 1);
    TIDE_CHECK(world.Stats.arenaLoads == 1);
    TIDE_CHECK(tide_world_entity_count(&world) == 1);
    ticks(1);
    TIDE_CHECK(tide_world_entity_count(&world) == 2); // The arena and the wall
    ticks(1);
    // The arena unloaded, so Main loaded again, and loaded another one. The
    // wall is in no scene, so it didn't count.
    TIDE_CHECK(world.Stats.mainLoads == 2);
    TIDE_CHECK(world.Stats.arenaLoads == 2);
    TIDE_CHECK(tide_world_entity_count(&world) == 2);
}

TIDE_TEST(scenes_fallback_main_loads_once_a_tick)
{
    tide_world_init(&world, 1.0f);
    ticks(4);
    // The third Main left nothing loaded.
    TIDE_CHECK(world.Stats.mainLoads == 3);
    TIDE_CHECK(world.Stats.arenaLoads == 2);
    TIDE_CHECK(tide_world_entity_count(&world) == 1); // The wall
    ticks(1);
    TIDE_CHECK(world.Stats.mainLoads == 4);
    ticks(2);
    TIDE_CHECK(world.Stats.mainLoads == 6);
    TIDE_CHECK(tide_world_entity_count(&world) == 1);
}
