#include <stdio.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(show_values_all_checks_pass)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const Results *r = tide_get_Results(&world, (tide_entity){1, 1});
    TIDE_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    TIDE_CHECK(r->checks == 15);
    TIDE_CHECK(r->passed == r->checks);
    TIDE_CHECK(tide_scratch_mark() == 0); // Every run cleared what it made
}
