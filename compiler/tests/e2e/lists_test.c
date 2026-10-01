#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world copy;

// Whether two worlds hold the same: the same bytes (tide_world_pack, which
// leaves out where their pages are), and the same hash.
static bool same(const tide_world *a, const tide_world *b)
{
    const uint32_t size = tide_world_pack(a, NULL, 0);
    if (tide_world_pack(b, NULL, 0) != size) return false;
    uint8_t *x = malloc(size);
    uint8_t *y = malloc(size);
    tide_world_pack(a, x, size);
    tide_world_pack(b, y, size);
    const bool equal = memcmp(x, y, size) == 0;
    free(x);
    free(y);
    return equal && tide_world_hash(a) == tide_world_hash(b);
}

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
    tide_world_copy(&copy, &world);
    for (int i = 0; i < 5; i++) {
        tide_world_tick(&world);
        tide_world_tick(&copy);
    }
    TIDE_CHECK(same(&world, &copy));
}
