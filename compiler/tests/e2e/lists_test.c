#include <stdio.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world copy;

TIDE_TEST(lists_all_checks_pass)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const Results *r = tide_get_Results(&world, (tide_entity){1, 1});
    TIDE_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    TIDE_CHECK(r->checks == 18);
    TIDE_CHECK(r->passed == r->checks);
    TIDE_CHECK(tide_scratch_mark() == 0);
    TIDE_CHECK(world.heap.failed == 0);
}

TIDE_TEST(lists_reuse_their_memory)
{
    tide_world_init(&world, 1.0f);
    for (int i = 0; i < 50; i++) tide_world_tick(&world);
    const uint32_t used = world.heap.used;
    for (int i = 0; i < 200; i++) tide_world_tick(&world);
    TIDE_CHECK(world.heap.used == used);
    tide_world_print(&world);
}

TIDE_TEST(lists_snapshot_and_repeat)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    memcpy(&copy, &world, sizeof world);
    for (int i = 0; i < 5; i++) {
        tide_world_tick(&world);
        tide_world_tick(&copy);
    }
    TIDE_CHECK(memcmp(&world, &copy, sizeof world) == 0);
}
