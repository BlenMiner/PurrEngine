#include "game.h"
#include "purr_test.h"

static purr_world world;

static void ticks(int n)
{
    for (int i = 0; i < n; i++) purr_world_tick(&world);
}

PURR_TEST(enums_have_their_values)
{
    PURR_CHECK(Phase_Warmup == 0);
    PURR_CHECK(Phase_Playing == 5);
    PURR_CHECK(Phase_Over == 6); // One more than the member before it
    PURR_CHECK(Weapon_Bow == 1);
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Match.phase == Phase_Warmup);
    PURR_CHECK(world.Match.loadout.primary == Weapon_Bow); // The struct's default
}

PURR_TEST(enums_switch_between_phases)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_server_input(&world, (Controls){.weapon = Weapon_Bow});
    ticks(2);
    PURR_CHECK(world.Match.phase == Phase_Playing);
    PURR_CHECK(world.Match.phaseValue == 5);
    ticks(3);
    // Points for ticks 2, 3 and 4: 0, 10 and 1.
    PURR_CHECK(world.Match.score == 11);
    PURR_CHECK(world.Match.phase == Phase_Over);
    PURR_CHECK(world.Match.phaseValue == 6);
    ticks(1);
    PURR_CHECK(world.Match.phase == Phase_Over); // default: nothing happens
}

PURR_TEST(enums_from_other_machines_are_repaired)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_server_input(&world, (Controls){.weapon = 7});
    ticks(1);
    PURR_CHECK(world.Match.loadout.primary == Weapon_Sword); // 7 isn't a Weapon, so it's the default
    purr_world_set_server_input(&world, (Controls){.weapon = Weapon_Bow});
    ticks(1);
    PURR_CHECK(world.Match.loadout.primary == Weapon_Bow);
}
