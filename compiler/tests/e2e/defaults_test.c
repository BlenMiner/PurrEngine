#include "game.h"
#include "tide_test.h"

static tide_world world;

// The Main scene is slot 0, and its Setup spawns these first, so they get slots 1, 2 and 3.
static const tide_entity BARE = {1, 1};
static const tide_entity PARTIAL = {2, 1};
static const tide_entity BITS = {3, 1};

TIDE_TEST(defaults_singletons_start_with_declared_values)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Physics.collisionCount == 69);
    TIDE_CHECK(world.Physics.gravity.y == -9.81f);
    TIDE_CHECK(world.Physics.flags == 9);
    TIDE_CHECK(world.Physics.mask == 15);
}

TIDE_TEST(defaults_main_can_override_singleton_defaults)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Overridden.value == 6);
}

TIDE_TEST(defaults_bare_spawn_uses_defaults)
{
    tide_world_init(&world, 1.0f);
    Health *h = tide_get_Health(&world, BARE);
    TIDE_REQUIRE(h != NULL);
    TIDE_CHECK(h->value == 100);
    TIDE_CHECK(h->max == 100);
    TIDE_CHECK(tide_entity_is_null(h->attacker));
}

TIDE_TEST(defaults_unset_fields_use_defaults_not_zero)
{
    tide_world_init(&world, 1.0f);
    Health *h = tide_get_Health(&world, PARTIAL);
    TIDE_REQUIRE(h != NULL);
    TIDE_CHECK(h->value == 100);
    TIDE_CHECK(h->max == 200);
}

TIDE_TEST(defaults_bare_add_uses_defaults)
{
    tide_world_init(&world, 1.0f);
    Marker *m = tide_get_Marker(&world, BARE);
    TIDE_REQUIRE(m != NULL);
    TIDE_CHECK(m->id == -7);
}

TIDE_TEST(defaults_hex_and_binary_literals)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Literals.hex == 255);
    TIDE_CHECK(world.Literals.upperHex == 171);
    TIDE_CHECK(world.Literals.binary == 10);
    TIDE_CHECK(world.Literals.allBits == -1);
    TIDE_CHECK(world.Literals.lowest == INT32_MIN);
    TIDE_CHECK(world.Literals.mask == 240);
    TIDE_CHECK(world.Literals.million == 1000000);
    TIDE_CHECK(world.Literals.separatedMask == 240);
    TIDE_CHECK(world.Literals.separatedHex == 0xFFFF);
    TIDE_CHECK(world.Literals.separatedFloat == 1000.25f);
}

TIDE_TEST(defaults_bitwise_operators)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    Bits *b = tide_get_Bits(&world, BITS);
    TIDE_REQUIRE(b != NULL);
    TIDE_CHECK(b->shifted == 16);
    TIDE_CHECK(b->negativeShift == -4);
    TIDE_CHECK(b->bigShift == 2);
    TIDE_CHECK(b->mixed == 5);
    TIDE_CHECK(b->masked == 48);
    TIDE_CHECK(b->negatedLowest == INT32_MIN);
    TIDE_CHECK(b->fromHex == 16.0f);
}
