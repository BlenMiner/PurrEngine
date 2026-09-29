#include "game.h"
#include "purr_test.h"

static purr_world world;

// The Main scene is slot 0; its Setup loads the arena (1) and the hand (2); the
// arena's Setup spawns a floor (3) and two enemies (4 and 5).
static const purr_entity FLOOR = {3, 1};

static void ticks(int n)
{
    for (int i = 0; i < n; i++) purr_world_tick(&world);
}

PURR_TEST(scenes_load_and_set_up)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(purr_world_entity_count(&world) == 6);
    PURR_CHECK(world.Stats.arenasSetUp == 1);
    PURR_REQUIRE(purr_get_Arena(&world, world.Stats.arena) != NULL);
    PURR_CHECK(purr_get_Arena(&world, world.Stats.arena)->size == 30);
    PURR_REQUIRE(purr_get_Floor(&world, FLOOR) != NULL);
    PURR_CHECK(purr_get_Floor(&world, FLOOR)->size == 30);
}

PURR_TEST(scenes_remember_who_sees_them)
{
    purr_world_init(&world, 1.0f);
    const Hand *hand = purr_get_Hand(&world, world.Stats.hand);
    PURR_REQUIRE(hand != NULL);
    PURR_CHECK(hand->purr_visibility == SceneVisibility_Private);
    PURR_CHECK(hand->purr_players == 1 << 1); // Player 1; player 3 was added, then removed
    PURR_CHECK(purr_get_Arena(&world, world.Stats.arena)->purr_visibility == SceneVisibility_Public);
}

PURR_TEST(scenes_unload_everything_in_them)
{
    purr_world_init(&world, 1.0f);
    ticks(1);
    PURR_CHECK(purr_world_entity_count(&world) == 8); // Each enemy dropped loot into the arena
    ticks(2);
    // The arena, its floor, its enemies and their loot are gone; Main and the hand stay.
    PURR_CHECK(purr_world_entity_count(&world) == 2);
    PURR_CHECK(!purr_entity_alive(&world.entities, world.Stats.arena));
    PURR_CHECK(purr_get_Hand(&world, world.Stats.hand) != NULL);
    PURR_CHECK(world.Stats.destroyed == 6);
    PURR_CHECK(world.Stats.lootDropped == 2); // Dropped into the arena as it went, so never made
    ticks(1);
    PURR_CHECK(purr_world_entity_count(&world) == 2);
}
