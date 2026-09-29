#include "game.h"
#include "purr_test.h"

static purr_world world;

// The Main scene is slot 0, and its Setup spawns these first, so they get slots 1, 2 and 3.
static const purr_entity BARE = {1, 1};
static const purr_entity PARTIAL = {2, 1};
static const purr_entity BITS = {3, 1};

PURR_TEST(defaults_singletons_start_with_declared_values)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Physics.collisionCount == 69);
    PURR_CHECK(world.Physics.gravity.y == -9.81f);
    PURR_CHECK(world.Physics.flags == 9);
    PURR_CHECK(world.Physics.mask == 15);
}

PURR_TEST(defaults_main_can_override_singleton_defaults)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Overridden.value == 6);
}

PURR_TEST(defaults_bare_spawn_uses_defaults)
{
    purr_world_init(&world, 1.0f);
    Health *h = purr_get_Health(&world, BARE);
    PURR_REQUIRE(h != NULL);
    PURR_CHECK(h->value == 100);
    PURR_CHECK(h->max == 100);
    PURR_CHECK(purr_entity_is_null(h->attacker));
}

PURR_TEST(defaults_unset_fields_use_defaults_not_zero)
{
    purr_world_init(&world, 1.0f);
    Health *h = purr_get_Health(&world, PARTIAL);
    PURR_REQUIRE(h != NULL);
    PURR_CHECK(h->value == 100);
    PURR_CHECK(h->max == 200);
}

PURR_TEST(defaults_bare_add_uses_defaults)
{
    purr_world_init(&world, 1.0f);
    Marker *m = purr_get_Marker(&world, BARE);
    PURR_REQUIRE(m != NULL);
    PURR_CHECK(m->id == -7);
}

PURR_TEST(defaults_hex_and_binary_literals)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Literals.hex == 255);
    PURR_CHECK(world.Literals.upperHex == 171);
    PURR_CHECK(world.Literals.binary == 10);
    PURR_CHECK(world.Literals.allBits == -1);
    PURR_CHECK(world.Literals.lowest == INT32_MIN);
    PURR_CHECK(world.Literals.mask == 240);
    PURR_CHECK(world.Literals.million == 1000000);
    PURR_CHECK(world.Literals.separatedMask == 240);
    PURR_CHECK(world.Literals.separatedHex == 0xFFFF);
    PURR_CHECK(world.Literals.separatedFloat == 1000.25f);
}

PURR_TEST(defaults_bitwise_operators)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    Bits *b = purr_get_Bits(&world, BITS);
    PURR_REQUIRE(b != NULL);
    PURR_CHECK(b->shifted == 16);
    PURR_CHECK(b->negativeShift == -4);
    PURR_CHECK(b->bigShift == 2);
    PURR_CHECK(b->mixed == 5);
    PURR_CHECK(b->masked == 48);
    PURR_CHECK(b->negatedLowest == INT32_MIN);
    PURR_CHECK(b->fromHex == 16.0f);
}
