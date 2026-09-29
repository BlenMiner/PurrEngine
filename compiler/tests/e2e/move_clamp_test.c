#include <string.h>

#include "game.h"
#include "purr_test.h"

// Worlds are large, so keep them out of the stack.
static purr_world world;
static purr_world snapshot;

// Archetypes created by spawns come first, in the order they appear in the source.
#define PLAYERS world.arch0_Transform_Player
#define PROPS world.arch1_Transform_Prop

PURR_TEST(move_clamp_main_spawns)
{
    purr_world_init(&world, 0.5f);
    PURR_CHECK(purr_world_entity_count(&world) == 3);
    PURR_REQUIRE(PLAYERS.count == 1);
    PURR_REQUIRE(PROPS.count == 1);
    PURR_CHECK(PLAYERS.Transform[0].position.y == 10.0f);
    PURR_CHECK(PLAYERS.Transform[0].scale.x == 1.0f);
    PURR_CHECK(PLAYERS.Transform[0].rotation.x == 0.0f); // Unset fields are zero.
    PURR_CHECK(PROPS.Prop[0].id == 7);
}

PURR_TEST(move_clamp_moves_players)
{
    purr_world_init(&world, 0.5f);
    purr_world_tick(&world);
    PURR_CHECK(PLAYERS.Transform[0].position.x == 0.5f);
    PURR_CHECK(PLAYERS.Transform[0].position.y == 9.0f);
    PURR_CHECK(world.Time.tick == 1);
}

PURR_TEST(move_clamp_clamps_players_at_zero)
{
    purr_world_init(&world, 0.5f);
    for (int i = 0; i < 20; i++) purr_world_tick(&world);
    PURR_CHECK(PLAYERS.Transform[0].position.x == 10.0f);
    PURR_CHECK(PLAYERS.Transform[0].position.y == 0.0f);
}

PURR_TEST(move_clamp_leaves_props_alone)
{
    purr_world_init(&world, 0.5f);
    for (int i = 0; i < 20; i++) purr_world_tick(&world);
    PURR_CHECK(PROPS.Transform[0].position.x == 0.0f);
    PURR_CHECK(PROPS.Transform[0].position.y == 10.0f);
}

// Rollback: restoring a snapshot and re-running the same ticks gives
// bit-identical results.
PURR_TEST(move_clamp_snapshot_restore_is_exact)
{
    purr_world_init(&world, 1.0f / 60.0f);
    for (int i = 0; i < 5; i++) purr_world_tick(&world);
    snapshot = world;

    for (int i = 0; i < 100; i++) purr_world_tick(&world);
    purr_float3 first = PLAYERS.Transform[0].position;

    world = snapshot;
    for (int i = 0; i < 100; i++) purr_world_tick(&world);
    purr_float3 second = PLAYERS.Transform[0].position;

    PURR_CHECK(memcmp(&first, &second, sizeof first) == 0);
    PURR_CHECK(world.Time.tick == 105);
}
