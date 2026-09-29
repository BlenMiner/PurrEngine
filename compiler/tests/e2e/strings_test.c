#include <stdio.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(strings_all_checks_pass)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    const Results *r = purr_get_Results(&world, (purr_entity){1, 1});
    PURR_REQUIRE(r != NULL);
    if (r->firstFailure) printf("    check %d failed\n", (int)r->firstFailure);
    PURR_CHECK(r->checks == 28);
    PURR_CHECK(r->passed == r->checks);
    PURR_CHECK(purr_scratch_mark() == 0); // Every run cleared what it made
}
