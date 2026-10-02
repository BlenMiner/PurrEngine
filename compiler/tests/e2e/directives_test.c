#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(directives_read_the_branches_for_this_tide)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Picked.a == 1);
    TIDE_CHECK(world.Picked.b == 2);
    TIDE_CHECK(world.Picked.c == 3);
    TIDE_CHECK(world.Picked.d == 5);
    tide_world_free(&world);
}
