#include <math.h>
#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

static const purr_entity first = {0, 1};
static const purr_entity second = {1, 1};

static bool all_zero(const uint8_t *bytes, const size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

PURR_TEST(struct_defaults_nest)
{
    purr_world_init(&world, 1.0f);
    const Unit *a = purr_get_Unit(&world, first);
    const Unit *b = purr_get_Unit(&world, second);
    PURR_REQUIRE(a != NULL && b != NULL);
    PURR_CHECK(a->stats.alive && a->stats.health == 100.0f);
    PURR_CHECK(a->stats.damage.lo == 0.0f && a->stats.damage.hi == 1.0f); // Range's own default
    PURR_CHECK(a->stats.armor.lo == 0.0f && a->stats.armor.hi == 5.0f);  // The field's default
    PURR_CHECK(b->stats.health == 50.0f && b->stats.damage.lo == 2.0f && b->stats.damage.hi == 3.0f);
    PURR_CHECK(b->stats.alive && b->stats.armor.hi == 5.0f); // Fields the literal leaves out keep their defaults
}

PURR_TEST(struct_fields_write_through_mut)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    const Unit *a = purr_get_Unit(&world, first);
    const Unit *b = purr_get_Unit(&world, second);
    PURR_REQUIRE(a != NULL && b != NULL);
    PURR_CHECK(a->stats.health == 99.0f && a->stats.alive);
    PURR_CHECK(b->stats.health == 47.0f && !b->stats.alive);
}

PURR_TEST(struct_values_are_copies)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    // Copy runs for both units, the second one last: its copy has health 0, and the unit keeps 47.
    PURR_CHECK(world.Log.copy.health == 0.0f);
    PURR_CHECK(world.Log.copy.damage.hi == 3.0f);
    PURR_CHECK(world.Log.copy_health == 47.0f);
}

PURR_TEST(struct_padding_is_explicit_and_zero)
{
    // purrc writes padding out as members, so the parts add up to the size.
    PURR_CHECK(sizeof(Stats) == 24);
    PURR_CHECK(sizeof(Aim) == 12);
    PURR_CHECK(sizeof(PlayerInput) == 16);
    purr_world_init(&world, 1.0f);
    const Unit *a = purr_get_Unit(&world, first);
    PURR_REQUIRE(a != NULL);
    PURR_CHECK(all_zero(a->stats.purr_pad0, sizeof a->stats.purr_pad0));
}

PURR_TEST(struct_input_fields_are_repaired)
{
    purr_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.aim.direction = (purr_float2){5.0f, NAN};
    in.aim.fire = true;
    memset(in.aim.purr_pad0, 0xAB, sizeof in.aim.purr_pad0); // Whatever arrived over the network
    in.speed = -3.0f;
    purr_world_set_server_input(&world, in);

    const PlayerInput *stored = &world.inputs[PURR_SERVER_INPUT];
    PURR_CHECK(stored->aim.direction.x == 1.0f); // [Clamp] inside the struct
    PURR_CHECK(stored->aim.direction.y == 0.0f); // NaN back to the default
    PURR_CHECK(stored->speed == 0.0f);
    PURR_CHECK(all_zero(stored->aim.purr_pad0, sizeof stored->aim.purr_pad0));

    purr_world_tick(&world);
    purr_world_tick(&world); // Still held
    PURR_CHECK(world.Log.fired == 2);
    PURR_CHECK(world.Log.fire_downs == 1); // Down on the first tick only
    PURR_CHECK(world.Log.aim.x == 1.0f && world.Log.speed == 0.0f);
}
