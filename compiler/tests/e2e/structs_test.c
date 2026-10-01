#include <math.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static const tide_entity first = {1, 1};
static const tide_entity second = {2, 1};

static bool all_zero(const uint8_t *bytes, const size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

TIDE_TEST(struct_defaults_nest)
{
    tide_world_init(&world, 1.0f);
    const Unit *a = tide_get_Unit(&world, first);
    const Unit *b = tide_get_Unit(&world, second);
    TIDE_REQUIRE(a != NULL && b != NULL);
    TIDE_CHECK(a->stats.alive && a->stats.health == 100.0f);
    TIDE_CHECK(a->stats.damage.lo == 0.0f && a->stats.damage.hi == 1.0f); // Range's own default
    TIDE_CHECK(a->stats.armor.lo == 0.0f && a->stats.armor.hi == 5.0f);  // The field's default
    TIDE_CHECK(b->stats.health == 50.0f && b->stats.damage.lo == 2.0f && b->stats.damage.hi == 3.0f);
    TIDE_CHECK(b->stats.alive && b->stats.armor.hi == 5.0f); // Fields the literal leaves out keep their defaults
}

TIDE_TEST(struct_fields_write_through_mut)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const Unit *a = tide_get_Unit(&world, first);
    const Unit *b = tide_get_Unit(&world, second);
    TIDE_REQUIRE(a != NULL && b != NULL);
    TIDE_CHECK(a->stats.health == 99.0f && a->stats.alive);
    TIDE_CHECK(b->stats.health == 47.0f && !b->stats.alive);
}

TIDE_TEST(struct_values_are_copies)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    // Copy runs for both units, the second one last: its copy has health 0, and the unit keeps 47.
    TIDE_CHECK(world.Log.copy.health == 0.0f);
    TIDE_CHECK(world.Log.copy.damage.hi == 3.0f);
    TIDE_CHECK(world.Log.copy_health == 47.0f);
}

TIDE_TEST(struct_padding_is_explicit_and_zero)
{
    // tidec writes padding out as members, so the parts add up to the size.
    TIDE_CHECK(sizeof(Stats) == 24);
    TIDE_CHECK(sizeof(Aim) == 12);
    TIDE_CHECK(sizeof(PlayerInput) == 16);
    tide_world_init(&world, 1.0f);
    const Unit *a = tide_get_Unit(&world, first);
    TIDE_REQUIRE(a != NULL);
    TIDE_CHECK(all_zero(a->stats.tide_pad0, sizeof a->stats.tide_pad0));
}

TIDE_TEST(struct_input_fields_are_repaired)
{
    tide_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.aim.direction = (tide_float2){5.0f, NAN};
    in.aim.fire = true;
    memset(in.aim.tide_pad0, 0xAB, sizeof in.aim.tide_pad0); // Whatever arrived over the network
    in.speed = -3.0f;
    tide_world_set_server_input(&world, in);

    const PlayerInput *stored = &world.inputs[TIDE_SERVER_INPUT];
    TIDE_CHECK(stored->aim.direction.x == 1.0f); // [Clamp] inside the struct
    TIDE_CHECK(stored->aim.direction.y == 0.0f); // NaN back to the default
    TIDE_CHECK(stored->speed == 0.0f);
    TIDE_CHECK(all_zero(stored->aim.tide_pad0, sizeof stored->aim.tide_pad0));

    tide_world_tick(&world);
    tide_world_tick(&world); // Still held
    TIDE_CHECK(world.Log.fired == 2);
    TIDE_CHECK(world.Log.fire_downs == 1); // Down on the first tick only
    TIDE_CHECK(world.Log.aim.x == 1.0f && world.Log.speed == 0.0f);
}
