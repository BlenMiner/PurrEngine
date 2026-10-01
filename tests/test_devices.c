#include "tide/devices.h"
#include "tide/player.h"
#include "tide_test.h"

TIDE_TEST(devices_quick_tap_between_samples_is_not_lost)
{
    tide_devices d = {0};
    tide_button_set(&d.keyboard.space, true);  // frame 1: down
    tide_button_set(&d.keyboard.space, false); // frame 2: up again, before any sample
    TIDE_CHECK(d.keyboard.space.pressed);
    TIDE_CHECK(d.keyboard.space.down);
    TIDE_CHECK(d.keyboard.space.up);

    tide_devices_consume(&d);
    TIDE_CHECK(!d.keyboard.space.pressed);
    TIDE_CHECK(!d.keyboard.space.down);
    TIDE_CHECK(!d.keyboard.space.up);
}

TIDE_TEST(devices_held_button_stays_pressed_but_goes_down_once)
{
    tide_devices d = {0};
    tide_button_set(&d.gamepad.buttonSouth, true);
    tide_devices_consume(&d);
    tide_button_set(&d.gamepad.buttonSouth, true);
    TIDE_CHECK(d.gamepad.buttonSouth.pressed);
    TIDE_CHECK(!d.gamepad.buttonSouth.down);
}

TIDE_TEST(devices_consume_resets_mouse_motion)
{
    tide_devices d = {0};
    d.mouse.position = tide_f2(10, 20);
    d.mouse.delta = tide_f2(3, 4);
    d.mouse.scroll = tide_f2(0, 1);
    tide_devices_consume(&d);
    TIDE_CHECK(d.mouse.position.x == 10.0f);
    TIDE_CHECK(d.mouse.delta.x == 0.0f && d.mouse.scroll.y == 0.0f);
}

TIDE_TEST(player_ids)
{
    const tide_player_id none = {0};
    TIDE_CHECK(tide_player_is_null(none));
    TIDE_CHECK(tide_player_index(none) == -1);
    TIDE_CHECK(tide_player_index(tide_player_from_index(0)) == 0);
    TIDE_CHECK(tide_player_index(tide_player_from_index(3)) == 3);
    TIDE_CHECK(tide_player_index(tide_player_from_index((int32_t)TIDE_MAX_PLAYERS)) == -1);
    TIDE_CHECK(tide_player_is_null(tide_player_from_index(-1)));
    TIDE_CHECK(tide_player_equal(tide_player_from_index(2), tide_player_from_index(2)));
}
