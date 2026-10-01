#include <math.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(input_bounds_apply_before_sanitize)
{
    tide_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.move = (tide_float2){5.0f, -0.5f};
    in.aim = (tide_float2){-3.0f, 9.0f};
    in.throttle = -2.0f;
    in.count = 50;
    in.gear = 7;
    tide_world_set_server_input(&world, in);
    tide_world_tick(&world);

    TIDE_CHECK(world.Seen.move.x == 1.0f && world.Seen.move.y == -0.5f);
    TIDE_CHECK(world.Seen.aim.x == 0.0f && world.Seen.aim.y == 5.0f);
    TIDE_CHECK(world.Seen.throttle == 0.0f);
    TIDE_CHECK(world.Seen.count == 20); // Clamped to 10, then doubled by Sanitize
    TIDE_CHECK(world.Seen.gear == 3);
}

TIDE_TEST(input_bounds_apply_to_the_defaults)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world); // No input set: the defaults
    TIDE_CHECK(world.Seen.lives == 1);
    TIDE_CHECK(world.Seen.gear == 1);
}

TIDE_TEST(input_bounds_apply_after_nan_repair)
{
    tide_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.throttle = NAN; // Back to its default, 0
    in.gear = -4;
    tide_world_set_server_input(&world, in);
    tide_world_tick(&world);
    TIDE_CHECK(world.Seen.throttle == 0.0f);
    TIDE_CHECK(world.Seen.gear == 1);
}
