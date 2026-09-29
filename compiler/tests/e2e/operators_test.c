#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(operators_do_arithmetic)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Wallet.total.cents == 750);
    PURR_CHECK(world.Wallet.negated.cents == -250);
    PURR_CHECK(world.Wallet.stretched == 6.0f);
}

PURR_TEST(operators_compare)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Wallet.equal);
    PURR_CHECK(world.Wallet.unequal);
    PURR_CHECK(world.Wallet.ordered);
}
