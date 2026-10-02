#include "game.h"
#include "tide_test.h"

static tide_world world;

static const tide_entity first = {1, 1};
static const tide_entity second = {2, 1};

// One tick: Fight, Rage, Record, Mend, ReadInput.
static void one_tick(void)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
}

TIDE_TEST(methods_change_fields_through_mut)
{
    one_tick();
    const Unit a = tide_get_Unit(&world, first);
    const Unit b = tide_get_Unit(&world, second);
    TIDE_REQUIRE(tide_has_Unit(&world, first) && tide_has_Unit(&world, second));
    // TakeHit: 100 - 1, then Mend heals 2.
    TIDE_CHECK(a.stats.health == 101.0f && a.kills == 0);
    // TakeHit: 1 - 5 is dead, so 0; Kill; then Enrage doubles the damage and hits again, still 0.
    TIDE_CHECK(b.stats.health == 0.0f && b.kills == 1);
    TIDE_CHECK(b.stats.damage.hi == 10.0f);
}

TIDE_TEST(methods_and_functions_return_values)
{
    one_tick();
    TIDE_CHECK(world.Log.width == 30.0f);        // Wider (x3) of the second unit's 0..10, the last recorded
    TIDE_CHECK(world.Log.literal_width == 3.0f); // On a value that isn't stored
    TIDE_CHECK(world.Log.healed == 101.0f);      // Heal returns the new health
    TIDE_CHECK(world.Log.alive == 1);
}

TIDE_TEST(functions_change_their_mut_arguments)
{
    one_tick();
    TIDE_CHECK(world.Log.bumps == 2); // Bump(log.bumps) for each unit
}

TIDE_TEST(component_methods_see_their_entity)
{
    one_tick();
    const tide_entity a = {3, 1};
    const tide_entity b = {4, 1};
    const Tracker ta = tide_get_Tracker(&world, a);
    const Tracker tb = tide_get_Tracker(&world, b);
    TIDE_REQUIRE(tide_has_Tracker(&world, a) && tide_has_Tracker(&world, b));
    TIDE_CHECK(tide_entity_equal(ta.me, a) && tide_entity_equal(tb.me, b));
    TIDE_CHECK(ta.calls == 2 && tb.calls == 2); // Greet as it's spawned, then Watch
    TIDE_CHECK(ta.isMe && tb.isMe);
    TIDE_CHECK(!ta.otherIsMe && !tb.otherIsMe); // a's other is null, b's is a
}

TIDE_TEST(functions_run_in_sample)
{
    tide_world_init(&world, 1.0f);
    const tide_devices devices = {0};
    tide_world_set_server_input(&world, tide_input_sample(&devices, NULL));
    tide_world_tick(&world);
    TIDE_CHECK(world.Log.sampled.x == 0.6f && world.Log.sampled.y == 0.8f); // (3, 4) normalized
}
