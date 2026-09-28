#include <math.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(input_bounds_apply_before_sanitize)
{
    purr_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.move = (purr_float2){5.0f, -0.5f};
    in.aim = (purr_float2){-3.0f, 9.0f};
    in.throttle = -2.0f;
    in.count = 50;
    in.gear = 7;
    purr_world_set_server_input(&world, in);
    purr_world_tick(&world);

    PURR_CHECK(world.Seen.move.x == 1.0f && world.Seen.move.y == -0.5f);
    PURR_CHECK(world.Seen.aim.x == 0.0f && world.Seen.aim.y == 5.0f);
    PURR_CHECK(world.Seen.throttle == 0.0f);
    PURR_CHECK(world.Seen.count == 20); // Clamped to 10, then doubled by Sanitize
    PURR_CHECK(world.Seen.gear == 3);
}

PURR_TEST(input_bounds_apply_to_the_defaults)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world); // No input set: the defaults
    PURR_CHECK(world.Seen.lives == 1);
    PURR_CHECK(world.Seen.gear == 1);
}

PURR_TEST(input_bounds_apply_after_nan_repair)
{
    purr_world_init(&world, 1.0f);
    PlayerInput in = {0};
    in.throttle = NAN; // Back to its default, 0
    in.gear = -4;
    purr_world_set_server_input(&world, in);
    purr_world_tick(&world);
    PURR_CHECK(world.Seen.throttle == 0.0f);
    PURR_CHECK(world.Seen.gear == 1);
}
