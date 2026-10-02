#include "game.h"
#include "tide_test.h"

static tide_world world;

// The Main scene is slot 0; its Setup loads the arena (1) and the hand (2); the
// arena's Setup spawns a floor (3) and two enemies (4 and 5).
static const tide_entity FLOOR = {3, 1};

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

TIDE_TEST(scenes_load_and_set_up)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(tide_world_entity_count(&world) == 6);
    TIDE_CHECK(world.Stats.arenasSetUp == 1);
    TIDE_REQUIRE(tide_has_Arena(&world, world.Stats.arena));
    TIDE_CHECK(tide_get_Arena(&world, world.Stats.arena).size == 30);
    TIDE_REQUIRE(tide_has_Floor(&world, FLOOR));
    TIDE_CHECK(tide_get_Floor(&world, FLOOR).size == 30);
}

TIDE_TEST(scenes_remember_who_sees_them)
{
    tide_world_init(&world, 1.0f);
    const Hand hand = tide_get_Hand(&world, world.Stats.hand);
    TIDE_REQUIRE(tide_has_Hand(&world, world.Stats.hand));
    TIDE_CHECK(hand.tide_visibility == SceneVisibility_Private);
    TIDE_CHECK(hand.tide_players == 1 << 1); // Player 1; player 3 was added, then removed
    TIDE_CHECK(tide_get_Arena(&world, world.Stats.arena).tide_visibility == SceneVisibility_Public);
}

TIDE_TEST(scenes_unload_everything_in_them)
{
    tide_world_init(&world, 1.0f);
    ticks(1);
    TIDE_CHECK(tide_world_entity_count(&world) == 8); // Each enemy dropped loot into the arena
    ticks(2);
    // The arena, its floor, its enemies and their loot are gone; Main and the hand stay.
    TIDE_CHECK(tide_world_entity_count(&world) == 2);
    TIDE_CHECK(!tide_entity_alive(&world.entities, world.Stats.arena));
    TIDE_CHECK(tide_has_Hand(&world, world.Stats.hand));
    TIDE_CHECK(world.Stats.destroyed == 6);
    TIDE_CHECK(world.Stats.lootDropped == 2); // Dropped into the arena as it went, so never made
    ticks(1);
    TIDE_CHECK(tide_world_entity_count(&world) == 2);
}
