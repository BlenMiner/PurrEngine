#include "game.h"
#include "purr_test.h"

static purr_world world;

static const purr_entity first = {0, 1};
static const purr_entity second = {1, 1};

// One tick: Fight, Rage, Record, Mend, ReadInput.
static void one_tick(void)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
}

PURR_TEST(methods_change_fields_through_mut)
{
    one_tick();
    const Unit *a = purr_get_Unit(&world, first);
    const Unit *b = purr_get_Unit(&world, second);
    PURR_REQUIRE(a != NULL && b != NULL);
    // TakeHit: 100 - 1, then Mend heals 2.
    PURR_CHECK(a->stats.health == 101.0f && a->kills == 0);
    // TakeHit: 1 - 5 is dead, so 0; Kill; then Enrage doubles the damage and hits again, still 0.
    PURR_CHECK(b->stats.health == 0.0f && b->kills == 1);
    PURR_CHECK(b->stats.damage.hi == 10.0f);
}

PURR_TEST(methods_and_functions_return_values)
{
    one_tick();
    PURR_CHECK(world.Log.width == 30.0f);        // Wider (x3) of the second unit's 0..10, the last recorded
    PURR_CHECK(world.Log.literal_width == 3.0f); // On a value that isn't stored
    PURR_CHECK(world.Log.healed == 101.0f);      // Heal returns the new health
    PURR_CHECK(world.Log.alive == 1);
}

PURR_TEST(functions_change_their_mut_arguments)
{
    one_tick();
    PURR_CHECK(world.Log.bumps == 2); // Bump(log.bumps) for each unit
}

PURR_TEST(functions_run_in_sample)
{
    purr_world_init(&world, 1.0f);
    const purr_devices devices = {0};
    purr_world_set_server_input(&world, purr_input_sample(&devices));
    purr_world_tick(&world);
    PURR_CHECK(world.Log.sampled.x == 0.6f && world.Log.sampled.y == 0.8f); // (3, 4) normalized
}
