#include <string.h>

#include "game.h"
#include "tide_test.h"

// Worlds are large, so keep them out of the stack.
static tide_world world;
static tide_world snapshot;

// Archetypes created by spawns come first, in the order they appear in the source.
#define PLAYERS world.arch0_Transform_Player
#define PROPS world.arch1_Transform_Prop

TIDE_TEST(move_clamp_main_spawns)
{
    tide_world_init(&world, 0.5f);
    TIDE_CHECK(tide_world_entity_count(&world) == 3);
    TIDE_REQUIRE(PLAYERS.count == 1);
    TIDE_REQUIRE(PROPS.count == 1);
    TIDE_CHECK(PLAYERS.Transform[0].position.y == 10.0f);
    TIDE_CHECK(PLAYERS.Transform[0].scale.x == 1.0f);
    TIDE_CHECK(PLAYERS.Transform[0].rotation.x == 0.0f); // Unset fields are zero.
    TIDE_CHECK(PROPS.Prop[0].id == 7);
}

TIDE_TEST(move_clamp_moves_players)
{
    tide_world_init(&world, 0.5f);
    tide_world_tick(&world);
    TIDE_CHECK(PLAYERS.Transform[0].position.x == 0.5f);
    TIDE_CHECK(PLAYERS.Transform[0].position.y == 9.0f);
    TIDE_CHECK(world.Time.tick == 1);
}

TIDE_TEST(move_clamp_clamps_players_at_zero)
{
    tide_world_init(&world, 0.5f);
    for (int i = 0; i < 20; i++) tide_world_tick(&world);
    TIDE_CHECK(PLAYERS.Transform[0].position.x == 10.0f);
    TIDE_CHECK(PLAYERS.Transform[0].position.y == 0.0f);
}

TIDE_TEST(move_clamp_leaves_props_alone)
{
    tide_world_init(&world, 0.5f);
    for (int i = 0; i < 20; i++) tide_world_tick(&world);
    TIDE_CHECK(PROPS.Transform[0].position.x == 0.0f);
    TIDE_CHECK(PROPS.Transform[0].position.y == 10.0f);
}

// Rollback: restoring a snapshot and re-running the same ticks gives
// bit-identical results.
TIDE_TEST(move_clamp_snapshot_restore_is_exact)
{
    tide_world_init(&world, 1.0f / 60.0f);
    for (int i = 0; i < 5; i++) tide_world_tick(&world);
    snapshot = world;

    for (int i = 0; i < 100; i++) tide_world_tick(&world);
    tide_float3 first = PLAYERS.Transform[0].position;

    world = snapshot;
    for (int i = 0; i < 100; i++) tide_world_tick(&world);
    tide_float3 second = PLAYERS.Transform[0].position;

    TIDE_CHECK(memcmp(&first, &second, sizeof first) == 0);
    TIDE_CHECK(world.Time.tick == 105);
}
