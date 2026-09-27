#include "purr/devices.h"
#include "purr/player.h"
#include "purr_test.h"

PURR_TEST(devices_quick_tap_between_samples_is_not_lost)
{
    purr_devices d = {0};
    purr_button_set(&d.keyboard.space, true);  // frame 1: down
    purr_button_set(&d.keyboard.space, false); // frame 2: up again, before any sample
    PURR_CHECK(d.keyboard.space.down);
    PURR_CHECK(d.keyboard.space.pressed);
    PURR_CHECK(d.keyboard.space.released);

    purr_devices_consume(&d);
    PURR_CHECK(!d.keyboard.space.down);
    PURR_CHECK(!d.keyboard.space.pressed);
    PURR_CHECK(!d.keyboard.space.released);
}

PURR_TEST(devices_held_button_stays_down_without_repeating_press)
{
    purr_devices d = {0};
    purr_button_set(&d.gamepad.buttonSouth, true);
    purr_devices_consume(&d);
    purr_button_set(&d.gamepad.buttonSouth, true);
    PURR_CHECK(d.gamepad.buttonSouth.down);
    PURR_CHECK(!d.gamepad.buttonSouth.pressed);
}

PURR_TEST(devices_consume_resets_mouse_motion)
{
    purr_devices d = {0};
    d.mouse.position = purr_f2(10, 20);
    d.mouse.delta = purr_f2(3, 4);
    d.mouse.scroll = purr_f2(0, 1);
    purr_devices_consume(&d);
    PURR_CHECK(d.mouse.position.x == 10.0f);
    PURR_CHECK(d.mouse.delta.x == 0.0f && d.mouse.scroll.y == 0.0f);
}

PURR_TEST(player_ids)
{
    const purr_player_id none = {0};
    PURR_CHECK(purr_player_is_null(none));
    PURR_CHECK(purr_player_index(none) == -1);
    PURR_CHECK(purr_player_index(purr_player_from_index(0)) == 0);
    PURR_CHECK(purr_player_index(purr_player_from_index(3)) == 3);
    PURR_CHECK(purr_player_index(purr_player_from_index((int32_t)PURR_MAX_PLAYERS)) == -1);
    PURR_CHECK(purr_player_is_null(purr_player_from_index(-1)));
    PURR_CHECK(purr_player_equal(purr_player_from_index(2), purr_player_from_index(2)));
}
