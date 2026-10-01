#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(operators_do_arithmetic)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Wallet.total.cents == 750);
    TIDE_CHECK(world.Wallet.negated.cents == -250);
    TIDE_CHECK(world.Wallet.stretched == 6.0f);
}

TIDE_TEST(operators_compare)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Wallet.equal);
    TIDE_CHECK(world.Wallet.unequal);
    TIDE_CHECK(world.Wallet.ordered);
}
