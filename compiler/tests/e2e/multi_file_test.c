#include "game.h"
#include "purr_test.h"

static purr_world world;

// Default order: Physics.Move, Physics.Gravity, Combat.Damage, Items.Heal
// (files by path, then declaration order). [After(Gravity)] on Move and
// [Before(Physics.Move)] on Damage give Gravity, Damage, Move, Heal.
PURR_TEST(multi_file_order)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    PURR_CHECK(world.Trace.gravity == 0);
    PURR_CHECK(world.Trace.damage == 1);
    PURR_CHECK(world.Trace.move == 2);
    PURR_CHECK(world.Trace.heal == 3);
    PURR_CHECK(world.Trace.step == 4);
}

// Namespaced names in C: Combat.Health is Combat_Health.
PURR_TEST(multi_file_namespaces)
{
    purr_world_init(&world, 1.0f);
    const purr_entity e = {0, 1};
    PURR_REQUIRE(purr_get_Physics_Body(&world, e) != NULL);
    PURR_REQUIRE(purr_get_Combat_Health(&world, e) != NULL);
    PURR_REQUIRE(purr_get_Items_Health(&world, e) != NULL);
    PURR_CHECK(purr_get_Combat_Health(&world, e)->value == 10);
    PURR_CHECK(purr_get_Items_Health(&world, e)->value == 3);
    purr_world_tick(&world);
    PURR_CHECK(purr_get_Combat_Health(&world, e)->value == 10); // Heal changes Items.Health only
    PURR_CHECK(purr_get_Items_Health(&world, e)->value == 4);
}
