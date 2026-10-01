#include "game.h"
#include "tide_test.h"

static tide_world world;

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

TIDE_TEST(enums_have_their_values)
{
    TIDE_CHECK(Phase_Warmup == 0);
    TIDE_CHECK(Phase_Playing == 5);
    TIDE_CHECK(Phase_Over == 6); // One more than the member before it
    TIDE_CHECK(Weapon_Bow == 1);
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Match.phase == Phase_Warmup);
    TIDE_CHECK(world.Match.loadout.primary == Weapon_Bow); // The struct's default
}

TIDE_TEST(enums_switch_between_phases)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_server_input(&world, (Controls){.weapon = Weapon_Bow});
    ticks(2);
    TIDE_CHECK(world.Match.phase == Phase_Playing);
    TIDE_CHECK(world.Match.phaseValue == 5);
    ticks(3);
    // Points for ticks 2, 3 and 4: 0, 10 and 1.
    TIDE_CHECK(world.Match.score == 11);
    TIDE_CHECK(world.Match.phase == Phase_Over);
    TIDE_CHECK(world.Match.phaseValue == 6);
    ticks(1);
    TIDE_CHECK(world.Match.phase == Phase_Over); // default: nothing happens
}

TIDE_TEST(enums_from_other_machines_are_repaired)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_server_input(&world, (Controls){.weapon = 7});
    ticks(1);
    TIDE_CHECK(world.Match.loadout.primary == Weapon_Sword); // 7 isn't a Weapon, so it's the default
    tide_world_set_server_input(&world, (Controls){.weapon = Weapon_Bow});
    ticks(1);
    TIDE_CHECK(world.Match.loadout.primary == Weapon_Bow);
}
