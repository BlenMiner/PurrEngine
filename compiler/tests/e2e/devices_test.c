#include <math.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

// The Main scene is slot 0, and its Setup spawns these in order.
static const tide_entity FIRST = {1, 1};  // Owned by player 0
static const tide_entity SECOND = {2, 1}; // Owned by player 1
static const tide_entity SERVER = {3, 1}; // No owner

TIDE_TEST(devices_input_sends_what_systems_read)
{
    tide_devices d = {0};
    tide_button_set(&d.keyboard.space, true);
    tide_button_set(&d.keyboard.a, true); // Nothing reads it
    d.gamepad.leftStick = tide_f2(0.5f, -0.25f);
    d.gamepad.rightStick = tide_f2(1.0f, 1.0f); // Nothing reads it
    d.gamepad.connected = true;
    d.mouse.position = tide_f2(100.0f, 200.0f);

    const tide_input in = tide_input_sample(&d, NULL);
    TIDE_CHECK(in.tide_dev.keyboard.space.pressed);
    TIDE_CHECK(!in.tide_dev.keyboard.space.down); // The tick works it out
    TIDE_CHECK(!in.tide_dev.keyboard.space.held); // The platform's own
    TIDE_CHECK(in.tide_dev.gamepad.leftStick.x == 0.5f && in.tide_dev.gamepad.leftStick.y == -0.25f);
    TIDE_CHECK(in.tide_dev.gamepad.connected);
    TIDE_CHECK(!in.tide_dev.keyboard.a.pressed);
    TIDE_CHECK(in.tide_dev.gamepad.rightStick.x == 0.0f);
    TIDE_CHECK(in.tide_dev.mouse.position.x == 0.0f);

    // The input is the devices: no padding the compiler adds.
    TIDE_CHECK(sizeof(tide_input) == sizeof(tide_devices));
}

TIDE_TEST(devices_are_repaired)
{
    tide_devices d = {0};
    d.gamepad.leftStick = tide_f2(5.0f, NAN);
    d.gamepad.rightTrigger = -3.0f;
    tide_input in = tide_input_sample(&d, NULL);
    TIDE_CHECK(in.tide_dev.gamepad.leftStick.x == 1.0f && in.tide_dev.gamepad.leftStick.y == 0.0f);
    TIDE_CHECK(in.tide_dev.gamepad.rightTrigger == 0.0f);

    // From another machine: anything could be in it.
    in.tide_dev.keyboard.a.pressed = true;
    in.tide_dev.keyboard.space.held = true;
    in.tide_dev.mouse.position = tide_f2(3.0f, 4.0f);
    in.tide_dev.text.count = 5;
    in.tide_dev.gamepad.leftStick = tide_f2(INFINITY, -2.0f);
    in.tide_dev.gamepad.rightTrigger = NAN;
    tide_world_init(&world, 1.0f);
    tide_world_set_input(&world, tide_player_from_index(0), in);
    const tide_devices *got = &world.inputs[0].tide_dev;
    TIDE_CHECK(got->gamepad.leftStick.x == 1.0f && got->gamepad.leftStick.y == -1.0f);
    TIDE_CHECK(got->gamepad.rightTrigger == 0.0f);
    TIDE_CHECK(!got->keyboard.a.pressed && !got->keyboard.space.held);
    TIDE_CHECK(got->mouse.position.x == 0.0f && got->text.count == 0);
}

TIDE_TEST(devices_are_the_owners)
{
    tide_world_init(&world, 1.0f);
    tide_devices first = {0};
    first.gamepad.leftStick = tide_f2(1.0f, 0.0f);
    first.gamepad.connected = true;
    tide_button_set(&first.keyboard.space, true);
    tide_world_set_input(&world, tide_player_from_index(0), tide_input_sample(&first, NULL));
    tide_devices server = {0};
    server.gamepad.rightTrigger = 0.5f;
    tide_world_set_server_input(&world, tide_input_sample(&server, NULL));
    tide_world_tick(&world);

    const Walker *a = tide_get_Walker(&world, FIRST);
    const Walker *b = tide_get_Walker(&world, SECOND);
    const Walker *s = tide_get_Walker(&world, SERVER);
    TIDE_REQUIRE(a && b && s);
    TIDE_CHECK(a->position.x == 1.0f && a->jumps == 1 && a->connected && a->throttle == 0.0f);
    TIDE_CHECK(b->position.x == 0.0f && b->jumps == 0 && !b->connected);
    TIDE_CHECK(s->throttle == 0.5f && s->jumps == 0);
}

// A button went down against last tick's input, so an input that repeats,
// as a guess for a missing one does, doesn't press it again.
TIDE_TEST(devices_buttons_go_down_once)
{
    tide_world_init(&world, 1.0f);
    tide_devices d = {0};
    tide_button_set(&d.keyboard.space, true);
    tide_world_set_input(&world, tide_player_from_index(0), tide_input_sample(&d, NULL));
    tide_world_tick(&world);
    tide_world_tick(&world); // Not set again: it repeats
    TIDE_CHECK(tide_get_Walker(&world, FIRST)->jumps == 1);

    // As a host does: sample, then start the next window.
    tide_devices_consume(&d);
    tide_button_set(&d.keyboard.space, false);
    tide_devices_consume(&d);
    tide_world_set_input(&world, tide_player_from_index(0), tide_input_sample(&d, NULL));
    tide_world_tick(&world);
    tide_button_set(&d.keyboard.space, true);
    tide_world_set_input(&world, tide_player_from_index(0), tide_input_sample(&d, NULL));
    tide_world_tick(&world);
    TIDE_CHECK(tide_get_Walker(&world, FIRST)->jumps == 2);
}
