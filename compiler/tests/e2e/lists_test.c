#include <stdio.h>
#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_world copy;

PURR_TEST(lists_all_checks_pass)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    const Results *r = purr_get_Results(&world, (purr_entity){1, 1});
    PURR_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    PURR_CHECK(r->checks == 18);
    PURR_CHECK(r->passed == r->checks);
    PURR_CHECK(purr_scratch_mark() == 0);
    PURR_CHECK(world.heap.failed == 0);
}

PURR_TEST(lists_reuse_their_memory)
{
    purr_world_init(&world, 1.0f);
    for (int i = 0; i < 50; i++) purr_world_tick(&world);
    const uint32_t used = world.heap.used;
    for (int i = 0; i < 200; i++) purr_world_tick(&world);
    PURR_CHECK(world.heap.used == used);
    purr_world_print(&world);
}

PURR_TEST(lists_snapshot_and_repeat)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    memcpy(&copy, &world, sizeof world);
    for (int i = 0; i < 5; i++) {
        purr_world_tick(&world);
        purr_world_tick(&copy);
    }
    PURR_CHECK(memcmp(&world, &copy, sizeof world) == 0);
}
