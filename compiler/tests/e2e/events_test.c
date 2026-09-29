#include "game.h"
#include "purr_test.h"

static purr_world world;

// Entity handles are allocated as spawns are recorded. Main spawns a, b, c, a
// vanisher, a bomb, the player and a mirror; then, while Main's changes apply,
// the player's weapon and the loot it drops.
static const purr_entity A = {0, 1};
static const purr_entity C = {2, 1};
static const purr_entity PLAYER = {5, 1};
static const purr_entity WEAPON = {7, 1};
static const purr_entity PLAYER_LOOT = {8, 1};

static void ticks(int n)
{
    for (int i = 0; i < n; i++) purr_world_tick(&world);
}

PURR_TEST(events_sent_in_main_are_handled_as_it_applies)
{
    purr_world_init(&world, 1.0f);
    // a took 1, then 2 more that the mirror passed on in a later round.
    PURR_REQUIRE(purr_get_Health(&world, A) != NULL);
    PURR_CHECK(purr_get_Health(&world, A)->value == 7);
    // a, b, the mirror, the player, and the mirror's Hit passed on to a.
    PURR_CHECK(world.Stats.hits == 5);
    PURR_CHECK(world.Stats.pings == 1);
    PURR_CHECK(world.command_count == 0);
}

PURR_TEST(events_handlers_run_in_order)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Stats.order == 213); // Zeroth has [Before(First)]
}

PURR_TEST(events_spawned_and_destroyed_run_as_changes_apply)
{
    purr_world_init(&world, 1.0f);
    // Seven from Main, the player's weapon, and the loot the player dropped.
    PURR_CHECK(world.Stats.spawned == 9);
    PURR_REQUIRE(purr_get_Weapon(&world, WEAPON) != NULL);
    PURR_CHECK(purr_entity_equal(purr_get_Weapon(&world, WEAPON)->owner, PLAYER));
    // The player's Hit took all its health, so TakeHit destroyed it, and
    // DropLoot read its Target as it went.
    PURR_CHECK(!purr_entity_alive(&world.entities, PLAYER));
    PURR_CHECK(world.Stats.destroyed == 1);
    PURR_REQUIRE(purr_get_Loot(&world, PLAYER_LOOT) != NULL);
    PURR_CHECK(purr_get_Loot(&world, PLAYER_LOOT)->from == 7);
}

PURR_TEST(events_follow_the_order_they_were_recorded_in)
{
    purr_world_init(&world, 1.0f);
    ticks(1);
    // The bomb's Hit came before its Destroy.
    PURR_CHECK(purr_get_Health(&world, A)->value == 3);
    // c was destroyed before its Hit's turn, so that Hit was dropped.
    PURR_CHECK(!purr_entity_alive(&world.entities, C));
    PURR_CHECK(world.Stats.hits == 6);
    PURR_CHECK(world.Stats.destroyed == 2);
    PURR_CHECK(world.Stats.spawned == 10);
}

PURR_TEST(events_sent_to_the_world)
{
    purr_world_init(&world, 1.0f);
    ticks(2);
    PURR_CHECK(world.Stats.rounds == 0);
    ticks(1);
    PURR_CHECK(world.Stats.rounds == 1);
}

PURR_TEST(events_player_joined_waits_for_the_next_tick)
{
    purr_world_init(&world, 1.0f);
    purr_world_player_joined(&world, purr_player_from_index(2));
    PURR_CHECK(world.Stats.joined == 0);
    ticks(1);
    PURR_CHECK(world.Stats.joined == 1);
    PURR_CHECK(purr_player_index(world.Stats.lastJoined) == 2);
}
