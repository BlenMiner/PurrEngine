#include <math.h>
#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

// The Main scene is slot 0, and its Setup spawns these in order.
static const purr_entity FIRST = {1, 1};  // Owned by player 0
static const purr_entity SECOND = {2, 1}; // Owned by player 1
static const purr_entity SERVER = {3, 1}; // No owner

PURR_TEST(devices_input_sends_what_systems_read)
{
    purr_devices d = {0};
    purr_button_set(&d.keyboard.space, true);
    purr_button_set(&d.keyboard.a, true); // Nothing reads it
    d.gamepad.leftStick = purr_f2(0.5f, -0.25f);
    d.gamepad.rightStick = purr_f2(1.0f, 1.0f); // Nothing reads it
    d.gamepad.connected = true;
    d.mouse.position = purr_f2(100.0f, 200.0f);

    const purr_input in = purr_input_sample(&d, NULL);
    PURR_CHECK(in.purr_dev.keyboard.space.pressed);
    PURR_CHECK(!in.purr_dev.keyboard.space.down); // The tick works it out
    PURR_CHECK(!in.purr_dev.keyboard.space.held); // The platform's own
    PURR_CHECK(in.purr_dev.gamepad.leftStick.x == 0.5f && in.purr_dev.gamepad.leftStick.y == -0.25f);
    PURR_CHECK(in.purr_dev.gamepad.connected);
    PURR_CHECK(!in.purr_dev.keyboard.a.pressed);
    PURR_CHECK(in.purr_dev.gamepad.rightStick.x == 0.0f);
    PURR_CHECK(in.purr_dev.mouse.position.x == 0.0f);

    // The input is the devices: no padding the compiler adds.
    PURR_CHECK(sizeof(purr_input) == sizeof(purr_devices));
}

PURR_TEST(devices_are_repaired)
{
    purr_devices d = {0};
    d.gamepad.leftStick = purr_f2(5.0f, NAN);
    d.gamepad.rightTrigger = -3.0f;
    purr_input in = purr_input_sample(&d, NULL);
    PURR_CHECK(in.purr_dev.gamepad.leftStick.x == 1.0f && in.purr_dev.gamepad.leftStick.y == 0.0f);
    PURR_CHECK(in.purr_dev.gamepad.rightTrigger == 0.0f);

    // From another machine: anything could be in it.
    in.purr_dev.keyboard.a.pressed = true;
    in.purr_dev.keyboard.space.held = true;
    in.purr_dev.mouse.position = purr_f2(3.0f, 4.0f);
    in.purr_dev.text.count = 5;
    in.purr_dev.gamepad.leftStick = purr_f2(INFINITY, -2.0f);
    in.purr_dev.gamepad.rightTrigger = NAN;
    purr_world_init(&world, 1.0f);
    purr_world_set_input(&world, purr_player_from_index(0), in);
    const purr_devices *got = &world.inputs[0].purr_dev;
    PURR_CHECK(got->gamepad.leftStick.x == 1.0f && got->gamepad.leftStick.y == -1.0f);
    PURR_CHECK(got->gamepad.rightTrigger == 0.0f);
    PURR_CHECK(!got->keyboard.a.pressed && !got->keyboard.space.held);
    PURR_CHECK(got->mouse.position.x == 0.0f && got->text.count == 0);
}

PURR_TEST(devices_are_the_owners)
{
    purr_world_init(&world, 1.0f);
    purr_devices first = {0};
    first.gamepad.leftStick = purr_f2(1.0f, 0.0f);
    first.gamepad.connected = true;
    purr_button_set(&first.keyboard.space, true);
    purr_world_set_input(&world, purr_player_from_index(0), purr_input_sample(&first, NULL));
    purr_devices server = {0};
    server.gamepad.rightTrigger = 0.5f;
    purr_world_set_server_input(&world, purr_input_sample(&server, NULL));
    purr_world_tick(&world);

    const Walker *a = purr_get_Walker(&world, FIRST);
    const Walker *b = purr_get_Walker(&world, SECOND);
    const Walker *s = purr_get_Walker(&world, SERVER);
    PURR_REQUIRE(a && b && s);
    PURR_CHECK(a->position.x == 1.0f && a->jumps == 1 && a->connected && a->throttle == 0.0f);
    PURR_CHECK(b->position.x == 0.0f && b->jumps == 0 && !b->connected);
    PURR_CHECK(s->throttle == 0.5f && s->jumps == 0);
}

// A button went down against last tick's input, so an input that repeats,
// as a guess for a missing one does, doesn't press it again.
PURR_TEST(devices_buttons_go_down_once)
{
    purr_world_init(&world, 1.0f);
    purr_devices d = {0};
    purr_button_set(&d.keyboard.space, true);
    purr_world_set_input(&world, purr_player_from_index(0), purr_input_sample(&d, NULL));
    purr_world_tick(&world);
    purr_world_tick(&world); // Not set again: it repeats
    PURR_CHECK(purr_get_Walker(&world, FIRST)->jumps == 1);

    // As a host does: sample, then start the next window.
    purr_devices_consume(&d);
    purr_button_set(&d.keyboard.space, false);
    purr_devices_consume(&d);
    purr_world_set_input(&world, purr_player_from_index(0), purr_input_sample(&d, NULL));
    purr_world_tick(&world);
    purr_button_set(&d.keyboard.space, true);
    purr_world_set_input(&world, purr_player_from_index(0), purr_input_sample(&d, NULL));
    purr_world_tick(&world);
    PURR_CHECK(purr_get_Walker(&world, FIRST)->jumps == 2);
}
