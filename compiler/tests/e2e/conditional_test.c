#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(conditional_in_defaults)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(world.Defaults.picked == 10);
    PURR_CHECK(world.Defaults.widened == 1.0f);
}

PURR_TEST(conditional_picks_a_side)
{
    purr_world_init(&world, 1.0f);

    purr_world_tick(&world); // ticks = 1
    PURR_CHECK(world.Results.sign == -1);
    PURR_CHECK(world.Results.half == 1.0f);
    PURR_CHECK(world.Results.flag);
    PURR_CHECK(world.Results.precedence == 1);
    PURR_CHECK(world.Results.position.x == 0.0f && world.Results.position.z == 0.0f);

    purr_world_tick(&world); // ticks = 2
    PURR_CHECK(world.Results.sign == 0);
    PURR_CHECK(world.Results.half == 0.5f);
    PURR_CHECK(!world.Results.flag);
    PURR_CHECK(world.Results.position.x == 1.0f && world.Results.position.y == 2.0f);
    PURR_CHECK(world.Results.position.z == 3.0f);

    purr_world_tick(&world); // ticks = 3
    PURR_CHECK(world.Results.sign == 1);
}

PURR_TEST(conditional_in_the_input_constructor)
{
    purr_devices d = {0};
    purr_button_set(&d.keyboard.d, true);
    PURR_CHECK(purr_input_sample(&d).horizontal == 1.0f);
    purr_button_set(&d.keyboard.a, true);
    PURR_CHECK(purr_input_sample(&d).horizontal == 0.0f);
}
