#include <math.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

// The Main scene is slot 0, and its Setup spawns these in order, so they get slots 1 to 4.
static const purr_entity FIRST = {1, 1};  // Owned by player 0
static const purr_entity SECOND = {2, 1}; // Owned by player 1
static const purr_entity NOBODY = {3, 1}; // Owned by no player
static const purr_entity UNOWNED = {4, 1}; // No Owner

static PlayerInput moving_right(const bool jump)
{
    purr_devices d = {0};
    purr_button_set(&d.keyboard.d, true);
    purr_button_set(&d.keyboard.space, jump);
    return purr_input_sample(&d);
}

PURR_TEST(input_sample_reads_devices)
{
    purr_devices d = {0};
    purr_button_set(&d.keyboard.d, true);
    purr_button_set(&d.keyboard.w, true);
    d.gamepad.leftStick = purr_f2(0.5f, 0.0f);
    purr_button_set(&d.gamepad.buttonSouth, true);

    const PlayerInput in = purr_input_sample(&d);
    PURR_CHECK(in.move.x == 1.5f && in.move.y == 1.0f);
    PURR_CHECK(in.jump);
    PURR_CHECK(in.speed == 1.0f); // Default, since the mouse wasn't pressed
}

PURR_TEST(input_sample_starts_from_defaults_each_time)
{
    purr_devices d = {0};
    purr_button_set(&d.mouse.left, true);
    PURR_CHECK(purr_input_sample(&d).speed == 2.0f);

    purr_devices_consume(&d); // Still held, but no longer "pressed"
    PURR_CHECK(purr_input_sample(&d).speed == 1.0f);
}

PURR_TEST(input_moves_the_owners_entity)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_input(&world, purr_player_from_index(0), moving_right(false));
    purr_world_tick(&world);

    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 1.0f);
    PURR_CHECK(purr_get_Transform(&world, SECOND)->position.x == 0.0f);
    PURR_CHECK(purr_get_Transform(&world, NOBODY)->position.x == 0.0f);

    // Player 0's input isn't set again: it repeats.
    purr_world_tick(&world);
    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 2.0f);
}

PURR_TEST(input_pressed_and_released_compare_with_last_tick)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_input(&world, purr_player_from_index(0), moving_right(true));
    purr_world_tick(&world); // jump goes down: pressed
    purr_world_tick(&world); // still down: not pressed again
    PURR_CHECK(purr_get_Jumps(&world, FIRST)->count == 1);
    PURR_CHECK(purr_get_Jumps(&world, FIRST)->landings == 0);

    purr_world_set_input(&world, purr_player_from_index(0), moving_right(false));
    purr_world_tick(&world); // jump goes up: released
    PURR_CHECK(purr_get_Jumps(&world, FIRST)->count == 1);
    PURR_CHECK(purr_get_Jumps(&world, FIRST)->landings == 1);
}

PURR_TEST(input_changing_owner_changes_whose_input_applies)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_input(&world, purr_player_from_index(0), moving_right(false));
    world.Swap.now = true;
    purr_world_tick(&world); // Move runs before the swap: player 0 still has FIRST
    world.Swap.now = false;
    purr_world_tick(&world); // Now player 0 owns SECOND

    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 1.0f);
    PURR_CHECK(purr_get_Transform(&world, SECOND)->position.x == 1.0f);
    PURR_CHECK(purr_player_index(purr_get_Owner(&world, SECOND)->player) == 0);
}

PURR_TEST(input_ignores_players_out_of_range)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_input(&world, purr_player_from_index((int32_t)PURR_MAX_PLAYERS), moving_right(false));
    purr_world_tick(&world);
    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 0.0f);
}

PURR_TEST(input_unowned_entities_and_once_per_tick_systems_read_the_server)
{
    purr_world_init(&world, 1.0f);
    purr_world_set_server_input(&world, moving_right(false));
    purr_world_tick(&world);

    PURR_CHECK(purr_get_Transform(&world, NOBODY)->position.x == 1.0f);
    PURR_CHECK(purr_get_Transform(&world, UNOWNED)->position.x == 1.0f);
    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 0.0f); // Player 0's input isn't set
    PURR_CHECK(world.ServerView.moveX == 1.0f);
}

PURR_TEST(input_sanitize_runs_before_the_simulation_sees_input)
{
    purr_world_init(&world, 1.0f);
    PlayerInput cheating = moving_right(false);
    cheating.move.x = 50.0f;
    cheating.speed = 10.0f;
    purr_world_set_input(&world, purr_player_from_index(0), cheating);
    purr_world_set_server_input(&world, cheating);
    purr_world_tick(&world);

    PURR_CHECK(purr_get_Transform(&world, FIRST)->position.x == 2.0f);   // move 1, speed 2
    PURR_CHECK(purr_get_Transform(&world, UNOWNED)->position.x == 2.0f); // The server's input too
    PURR_CHECK(world.ServerView.moveX == 1.0f);
}

PURR_TEST(input_nan_and_infinity_become_the_defaults)
{
    purr_world_init(&world, 1.0f);
    PlayerInput attack = moving_right(false); // move.x = 1
    attack.move.y = NAN;
    attack.speed = INFINITY;
    purr_world_set_input(&world, purr_player_from_index(0), attack);
    purr_world_tick(&world);

    const Transform *trs = purr_get_Transform(&world, FIRST);
    PURR_CHECK(trs->position.x == 1.0f); // speed is back to its default, 1
    PURR_CHECK(trs->position.z == 0.0f); // move.y is back to 0, not NaN
}
