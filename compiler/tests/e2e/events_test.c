#include "game.h"
#include "tide_test.h"

static tide_world world;

// Entity handles are allocated as spawns are recorded. The Main scene is first;
// its Setup spawns a, b, c, a vanisher, a bomb, the player and a mirror; then,
// while those changes apply, the player's weapon and the loot it drops.
static const tide_entity A = {1, 1};
static const tide_entity C = {3, 1};
static const tide_entity PLAYER = {6, 1};
static const tide_entity WEAPON = {8, 1};
static const tide_entity PLAYER_LOOT = {9, 1};

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

TIDE_TEST(events_sent_in_main_are_handled_as_it_applies)
{
    tide_world_init(&world, 1.0f);
    // a took 1, then 2 more that the mirror passed on in a later round.
    TIDE_REQUIRE(tide_get_Health(&world, A) != NULL);
    TIDE_CHECK(tide_get_Health(&world, A)->value == 7);
    // a, b, the mirror, the player, and the mirror's Hit passed on to a.
    TIDE_CHECK(world.Stats.hits == 5);
    TIDE_CHECK(world.Stats.pings == 1);
    TIDE_CHECK(world.command_count == 0);
}

TIDE_TEST(events_handlers_run_in_order)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Stats.order == 213); // Zeroth has [Before(First)]
}

TIDE_TEST(events_spawned_and_destroyed_run_as_changes_apply)
{
    tide_world_init(&world, 1.0f);
    // The Main scene, seven from its Setup, the player's weapon, and the loot the player dropped.
    TIDE_CHECK(world.Stats.spawned == 10);
    TIDE_REQUIRE(tide_get_Weapon(&world, WEAPON) != NULL);
    TIDE_CHECK(tide_entity_equal(tide_get_Weapon(&world, WEAPON)->owner, PLAYER));
    // The player's Hit took all its health, so TakeHit destroyed it, and
    // DropLoot read its Target as it went.
    TIDE_CHECK(!tide_entity_alive(&world.entities, PLAYER));
    TIDE_CHECK(world.Stats.destroyed == 1);
    TIDE_REQUIRE(tide_get_Loot(&world, PLAYER_LOOT) != NULL);
    TIDE_CHECK(tide_get_Loot(&world, PLAYER_LOOT)->from == 7);
}

TIDE_TEST(events_follow_the_order_they_were_recorded_in)
{
    tide_world_init(&world, 1.0f);
    ticks(1);
    // The bomb's Hit came before its Destroy.
    TIDE_CHECK(tide_get_Health(&world, A)->value == 3);
    // c was destroyed before its Hit's turn, so that Hit was dropped.
    TIDE_CHECK(!tide_entity_alive(&world.entities, C));
    TIDE_CHECK(world.Stats.hits == 6);
    TIDE_CHECK(world.Stats.destroyed == 2);
    TIDE_CHECK(world.Stats.spawned == 11);
}

TIDE_TEST(events_sent_to_the_world)
{
    tide_world_init(&world, 1.0f);
    ticks(2);
    TIDE_CHECK(world.Stats.rounds == 0);
    ticks(1);
    TIDE_CHECK(world.Stats.rounds == 1);
}

TIDE_TEST(events_player_joined_waits_for_the_next_tick)
{
    tide_world_init(&world, 1.0f);
    tide_world_player_joined(&world, tide_player_from_index(2));
    TIDE_CHECK(world.Stats.joined == 0);
    ticks(1);
    TIDE_CHECK(world.Stats.joined == 1);
    TIDE_CHECK(tide_player_index(world.Stats.lastJoined) == 2);
}
