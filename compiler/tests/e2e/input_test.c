#include <math.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

// The Main scene is slot 0, and its Setup spawns these in order, so they get slots 1 to 4.
static const tide_entity FIRST = {1, 1};  // Owned by player 0
static const tide_entity SECOND = {2, 1}; // Owned by player 1
static const tide_entity NOBODY = {3, 1}; // Owned by no player
static const tide_entity UNOWNED = {4, 1}; // No Owner

static PlayerInput moving_right(const bool jump)
{
    tide_devices d = {0};
    tide_button_set(&d.keyboard.d, true);
    tide_button_set(&d.keyboard.space, jump);
    return tide_input_sample(&d, NULL);
}

TIDE_TEST(input_sample_reads_devices)
{
    tide_devices d = {0};
    tide_button_set(&d.keyboard.d, true);
    tide_button_set(&d.keyboard.w, true);
    d.gamepad.leftStick = tide_f2(0.5f, 0.0f);
    tide_button_set(&d.gamepad.buttonSouth, true);

    const PlayerInput in = tide_input_sample(&d, NULL);
    TIDE_CHECK(in.move.x == 1.5f && in.move.y == 1.0f);
    TIDE_CHECK(in.jump);
    TIDE_CHECK(in.speed == 1.0f); // Default, since the mouse wasn't pressed
}

TIDE_TEST(input_sample_starts_from_defaults_each_time)
{
    tide_devices d = {0};
    tide_button_set(&d.mouse.left, true);
    TIDE_CHECK(tide_input_sample(&d, NULL).speed == 2.0f);

    tide_devices_consume(&d); // Still held, but no longer "pressed"
    TIDE_CHECK(tide_input_sample(&d, NULL).speed == 1.0f);
}

TIDE_TEST(input_moves_the_owners_entity)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_input(&world, tide_player_from_index(0), moving_right(false));
    tide_world_tick(&world);

    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 1.0f);
    TIDE_CHECK(tide_get_Transform(&world, SECOND).position.x == 0.0f);
    TIDE_CHECK(tide_get_Transform(&world, NOBODY).position.x == 0.0f);

    // Player 0's input isn't set again: it repeats.
    tide_world_tick(&world);
    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 2.0f);
}

TIDE_TEST(input_pressed_and_released_compare_with_last_tick)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_input(&world, tide_player_from_index(0), moving_right(true));
    tide_world_tick(&world); // jump goes down: pressed
    tide_world_tick(&world); // still down: not pressed again
    TIDE_CHECK(tide_get_Jumps(&world, FIRST).count == 1);
    TIDE_CHECK(tide_get_Jumps(&world, FIRST).landings == 0);

    tide_world_set_input(&world, tide_player_from_index(0), moving_right(false));
    tide_world_tick(&world); // jump goes up: released
    TIDE_CHECK(tide_get_Jumps(&world, FIRST).count == 1);
    TIDE_CHECK(tide_get_Jumps(&world, FIRST).landings == 1);
}

TIDE_TEST(input_changing_owner_changes_whose_input_applies)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_input(&world, tide_player_from_index(0), moving_right(false));
    world.Swap.now = true;
    tide_world_tick(&world); // Move runs before the swap: player 0 still has FIRST
    world.Swap.now = false;
    tide_world_tick(&world); // Now player 0 owns SECOND

    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 1.0f);
    TIDE_CHECK(tide_get_Transform(&world, SECOND).position.x == 1.0f);
    TIDE_CHECK(tide_player_index(tide_get_Owner(&world, SECOND).player) == 0);
}

TIDE_TEST(input_ignores_players_out_of_range)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_input(&world, tide_player_from_index((int32_t)TIDE_MAX_PLAYERS), moving_right(false));
    tide_world_tick(&world);
    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 0.0f);
}

TIDE_TEST(input_unowned_entities_and_once_per_tick_systems_read_the_server)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_server_input(&world, moving_right(false));
    tide_world_tick(&world);

    TIDE_CHECK(tide_get_Transform(&world, NOBODY).position.x == 1.0f);
    TIDE_CHECK(tide_get_Transform(&world, UNOWNED).position.x == 1.0f);
    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 0.0f); // Player 0's input isn't set
    TIDE_CHECK(world.ServerView.moveX == 1.0f);
}

TIDE_TEST(input_sanitize_runs_before_the_simulation_sees_input)
{
    tide_world_init(&world, 1.0f);
    PlayerInput cheating = moving_right(false);
    cheating.move.x = 50.0f;
    cheating.speed = 10.0f;
    tide_world_set_input(&world, tide_player_from_index(0), cheating);
    tide_world_set_server_input(&world, cheating);
    tide_world_tick(&world);

    TIDE_CHECK(tide_get_Transform(&world, FIRST).position.x == 2.0f);   // move 1, speed 2
    TIDE_CHECK(tide_get_Transform(&world, UNOWNED).position.x == 2.0f); // The server's input too
    TIDE_CHECK(world.ServerView.moveX == 1.0f);
}

TIDE_TEST(input_nan_and_infinity_become_the_defaults)
{
    tide_world_init(&world, 1.0f);
    PlayerInput attack = moving_right(false); // move.x = 1
    attack.move.y = NAN;
    attack.speed = INFINITY;
    tide_world_set_input(&world, tide_player_from_index(0), attack);
    tide_world_tick(&world);

    const Transform trs = tide_get_Transform(&world, FIRST);
    TIDE_CHECK(trs.position.x == 1.0f); // speed is back to its default, 1
    TIDE_CHECK(trs.position.z == 0.0f); // move.y is back to 0, not NaN
}
