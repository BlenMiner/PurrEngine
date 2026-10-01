#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(conditional_in_defaults)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Defaults.picked == 10);
    TIDE_CHECK(world.Defaults.widened == 1.0f);
}

TIDE_TEST(conditional_picks_a_side)
{
    tide_world_init(&world, 1.0f);

    tide_world_tick(&world); // ticks = 1
    TIDE_CHECK(world.Results.sign == -1);
    TIDE_CHECK(world.Results.half == 1.0f);
    TIDE_CHECK(world.Results.flag);
    TIDE_CHECK(world.Results.precedence == 1);
    TIDE_CHECK(world.Results.position.x == 0.0f && world.Results.position.z == 0.0f);

    tide_world_tick(&world); // ticks = 2
    TIDE_CHECK(world.Results.sign == 0);
    TIDE_CHECK(world.Results.half == 0.5f);
    TIDE_CHECK(!world.Results.flag);
    TIDE_CHECK(world.Results.position.x == 1.0f && world.Results.position.y == 2.0f);
    TIDE_CHECK(world.Results.position.z == 3.0f);

    tide_world_tick(&world); // ticks = 3
    TIDE_CHECK(world.Results.sign == 1);
}

TIDE_TEST(conditional_in_the_input_constructor)
{
    tide_devices d = {0};
    tide_button_set(&d.keyboard.d, true);
    TIDE_CHECK(tide_input_sample(&d, NULL).horizontal == 1.0f);
    tide_button_set(&d.keyboard.a, true);
    TIDE_CHECK(tide_input_sample(&d, NULL).horizontal == 0.0f);
}
