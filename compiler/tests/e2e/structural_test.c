#include "game.h"
#include "purr_test.h"

static purr_world world;

// Entity handles are allocated deterministically: Main spawns a, b and the
// spawner first, so they get slots 0, 1 and 2, each at generation 1.
static const purr_entity A = {0, 1};
static const purr_entity B = {1, 1};
static const purr_entity SPAWNER = {2, 1};

// Spawn archetypes come first, in source order: Health, Health + Poisoned,
// Spawner, Child.
#define CHILDREN world.arch3_Child

static void ticks(int n)
{
    for (int i = 0; i < n; i++) purr_world_tick(&world);
}

PURR_TEST(structural_main_applies_spawns_then_adds)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(purr_world_entity_count(&world) == 3);
    PURR_REQUIRE(purr_get_Health(&world, A) != NULL);
    PURR_CHECK(purr_get_Health(&world, A)->value == 3);
    PURR_REQUIRE(purr_get_Poisoned(&world, A) != NULL); // Add on a pending spawn.
    PURR_CHECK(purr_get_Poisoned(&world, A)->damage == 1);
    PURR_CHECK(purr_get_Health(&world, B)->value == 1);
    PURR_CHECK(purr_get_Spawner(&world, SPAWNER)->remaining == 2);
}

PURR_TEST(structural_changes_apply_at_end_of_tick)
{
    purr_world_init(&world, 1.0f);
    ticks(1);
    // b dropped to -4: it lost Poisoned and gained Dead, but Cleanup didn't see
    // Dead yet because the change was deferred.
    PURR_CHECK(purr_get_Health(&world, B)->value == -4);
    PURR_CHECK(purr_get_Poisoned(&world, B) == NULL);
    PURR_CHECK(purr_get_Dead(&world, B) != NULL);
    PURR_CHECK(purr_entity_alive(&world.entities, B));
    PURR_CHECK(purr_get_Health(&world, A)->value == 2);
    PURR_CHECK(purr_world_entity_count(&world) == 4); // a, b, spawner, one child
}

PURR_TEST(structural_destroy_frees_the_entity)
{
    purr_world_init(&world, 1.0f);
    ticks(2);
    PURR_CHECK(!purr_entity_alive(&world.entities, B));
    PURR_CHECK(purr_get_Health(&world, B) == NULL);
    PURR_CHECK(purr_world_entity_count(&world) == 4); // a, spawner, two children
}

PURR_TEST(structural_spawned_children_point_at_spawner)
{
    purr_world_init(&world, 1.0f);
    ticks(5);
    PURR_CHECK(purr_get_Spawner(&world, SPAWNER)->remaining == 0);
    PURR_REQUIRE(CHILDREN.count == 2);
    for (uint32_t i = 0; i < CHILDREN.count; i++) {
        PURR_CHECK(purr_entity_equal(CHILDREN.Child[i].parent, SPAWNER));
    }
}

PURR_TEST(structural_everything_poisoned_eventually_dies)
{
    purr_world_init(&world, 1.0f);
    ticks(4);
    // a hit zero on tick 3 and was destroyed on tick 4.
    PURR_CHECK(!purr_entity_alive(&world.entities, A));
    PURR_CHECK(purr_world_entity_count(&world) == 3); // spawner and two children
}
