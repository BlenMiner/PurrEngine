#include <string.h>

#include "purr/gui.h"
#include "purr_test.h"

// The GUI runtime driven by made-up devices, on a 1920 x 1080 window: one GUI
// unit per pixel. The mouse is given from the top left, y down, like the GUI.

static purr_gui gui;
static purr_devices devices;
static purr_draw_list draw;

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
}

static void mouse(const float x, const float y, const bool held)
{
    devices.mouse.position = purr_f2(x, 1080.0f - y);
    purr_button_set(&devices.mouse.left, held);
}

static void key(purr_button *b, const bool held)
{
    purr_button_set(b, held);
}

static void begin(void)
{
    purr_gui_begin(&gui, &devices, purr_f2(1920.0f, 1080.0f), NULL);
}

static void end(void)
{
    purr_draw_reset(&draw);
    purr_gui_end(&gui, &draw);
    devices.text.count = 0; // Typed characters last one frame
}

static void type(const char *text)
{
    for (const char *c = text; *c; c++) devices.text.chars[devices.text.count++] = (uint32_t)*c;
}

// A frame with two layout buttons; returns which were pressed (bit 0 and 1).
static int two_buttons(void)
{
    begin();
    const bool a = purr_gui_layout_button(&gui, 10, "Play");
    const bool b = purr_gui_layout_button(&gui, 20, "Quit");
    end();
    return (a ? 1 : 0) | (b ? 2 : 0);
}

PURR_TEST(gui_button_clicks_on_release)
{
    start();
    // Play is at the top left, 48 tall; Quit below it after 8 of spacing.
    mouse(20.0f, 70.0f, false);
    PURR_CHECK(two_buttons() == 0);
    mouse(20.0f, 70.0f, true);
    PURR_CHECK(two_buttons() == 0); // Pressed, not released yet
    mouse(20.0f, 70.0f, false);
    PURR_CHECK(two_buttons() == 2);
    PURR_CHECK(two_buttons() == 0);

    // A press that leaves the button before it's released doesn't count.
    mouse(20.0f, 20.0f, true);
    PURR_CHECK(two_buttons() == 0);
    mouse(900.0f, 900.0f, true);
    PURR_CHECK(two_buttons() == 0);
    mouse(900.0f, 900.0f, false);
    PURR_CHECK(two_buttons() == 0);
}

PURR_TEST(gui_draws_over_the_world)
{
    start();
    begin();
    purr_gui_layout_label(&gui, "Hello");
    purr_gui_layout_button(&gui, 1, "Go");
    end();
    PURR_REQUIRE(draw.count >= 4);
    PURR_CHECK(draw.commands[0].kind == PURR_DRAW_GUI);
    PURR_CHECK(draw.commands[1].kind == PURR_DRAW_TEXT && strcmp(draw.text + draw.commands[1].text, "Hello") == 0);
    PURR_CHECK(draw.commands[2].kind == PURR_DRAW_RECT); // The button, below the label
    PURR_CHECK(draw.commands[2].a.y == 48.0f + 8.0f + 24.0f);

    begin(); // A frame without a GUI adds nothing
    end();
    PURR_CHECK(draw.count == 0);
}

PURR_TEST(gui_toggle_and_slider_change_values)
{
    start();
    bool on = false;
    float volume = 0.5f;
    for (int frame = 0; frame < 3; frame++) {
        mouse(10.0f, 24.0f, frame == 1);
        begin();
        const bool toggled = purr_gui_layout_toggle(&gui, 1, "Fullscreen", &on);
        purr_gui_layout_slider(&gui, 2, "", &volume, 0.0f, 1.0f);
        end();
        PURR_CHECK(toggled == (frame == 2));
    }
    PURR_CHECK(on);

    // The slider is below the toggle: its track goes from x 8 to 8 + 320 + 100 - 100 - 16.
    const float left = 8.0f;
    const float width = 320.0f - 16.0f;
    for (int frame = 0; frame < 3; frame++) {
        mouse(frame == 0 ? left + 10.0f : left + width * 0.25f, 80.0f, frame < 2);
        begin();
        purr_gui_layout_toggle(&gui, 1, "Fullscreen", &on);
        purr_gui_layout_slider(&gui, 2, "", &volume, 0.0f, 1.0f);
        end();
    }
    PURR_CHECK(volume == 0.25f);
}

PURR_TEST(gui_ids_tell_loops_apart)
{
    start();
    begin();
    const uint32_t a = purr_gui_id(&gui, 7, 3);
    const uint32_t b = purr_gui_id(&gui, 7, 3);
    const uint32_t c = purr_gui_id(&gui, 8, 3);
    end();
    PURR_CHECK(a != b && a != c && b != c);
    PURR_CHECK(a != 0 && b != 0 && c != 0);
    begin();
    PURR_CHECK(purr_gui_id(&gui, 7, 3) == a); // The same next frame
    PURR_CHECK(purr_gui_id(&gui, 7, 3) == b);
    end();
}

PURR_TEST(gui_keyboard_navigation)
{
    start();
    PURR_CHECK(two_buttons() == 0); // Lays them out, for the order
    key(&devices.keyboard.downArrow, true);
    PURR_CHECK(two_buttons() == 0);
    PURR_CHECK(gui.focus == 10); // The first one
    key(&devices.keyboard.downArrow, false);
    PURR_CHECK(two_buttons() == 0);
    key(&devices.keyboard.downArrow, true);
    PURR_CHECK(two_buttons() == 0);
    PURR_CHECK(gui.focus == 20);
    key(&devices.keyboard.downArrow, false);
    key(&devices.keyboard.enter, true);
    PURR_CHECK(two_buttons() == 2);
    key(&devices.keyboard.enter, false);

    // While a widget has the focus, the game doesn't see the keyboard.
    purr_devices sampled = devices;
    key(&sampled.keyboard.w, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(!sampled.keyboard.w.held && !sampled.keyboard.w.pressed);

    key(&devices.keyboard.escape, true);
    PURR_CHECK(two_buttons() == 0);
    PURR_CHECK(gui.focus == 0);
    key(&devices.keyboard.escape, false);
    sampled = devices;
    key(&sampled.keyboard.w, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(sampled.keyboard.w.held);
}

PURR_TEST(gui_arrows_wait_while_the_game_reads_them)
{
    start();
    PURR_CHECK(two_buttons() == 0);
    // The game sampled the devices: the arrows are its, not the GUI's.
    purr_devices sampled = devices;
    purr_gui_hide(&gui, &sampled);
    key(&devices.keyboard.downArrow, true);
    PURR_CHECK(two_buttons() == 0);
    PURR_CHECK(gui.focus == 0);
    key(&devices.keyboard.downArrow, false);
    // Tab always moves the focus.
    purr_gui_hide(&gui, &sampled);
    key(&devices.keyboard.tab, true);
    PURR_CHECK(two_buttons() == 0);
    PURR_CHECK(gui.focus == 10);
}

PURR_TEST(gui_mouse_over_the_gui_is_hidden)
{
    start();
    mouse(20.0f, 20.0f, false);
    two_buttons();
    purr_devices sampled = devices;
    purr_button_set(&sampled.mouse.left, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(!sampled.mouse.left.pressed);

    mouse(900.0f, 900.0f, false);
    two_buttons();
    sampled = devices;
    purr_button_set(&sampled.mouse.left, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(sampled.mouse.left.pressed);
}

// A frame with an int field under a label column.
static bool int_field(int32_t *value)
{
    begin();
    const bool changed = purr_gui_layout_int_field(&gui, 5, "Players", value);
    end();
    return changed;
}

PURR_TEST(gui_typing_into_a_field)
{
    start();
    int32_t players = 4;
    mouse(260.0f, 24.0f, false); // Past the 240 label column
    int_field(&players);
    mouse(260.0f, 24.0f, true);
    int_field(&players);
    PURR_CHECK(gui.editing == 5);
    mouse(260.0f, 24.0f, false);
    type("1x2"); // The x isn't part of a number
    PURR_CHECK(!int_field(&players));
    PURR_CHECK(players == 4); // Not until it's kept
    key(&devices.keyboard.enter, true);
    PURR_CHECK(int_field(&players));
    PURR_CHECK(players == 12);
    PURR_CHECK(gui.editing == 0);
    key(&devices.keyboard.enter, false);

    // Escape goes back to the old value.
    key(&devices.keyboard.enter, true); // It still has the focus: Enter types again
    int_field(&players);
    key(&devices.keyboard.enter, false);
    type("99");
    int_field(&players);
    key(&devices.keyboard.escape, true);
    PURR_CHECK(!int_field(&players));
    PURR_CHECK(players == 12);
    key(&devices.keyboard.escape, false);

    // With the focus, the arrows step it.
    key(&devices.keyboard.rightArrow, true);
    PURR_CHECK(int_field(&players));
    PURR_CHECK(players == 13);
}

PURR_TEST(gui_anchored_area_centers_its_content)
{
    start();
    for (int frame = 0; frame < 2; frame++) {
        begin();
        const int depth = purr_gui_begin_area_at(&gui, 99, PURR_ANCHOR_MIDDLE_CENTER);
        purr_gui_layout_button(&gui, 1, "Play");
        purr_gui_close(&gui, depth);
        end();
    }
    // The panel, then the button, centered on the screen both frames.
    PURR_REQUIRE(draw.count >= 3);
    PURR_CHECK(draw.commands[1].a.x == 960.0f && draw.commands[1].a.y == 540.0f);
    PURR_CHECK(draw.commands[2].a.x == 960.0f && draw.commands[2].a.y == 540.0f);
    PURR_CHECK(draw.commands[1].b.y == 48.0f + 2.0f * 20.0f);
}

PURR_TEST(gui_horizontal_groups_sit_side_by_side)
{
    start();
    begin();
    const int depth = purr_gui_begin_horizontal(&gui, 3);
    purr_gui_layout_button(&gui, 1, "A");
    purr_gui_layout_button(&gui, 2, "B");
    purr_gui_close(&gui, depth);
    purr_gui_layout_button(&gui, 4, "C");
    end();
    // A and B on one row, C under them.
    PURR_REQUIRE(draw.count >= 7);
    const purr_draw_command *a = &draw.commands[1];
    const purr_draw_command *b = &draw.commands[3];
    const purr_draw_command *c = &draw.commands[5];
    PURR_CHECK(a->a.y == b->a.y && b->a.x > a->a.x);
    PURR_CHECK(c->a.y == a->a.y + 48.0f + 8.0f);
}

PURR_TEST(gui_close_ends_what_was_left_open)
{
    start();
    begin();
    const int outer = purr_gui_begin_vertical(&gui, 1);
    purr_gui_begin_horizontal(&gui, 2); // A return skipped its close
    purr_gui_close(&gui, outer);
    PURR_CHECK(gui.depth == 0);
    purr_gui_begin_vertical(&gui, 3);
    end(); // Closes the rest
    PURR_CHECK(gui.depth == 0);
}
