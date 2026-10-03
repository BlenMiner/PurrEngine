#include <string.h>

#include "tide/gui.h"
#include "tide/text.h"
#include "tide_test.h"

// The GUI runtime driven by made-up devices, on a 1920 x 1080 window. The
// mouse is given from the top left, y down, like the GUI.

static tide_gui gui;
static tide_devices devices;
static tide_draw_list draw;

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
}

static void mouse(const float x, const float y, const bool held)
{
    devices.mouse.position = tide_f2(x, 1080.0f - y);
    tide_button_set(&devices.mouse.left, held);
}

static void key(tide_button *b, const bool held)
{
    tide_button_set(b, held);
}

static void begin(void)
{
    tide_pointer_poll(&devices, true); // As the platform polls it: the pointer follows the mouse
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
}

static void end(void)
{
    tide_draw_reset(&draw);
    tide_gui_end(&gui, &draw);
    devices.keyboard.text.count = 0; // Typed characters last one frame
}

static void type(const char *text)
{
    for (const char *c = text; *c; c++) devices.keyboard.text.chars[devices.keyboard.text.count++] = (uint32_t)*c;
}

// A frame with two layout buttons; returns which were pressed (bit 0 and 1).
static int two_buttons(void)
{
    begin();
    const bool a = tide_gui_layout_button(&gui, 10, "Play");
    const bool b = tide_gui_layout_button(&gui, 20, "Quit");
    end();
    return (a ? 1 : 0) | (b ? 2 : 0);
}

TIDE_TEST(gui_button_clicks_on_release)
{
    start();
    // Play is at the top left, 28 tall; Quit below it after 5 of spacing.
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(two_buttons() == 0);
    mouse(20.0f, 47.0f, true);
    TIDE_CHECK(two_buttons() == 0); // Pressed, not released yet
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(two_buttons() == 2);
    TIDE_CHECK(two_buttons() == 0);

    // A press that leaves the button before it's released doesn't count.
    mouse(20.0f, 20.0f, true);
    TIDE_CHECK(two_buttons() == 0);
    mouse(900.0f, 900.0f, true);
    TIDE_CHECK(two_buttons() == 0);
    mouse(900.0f, 900.0f, false);
    TIDE_CHECK(two_buttons() == 0);
}

TIDE_TEST(gui_draws_over_the_world)
{
    start();
    begin();
    tide_gui_layout_label(&gui, "Hello");
    tide_gui_layout_button(&gui, 1, "Go");
    end();
    TIDE_REQUIRE(draw.count >= 4);
    TIDE_CHECK(draw.commands[0].kind == TIDE_DRAW_GUI);
    TIDE_CHECK(draw.commands[1].kind == TIDE_DRAW_TEXT && strcmp(draw.text + draw.commands[1].text, "Hello") == 0);
    TIDE_CHECK(draw.commands[2].kind == TIDE_DRAW_RECT); // The button, below the label
    TIDE_CHECK(draw.commands[2].a.y == 28.0f + 5.0f + 14.0f);

    begin(); // A frame without a GUI adds nothing
    end();
    TIDE_CHECK(draw.count == 0);
}

TIDE_TEST(gui_toggle_and_slider_change_values)
{
    start();
    bool on = false;
    float volume = 0.5f;
    for (int frame = 0; frame < 3; frame++) {
        mouse(10.0f, 14.0f, frame == 1);
        begin();
        const bool toggled = tide_gui_layout_toggle(&gui, 1, "Fullscreen", &on);
        tide_gui_layout_slider(&gui, 2, "", &volume, 0.0f, 1.0f);
        end();
        TIDE_CHECK(toggled == (frame == 2));
    }
    TIDE_CHECK(on);

    // The slider is below the toggle: its track goes from x 6 to 6 + 180 + 56 - 56 - 12.
    const float left = 6.0f;
    const float width = 180.0f - 12.0f;
    for (int frame = 0; frame < 3; frame++) {
        mouse(frame == 0 ? left + 10.0f : left + width * 0.25f, 28.0f + 5.0f + 14.0f, frame < 2);
        begin();
        tide_gui_layout_toggle(&gui, 1, "Fullscreen", &on);
        tide_gui_layout_slider(&gui, 2, "", &volume, 0.0f, 1.0f);
        end();
    }
    TIDE_CHECK(volume == 0.25f);
}

TIDE_TEST(gui_ids_tell_loops_apart)
{
    start();
    begin();
    const uint32_t a = tide_gui_id(&gui, 7, 3);
    const uint32_t b = tide_gui_id(&gui, 7, 3);
    const uint32_t c = tide_gui_id(&gui, 8, 3);
    end();
    TIDE_CHECK(a != b && a != c && b != c);
    TIDE_CHECK(a != 0 && b != 0 && c != 0);
    begin();
    TIDE_CHECK(tide_gui_id(&gui, 7, 3) == a); // The same next frame
    TIDE_CHECK(tide_gui_id(&gui, 7, 3) == b);
    end();
}

TIDE_TEST(gui_keyboard_navigation)
{
    start();
    TIDE_CHECK(two_buttons() == 0); // Lays them out, for the order
    key(&devices.keyboard.downArrow, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 10); // The first one
    key(&devices.keyboard.downArrow, false);
    TIDE_CHECK(two_buttons() == 0);
    key(&devices.keyboard.downArrow, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 20);
    key(&devices.keyboard.downArrow, false);
    key(&devices.keyboard.enter, true);
    TIDE_CHECK(two_buttons() == 2);
    key(&devices.keyboard.enter, false);

    // While a widget has the focus, the game doesn't see the keyboard.
    tide_devices sampled = devices;
    key(&sampled.keyboard.w, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.keyboard.w.held && !sampled.keyboard.w.pressed);

    key(&devices.keyboard.escape, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 0);
    key(&devices.keyboard.escape, false);
    sampled = devices;
    key(&sampled.keyboard.w, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(sampled.keyboard.w.held);
}

TIDE_TEST(gui_arrows_wait_while_the_game_reads_them)
{
    start();
    TIDE_CHECK(two_buttons() == 0);
    // The game sampled the devices: the arrows are its, not the GUI's.
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    key(&devices.keyboard.downArrow, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 0);
    key(&devices.keyboard.downArrow, false);
    // Tab always moves the focus.
    tide_gui_hide(&gui, &sampled);
    key(&devices.keyboard.tab, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 10);
}

// A frame of two_buttons with a finger: the touchscreen polled as the platform
// does, then up to two events at (x, y) from the top left (TIDE_TOUCHES for
// none).
static int touch_frame(const tide_touch_phase first, const tide_touch_phase second, const float x, const float y)
{
    tide_touches_poll(&devices.touchscreen);
    const tide_touch_phase phases[] = {first, second};
    for (int i = 0; i < 2; i++) {
        if ((int)phases[i] != TIDE_TOUCHES) tide_touch_event(&devices.touchscreen, phases[i], 1, tide_f2(x, 1080.0f - y));
    }
    tide_pointer_poll(&devices, false);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    const bool a = tide_gui_layout_button(&gui, 10, "Play");
    const bool b = tide_gui_layout_button(&gui, 20, "Quit");
    end();
    return (a ? 1 : 0) | (b ? 2 : 0);
}

#define NONE ((tide_touch_phase)TIDE_TOUCHES)

TIDE_TEST(gui_fingers_press_buttons)
{
    start();
    TIDE_CHECK(touch_frame(TIDE_TOUCH_BEGAN, NONE, 20.0f, 47.0f) == 0); // On Quit
    TIDE_CHECK(gui.active == 20);
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.touchscreen.primaryTouch.press.pressed && !sampled.touchscreen.touches.at[0].press.pressed);
    TIDE_CHECK(!sampled.pointer.press.pressed); // The finger is the GUI's
    TIDE_CHECK(touch_frame(TIDE_TOUCH_ENDED, NONE, 20.0f, 47.0f) == 2); // Lifted on it: a click
    TIDE_CHECK(touch_frame(NONE, NONE, 0.0f, 0.0f) == 0);
    TIDE_CHECK(gui.hot == 0); // Nothing stays hovered where the finger lifted

    // A tap between two frames clicks too.
    TIDE_CHECK(touch_frame(TIDE_TOUCH_BEGAN, TIDE_TOUCH_ENDED, 20.0f, 20.0f) == 0);
    TIDE_CHECK(touch_frame(NONE, NONE, 0.0f, 0.0f) == 1);
}

// What a host does before a frame's GUI, with a finger: polls the touchscreen,
// with up to two events at (x, y) from the top left (TIDE_TOUCHES for none),
// then samples the game's input. Returns whether the sample has the finger's
// press.
static bool finger_sample(const tide_touch_phase first, const tide_touch_phase second, const float x, const float y)
{
    tide_touches_poll(&devices.touchscreen);
    const tide_touch_phase phases[] = {first, second};
    for (int i = 0; i < 2; i++) {
        if ((int)phases[i] != TIDE_TOUCHES) tide_touch_event(&devices.touchscreen, phases[i], 1, tide_f2(x, 1080.0f - y));
    }
    tide_pointer_poll(&devices, false);
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    tide_devices_consume(&devices);
    return sampled.pointer.press.pressed || sampled.touchscreen.primaryTouch.press.pressed
        || sampled.touchscreen.touches.at[0].press.pressed;
}

// The same with the mouse at (x, y) from the top left: whether the sample has
// its left button's press.
static bool mouse_sample(const float x, const float y, const bool held)
{
    tide_touches_poll(&devices.touchscreen); // No finger did anything this poll
    mouse(x, y, held);
    tide_pointer_poll(&devices, true);
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    tide_devices_consume(&devices);
    return sampled.mouse.left.pressed || sampled.pointer.press.pressed;
}

// Whether views read a press this frame: the pointer's, the primary touch's
// or the mouse's left button's.
static bool views_read_a_press(void)
{
    const tide_devices *d = &gui.devices;
    return d->pointer.press.pressed || d->pointer.press.down || d->pointer.press.up
        || d->touchscreen.primaryTouch.press.pressed || d->touchscreen.primaryTouch.press.down
        || d->mouse.left.pressed || d->mouse.left.down || d->mouse.left.up;
}

// ...and then the frame's GUI, two_buttons': which were pressed (bits 0 and 1),
// and whether views read a press (bit 2).
static int sampled_frame(void)
{
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    const bool read = views_read_a_press();
    const bool a = tide_gui_layout_button(&gui, 10, "Play");
    const bool b = tide_gui_layout_button(&gui, 20, "Quit");
    end();
    return (a ? 1 : 0) | (b ? 2 : 0) | (read ? 4 : 0);
}

TIDE_TEST(gui_a_finger_on_a_button_is_never_the_games)
{
    start();
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(sampled_frame() == 0); // The buttons are up, and no finger is anywhere
    // A finger touches Quit. The game samples before the GUI sees the touch,
    // and still doesn't get it; nor do views.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, NONE, 20.0f, 47.0f));
    TIDE_CHECK(sampled_frame() == 0);
    TIDE_CHECK(gui.active == 20);
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(sampled_frame() == 0);
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_ENDED, NONE, 20.0f, 47.0f));
    TIDE_CHECK(sampled_frame() == 2); // Lifted on it: a click, and not the game's
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(sampled_frame() == 0);

    // A tap between two frames, too.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, TIDE_TOUCH_ENDED, 20.0f, 20.0f));
    TIDE_CHECK(sampled_frame() == 0);
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(sampled_frame() == 1);
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(sampled_frame() == 0);

    // Away from the buttons, a finger is the game's and the views', from the frame it touches.
    TIDE_CHECK(finger_sample(TIDE_TOUCH_BEGAN, NONE, 900.0f, 900.0f));
    TIDE_CHECK(sampled_frame() == 4);
}

TIDE_TEST(gui_where_the_mouse_is_now_says_whose_click_it_is)
{
    start();
    TIDE_CHECK(!mouse_sample(900.0f, 900.0f, false));
    TIDE_CHECK(sampled_frame() == 0);
    // Between two frames, the mouse comes onto Quit and presses: the GUI never
    // saw it there, and the click is still the GUI's.
    TIDE_CHECK(!mouse_sample(20.0f, 47.0f, true));
    TIDE_CHECK(sampled_frame() == 0);
    TIDE_CHECK(gui.active == 20);
    TIDE_CHECK(!mouse_sample(20.0f, 47.0f, false));
    TIDE_CHECK(sampled_frame() == 2);

    // And it leaves Quit and presses between two frames: the game's and the
    // views', though the GUI last saw it on the button.
    TIDE_CHECK(!mouse_sample(20.0f, 47.0f, false));
    TIDE_CHECK(sampled_frame() == 0);
    TIDE_CHECK(mouse_sample(900.0f, 900.0f, true));
    TIDE_CHECK(sampled_frame() == 4);
}

// A frame with a button in an anchored area, if it's `up`, and a disabled
// button at a rect. Returns whether views read a press.
static bool area_frame(const bool up)
{
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    const bool read = views_read_a_press();
    if (up) {
        const int area = tide_gui_begin_area_at(&gui, 99, TIDE_ANCHOR_MIDDLE_CENTER);
        tide_gui_layout_button(&gui, 1, "Play");
        tide_gui_close(&gui, area);
    }
    const int off = tide_gui_begin_disabled(&gui, true);
    tide_gui_button(&gui, 2, (tide_rect){100.0f, 100.0f, 200.0f, 40.0f}, "Locked");
    tide_gui_close(&gui, off);
    end();
    return read;
}

TIDE_TEST(gui_a_finger_on_an_area_or_a_disabled_widget_is_never_the_games)
{
    start();
    // The area is in the middle of the screen, 52 tall: its button, and 12 of padding around it.
    for (int frame = 0; frame < 3; frame++) {
        finger_sample(NONE, NONE, 0.0f, 0.0f);
        area_frame(true);
    }
    // On the area's padding, which no widget has.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, NONE, 962.0f, 518.0f));
    TIDE_CHECK(!area_frame(true));
    TIDE_CHECK(gui.active == 0);
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_ENDED, NONE, 962.0f, 518.0f));
    TIDE_CHECK(!area_frame(true));
    // On a disabled button: it doesn't work, and it's still the GUI's.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, NONE, 150.0f, 120.0f));
    TIDE_CHECK(!area_frame(true));
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_ENDED, NONE, 150.0f, 120.0f));
    TIDE_CHECK(!area_frame(true));

    // An area that wasn't up last frame takes the finger a frame after it
    // comes up under it: nothing said it was coming.
    finger_sample(NONE, NONE, 0.0f, 0.0f);
    area_frame(false);
    TIDE_CHECK(finger_sample(TIDE_TOUCH_BEGAN, NONE, 962.0f, 518.0f));
    TIDE_CHECK(area_frame(true));
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(!area_frame(true));
}

// A frame's GUI after a sample: the Play button, and two views, the first with
// a widget of its own at `rect`, which it says each frame. Returns whether the
// first view read a press (bit 0), and the second (bit 1).
static int widget_frame(const tide_rect rect)
{
    int read = 0;
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_gui_view(&gui, 1);
    if (views_read_a_press()) read |= 1;
    tide_gui_claim_pointer_at(&gui, rect);
    tide_gui_view(&gui, 2);
    if (views_read_a_press()) read |= 2;
    tide_gui_layout_button(&gui, 10, "Play");
    end();
    return read;
}

TIDE_TEST(gui_a_press_on_a_views_own_widget_is_the_views)
{
    start();
    const tide_rect widget = {0.0f, 0.0f, 400.0f, 400.0f}; // Under Play, which is at the top left
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(widget_frame(widget) == 0);
    // A finger touches the widget, with nothing before it: the view's from the
    // frame it lands in, and never the game's or the other view's.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, NONE, 300.0f, 300.0f));
    TIDE_CHECK(widget_frame(widget) == 1);
    TIDE_CHECK(!finger_sample(NONE, NONE, 0.0f, 0.0f));
    TIDE_CHECK(widget_frame(widget) == 1);
    // Off the widget, it's everyone's: a view that drags claims the pointer wherever it goes.
    TIDE_CHECK(finger_sample(TIDE_TOUCH_MOVED, NONE, 900.0f, 900.0f));
    TIDE_CHECK(widget_frame(widget) == 3);
    finger_sample(TIDE_TOUCH_ENDED, NONE, 900.0f, 900.0f);
    widget_frame(widget);

    // The GUI comes first: on a button of the GUI's over the widget, the press is the GUI's.
    TIDE_CHECK(!finger_sample(TIDE_TOUCH_BEGAN, NONE, 20.0f, 14.0f));
    TIDE_CHECK(widget_frame(widget) == 0);
    TIDE_CHECK(gui.active == 10);
    finger_sample(TIDE_TOUCH_ENDED, NONE, 20.0f, 14.0f);
    widget_frame(widget);

    // A click the mouse made as it came onto the widget, too.
    TIDE_CHECK(!mouse_sample(900.0f, 900.0f, false));
    TIDE_CHECK(widget_frame(widget) == 0);
    TIDE_CHECK(!mouse_sample(300.0f, 300.0f, true));
    TIDE_CHECK(widget_frame(widget) == 1);
    // A widget that's gone leaves nothing claimed, a frame later.
    TIDE_CHECK(!mouse_sample(300.0f, 300.0f, true));
    TIDE_CHECK(widget_frame((tide_rect){0}) == 1);
    TIDE_CHECK(mouse_sample(300.0f, 300.0f, true));
    TIDE_CHECK(widget_frame((tide_rect){0}) == 3);
}

TIDE_TEST(gui_places_claimed_past_the_limit_hide_a_frame_late)
{
    start();
    const tide_rect widget = {800.0f, 800.0f, 100.0f, 100.0f};
    for (int frame = 0; frame < 3; frame++) {
        mouse_sample(850.0f, 850.0f, frame == 2);
        tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
        tide_gui_view(&gui, 1);
        for (uint32_t i = 0; i < TIDE_GUI_MAX_CLAIM_RECTS; i++) {
            tide_gui_claim_pointer_at(&gui, (tide_rect){(float)i, 0.0f, 1.0f, 1.0f});
        }
        tide_gui_claim_pointer_at(&gui, widget); // One more than fit: the pointer is on it
        end();
        TIDE_CHECK(gui.taken == TIDE_GUI_POINTER);
    }
    TIDE_CHECK(!mouse_sample(850.0f, 850.0f, true));
}

#undef NONE

// A frame of more buttons than a frame remembers the places of, 28 wide, in
// rows of 64.
static void many_buttons(void)
{
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    for (uint32_t i = 0; i < TIDE_GUI_MAX_RECTS + 6; i++) {
        tide_gui_button(&gui, i + 1, (tide_rect){(float)(i % 64) * 30.0f, (float)(i / 64) * 30.0f, 28.0f, 28.0f}, "");
    }
    end();
}

TIDE_TEST(gui_widgets_past_those_remembered_hide_a_frame_late)
{
    start();
    const uint32_t last = TIDE_GUI_MAX_RECTS + 5;
    const float x = (float)(last % 64) * 30.0f + 10.0f;
    const float y = (float)(last / 64) * 30.0f + 10.0f;
    mouse_sample(1900.0f, 1000.0f, false);
    many_buttons();
    TIDE_CHECK(gui.rects_full);
    // A button whose place is remembered takes a click made as the mouse came.
    TIDE_CHECK(!mouse_sample(40.0f, 10.0f, true));
    many_buttons();
    mouse_sample(40.0f, 10.0f, false);
    many_buttons();
    // One past those takes it once the GUI saw the mouse on it, as every widget did before.
    TIDE_CHECK(!mouse_sample(x, y, false));
    many_buttons();
    TIDE_CHECK(gui.over && gui.active == 0);
    TIDE_CHECK(!mouse_sample(x, y, true));
}

TIDE_TEST(gui_mouse_over_the_gui_is_hidden)
{
    start();
    mouse(20.0f, 20.0f, false);
    two_buttons();
    tide_devices sampled = devices;
    tide_button_set(&sampled.mouse.left, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.mouse.left.pressed);

    mouse(900.0f, 900.0f, false);
    two_buttons();
    sampled = devices;
    tide_button_set(&sampled.mouse.left, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(sampled.mouse.left.pressed);
}

// A frame with an int field under a label column.
static bool int_field(int32_t *value)
{
    begin();
    const bool changed = tide_gui_layout_int_field(&gui, 5, "Players", value);
    end();
    return changed;
}

TIDE_TEST(gui_typing_into_a_field)
{
    start();
    int32_t players = 4;
    mouse(170.0f, 14.0f, false); // Past the 130 label column
    int_field(&players);
    mouse(170.0f, 14.0f, true);
    int_field(&players);
    TIDE_CHECK(gui.editing == 5);
    mouse(170.0f, 14.0f, false);
    type("1x2"); // The x isn't part of a number
    TIDE_CHECK(!int_field(&players));
    TIDE_CHECK(players == 4); // Not until it's kept
    key(&devices.keyboard.enter, true);
    TIDE_CHECK(int_field(&players));
    TIDE_CHECK(players == 12);
    TIDE_CHECK(gui.editing == 0);
    key(&devices.keyboard.enter, false);

    // Escape goes back to the old value.
    key(&devices.keyboard.enter, true); // It still has the focus: Enter types again
    int_field(&players);
    key(&devices.keyboard.enter, false);
    type("99");
    int_field(&players);
    key(&devices.keyboard.escape, true);
    TIDE_CHECK(!int_field(&players));
    TIDE_CHECK(players == 12);
    key(&devices.keyboard.escape, false);

    // With the focus, the arrows step it.
    key(&devices.keyboard.rightArrow, true);
    TIDE_CHECK(int_field(&players));
    TIDE_CHECK(players == 13);
}

TIDE_TEST(gui_anchored_area_centers_its_content)
{
    start();
    for (int frame = 0; frame < 2; frame++) {
        begin();
        const int depth = tide_gui_begin_area_at(&gui, 99, TIDE_ANCHOR_MIDDLE_CENTER);
        tide_gui_layout_button(&gui, 1, "Play");
        tide_gui_close(&gui, depth);
        end();
    }
    // The panel, then the button, centered on the screen both frames.
    TIDE_REQUIRE(draw.count >= 3);
    TIDE_CHECK(draw.commands[1].a.x == 960.0f && draw.commands[1].a.y == 540.0f);
    TIDE_CHECK(draw.commands[2].a.x == 960.0f && draw.commands[2].a.y == 540.0f);
    TIDE_CHECK(draw.commands[1].b.y == 28.0f + 2.0f * 12.0f);
}

// The draw list's rects, in the GUI's order: each one's left and right edges.
static void rect_edges(const int index, float *left, float *right)
{
    int seen = 0;
    for (uint32_t i = 0; i < draw.count; i++) {
        const tide_draw_command *c = &draw.commands[i];
        if (c->kind != TIDE_DRAW_RECT || seen++ != index) continue;
        *left = c->a.x - c->b.x * 0.5f;
        *right = c->a.x + c->b.x * 0.5f;
        return;
    }
    *left = *right = -1.0f;
}

TIDE_TEST(gui_rows_shrink_to_fit_the_screen)
{
    start();
    // On a screen 400 wide, the row wants 402 and its area has 352: the text
    // field and the button shrink, the label's column first.
    tide_str local = TIDE_STR_EMPTY;
    const tide_textref ip = {.local = &local};
    for (int frame = 0; frame < 3; frame++) {
        tide_gui_begin(&gui, &devices, tide_f2(400.0f, 600.0f), NULL);
        const int area = tide_gui_begin_area_at(&gui, 99, TIDE_ANCHOR_MIDDLE_CENTER);
        tide_gui_layout_button(&gui, 1, "Start Host");
        const int row = tide_gui_begin_horizontal(&gui, 2);
        tide_gui_layout_text_field(&gui, 3, "IP", ip);
        tide_gui_layout_button(&gui, 4, "Connect");
        tide_gui_close(&gui, row);
        tide_gui_close(&gui, area);
        end();
    }
    float left, right;
    rect_edges(0, &left, &right); // The panel, 12 from both edges
    TIDE_CHECK(left > 11.99f && left < 12.01f);
    TIDE_CHECK(right > 387.99f && right < 388.01f);
    rect_edges(1, &left, &right); // Start Host, as wide as the area's room
    TIDE_CHECK(left > 23.99f && right < 376.01f && right > 375.99f);
    rect_edges(2, &left, &right); // The text field's box: the column shrank, not the box
    TIDE_CHECK(right - left > 179.99f && right - left < 180.01f);
    TIDE_CHECK(left > 24.0f + 19.2f + 10.0f);
    rect_edges(3, &left, &right); // Connect, shrunk a little
    TIDE_CHECK(right - left < 87.2f && right - left > 77.2f);
    TIDE_CHECK(right > 375.99f && right < 376.01f);
}

TIDE_TEST(gui_horizontal_groups_sit_side_by_side)
{
    start();
    begin();
    const int depth = tide_gui_begin_horizontal(&gui, 3);
    tide_gui_layout_button(&gui, 1, "A");
    tide_gui_layout_button(&gui, 2, "B");
    tide_gui_close(&gui, depth);
    tide_gui_layout_button(&gui, 4, "C");
    end();
    // A and B on one row, C under them.
    TIDE_REQUIRE(draw.count >= 7);
    const tide_draw_command *a = &draw.commands[1];
    const tide_draw_command *b = &draw.commands[3];
    const tide_draw_command *c = &draw.commands[5];
    TIDE_CHECK(a->a.y == b->a.y && b->a.x > a->a.x);
    TIDE_CHECK(c->a.y == a->a.y + 28.0f + 5.0f);
}

// A frame with Play, Quit in a Disabled block, then Back; returns which were
// pressed (bits 0 to 2).
static int quit_disabled(const bool disabled)
{
    begin();
    const bool a = tide_gui_layout_button(&gui, 10, "Play");
    const int depth = tide_gui_begin_disabled(&gui, disabled);
    const bool b = tide_gui_layout_button(&gui, 20, "Quit");
    tide_gui_close(&gui, depth);
    const bool c = tide_gui_layout_button(&gui, 30, "Back");
    end();
    return (a ? 1 : 0) | (b ? 2 : 0) | (c ? 4 : 0);
}

TIDE_TEST(gui_disabled_widgets_dont_work)
{
    start();
    // A click on Quit does nothing, but the mouse on it is still the GUI's.
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(quit_disabled(true) == 0);
    mouse(20.0f, 47.0f, true);
    TIDE_CHECK(quit_disabled(true) == 0);
    TIDE_CHECK(gui.active == 0);
    tide_devices sampled = devices;
    tide_button_set(&sampled.mouse.left, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.mouse.left.pressed);
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(quit_disabled(true) == 0);

    // Drawn faded, and laid out as usual: Back is under Quit.
    TIDE_REQUIRE(draw.count >= 7);
    const tide_draw_command *play = &draw.commands[1];
    const tide_draw_command *quit = &draw.commands[3];
    const tide_draw_command *back = &draw.commands[5];
    TIDE_CHECK(quit->color.a == play->color.a * 0.5f && back->color.a == play->color.a);
    TIDE_CHECK(draw.commands[4].kind == TIDE_DRAW_TEXT && draw.commands[4].color.a == 0.5f);
    TIDE_CHECK(back->a.y == quit->a.y + 28.0f + 5.0f);

    // Tab goes past it.
    key(&devices.keyboard.tab, true);
    quit_disabled(true);
    TIDE_CHECK(gui.focus == 10);
    key(&devices.keyboard.tab, false);
    quit_disabled(true);
    key(&devices.keyboard.tab, true);
    quit_disabled(true);
    TIDE_CHECK(gui.focus == 30);
    key(&devices.keyboard.tab, false);

    // Enabled, it works again.
    mouse(20.0f, 47.0f, true);
    TIDE_CHECK(quit_disabled(false) == 0);
    TIDE_CHECK(gui.active == 20);
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(quit_disabled(false) == 2);

    // Disabled while it's pressed, it lets go, and the release doesn't press it.
    mouse(20.0f, 47.0f, true);
    quit_disabled(false);
    TIDE_CHECK(gui.active == 20);
    quit_disabled(true);
    TIDE_CHECK(gui.active == 0);
    mouse(20.0f, 47.0f, false);
    TIDE_CHECK(quit_disabled(false) == 0);
}

// A frame with an int field, in a Disabled block.
static bool int_field_disabled(int32_t *value, const bool disabled)
{
    begin();
    const int depth = tide_gui_begin_disabled(&gui, disabled);
    const bool changed = tide_gui_layout_int_field(&gui, 5, "Players", value);
    tide_gui_close(&gui, depth);
    end();
    return changed;
}

TIDE_TEST(gui_disabled_fields_stop_typing)
{
    start();
    int32_t players = 4;
    mouse(170.0f, 14.0f, false);
    int_field_disabled(&players, false);
    mouse(170.0f, 14.0f, true);
    int_field_disabled(&players, false);
    TIDE_CHECK(gui.editing == 5 && gui.focus == 5);
    mouse(170.0f, 14.0f, false);
    type("9");
    int_field_disabled(&players, false);

    // Disabled while typing: what was typed is dropped, and so is the focus.
    TIDE_CHECK(!int_field_disabled(&players, true));
    TIDE_CHECK(players == 4);
    TIDE_CHECK(gui.editing == 0 && gui.focus == 0);
    key(&devices.keyboard.enter, true);
    TIDE_CHECK(!int_field_disabled(&players, false));
    TIDE_CHECK(gui.editing == 0);
}

TIDE_TEST(gui_disabled_blocks_nest)
{
    start();
    begin();
    const int outer = tide_gui_begin_disabled(&gui, true);
    tide_gui_begin_disabled(&gui, false);
    TIDE_CHECK(gui.groups[gui.depth].disabled); // Still disabled inside another
    tide_gui_begin_horizontal(&gui, 1);
    TIDE_CHECK(gui.groups[gui.depth].disabled); // And so are containers in it
    tide_gui_close(&gui, outer); // A return skipped the inner closes
    TIDE_CHECK(gui.depth == 0 && !gui.groups[0].disabled);
    end();

    // In a row, a Disabled block's widgets go on in the row.
    begin();
    const int row = tide_gui_begin_horizontal(&gui, 3);
    tide_gui_layout_button(&gui, 1, "A");
    const int off = tide_gui_begin_disabled(&gui, true);
    tide_gui_layout_button(&gui, 2, "B");
    tide_gui_close(&gui, off);
    tide_gui_layout_button(&gui, 4, "C");
    tide_gui_close(&gui, row);
    tide_gui_layout_button(&gui, 5, "D");
    end();
    TIDE_REQUIRE(draw.count >= 8);
    const tide_draw_command *a = &draw.commands[1];
    const tide_draw_command *b = &draw.commands[3];
    const tide_draw_command *c = &draw.commands[5];
    const tide_draw_command *d = &draw.commands[7];
    TIDE_CHECK(a->a.y == b->a.y && b->a.y == c->a.y);
    TIDE_CHECK(a->a.x < b->a.x && b->a.x < c->a.x);
    TIDE_CHECK(d->a.y == a->a.y + 28.0f + 5.0f);
}

TIDE_TEST(gui_close_ends_what_was_left_open)
{
    start();
    begin();
    const int outer = tide_gui_begin_vertical(&gui, 1);
    tide_gui_begin_horizontal(&gui, 2); // A return skipped its close
    tide_gui_close(&gui, outer);
    TIDE_CHECK(gui.depth == 0);
    tide_gui_begin_vertical(&gui, 3);
    end(); // Closes the rest
    TIDE_CHECK(gui.depth == 0);
}

TIDE_TEST(gui_views_read_what_is_typed)
{
    start();
    int32_t players = 4;
    mouse(900.0f, 900.0f, false); // Away from the field
    devices.keyboard.text.count = 3;
    devices.keyboard.text.chars[0] = 'h';
    devices.keyboard.text.chars[1] = 0xE9; // e with an acute accent: two bytes in UTF-8
    devices.keyboard.text.chars[2] = 0xD800; // Half of a UTF-16 pair: no character
    begin();
    tide_gui_layout_int_field(&gui, 5, "Players", &players);
    const tide_str typed = tide_str_typed(&gui.devices.keyboard.text);
    TIDE_CHECK(typed.bytes == 3 && typed.chars == 2 && memcmp(typed.ptr, "h\xC3\xA9", 3) == 0);
    // It's a frame's: the game's sample never has it.
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(sampled.keyboard.text.count == 0);
    end();
    begin();
    tide_gui_layout_int_field(&gui, 5, "Players", &players);
    TIDE_CHECK(tide_str_typed(&gui.devices.keyboard.text).bytes == 0); // Typed once
    end();

    // While the player types into a field of the GUI's, it's the GUI's.
    mouse(170.0f, 14.0f, true);
    int_field(&players);
    mouse(170.0f, 14.0f, false);
    TIDE_CHECK(gui.editing == 5);
    type("12");
    begin();
    tide_gui_layout_int_field(&gui, 5, "Players", &players);
    TIDE_CHECK(gui.devices.keyboard.text.count == 0);
    end();
}

// What `d` has of the mouse's left button, the pointer's press and the scroll
// (bits 0 to 2), the W key (bit 3) and what's typed (bit 4).
static int has(const tide_devices *d)
{
    return (d->mouse.left.pressed ? 1 : 0) | (d->pointer.press.pressed ? 2 : 0) | (d->mouse.scroll.y != 0.0f ? 4 : 0)
         | (d->keyboard.w.pressed ? 8 : 0) | (d->keyboard.text.count ? 16 : 0);
}

#define POINTER 7  // has: the pointer's bits
#define KEYBOARD 24 // ...and the keyboard's

// A frame of two views, the first with a widget of its own that it claims
// `what` for: what each read, the first view's in the low byte and the
// second's in the next.
static int two_views(const uint32_t what)
{
    int seen = 0;
    begin();
    for (uint32_t view = 1; view <= 2; view++) {
        tide_gui_view(&gui, view);
        seen |= has(&gui.devices) << (8 * (view - 1));
        if (view == 1 && (what & TIDE_GUI_POINTER)) tide_gui_claim_pointer(&gui);
        if (view == 1 && (what & TIDE_GUI_KEYBOARD)) tide_gui_claim_keyboard(&gui);
    }
    end();
    return seen;
}

// What the game's next sample has. What's typed is never in one.
static int sample(void)
{
    tide_pointer_poll(&devices, true); // As the platform polls it, before a frame's samples
    tide_devices d = devices;
    tide_gui_hide(&gui, &d);
    return has(&d);
}

// The mouse pressed and scrolled away from the GUI, W held and typed.
static void use_everything(void)
{
    mouse(900.0f, 900.0f, true);
    devices.mouse.poll_scroll = devices.mouse.scroll = tide_f2(0.0f, 1.0f);
    key(&devices.keyboard.w, true);
    type("w");
}

TIDE_TEST(gui_claims_hide_the_pointer_from_the_game_and_other_views)
{
    start();
    // The pointer comes onto the view's widget, which claims it before any press.
    mouse(900.0f, 900.0f, false);
    TIDE_CHECK(two_views(TIDE_GUI_POINTER) == 0);
    use_everything();
    TIDE_CHECK(sample() == 8); // The click isn't the game's; the keyboard still is
    // The view that claimed it reads the press, and the other doesn't.
    TIDE_CHECK(two_views(TIDE_GUI_POINTER) == ((POINTER | KEYBOARD) | KEYBOARD << 8));
    TIDE_CHECK(sample() == 8);
    // The mouse's position is everyone's, as it is over the GUI.
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(sampled.mouse.position.x == 900.0f && sampled.pointer.position.x == 900.0f);

    // A claim lasts a frame: the frame after the view stops is the last without the pointer.
    use_everything();
    TIDE_CHECK(two_views(0) == ((POINTER | KEYBOARD) | KEYBOARD << 8));
    TIDE_CHECK(sample() == (POINTER | 8));
    use_everything();
    TIDE_CHECK(two_views(0) == ((POINTER | KEYBOARD) | (POINTER | KEYBOARD) << 8));
}

TIDE_TEST(gui_claims_hide_the_keyboard_from_the_game_and_other_views)
{
    start();
    mouse(900.0f, 900.0f, false);
    TIDE_CHECK(two_views(TIDE_GUI_KEYBOARD) == 0);
    use_everything();
    key(&devices.gamepad.buttonSouth, true);
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.keyboard.w.pressed && !sampled.keyboard.w.held);
    TIDE_CHECK(sampled.gamepad.buttonSouth.pressed); // The gamepad isn't the keyboard's
    TIDE_CHECK(sample() == POINTER);                 // Nor is the pointer
    // The view that claimed it reads the key and what's typed, and the other doesn't.
    TIDE_CHECK(two_views(TIDE_GUI_KEYBOARD) == ((POINTER | KEYBOARD) | POINTER << 8));

    // Both at once.
    use_everything();
    TIDE_CHECK(two_views(TIDE_GUI_KEYBOARD | TIDE_GUI_POINTER) == ((POINTER | KEYBOARD) | POINTER << 8));
    TIDE_CHECK(sample() == 0);
    use_everything();
    TIDE_CHECK(two_views(0) == (POINTER | KEYBOARD));
    use_everything();
    TIDE_CHECK(two_views(0) == ((POINTER | KEYBOARD) | (POINTER | KEYBOARD) << 8));
}

#undef POINTER
#undef KEYBOARD

TIDE_TEST(gui_two_views_that_claim_both_keep_it)
{
    start();
    mouse(900.0f, 900.0f, true);
    for (int frame = 0; frame < 2; frame++) {
        begin();
        for (uint32_t view = 1; view <= 3; view++) {
            tide_gui_view(&gui, view);
            if (frame == 1) TIDE_CHECK(gui.devices.mouse.left.pressed == (view != 3));
            if (view != 3) tide_gui_claim_pointer(&gui);
        }
        end();
    }
    TIDE_CHECK(gui.claim_count == 2 && gui.taken == TIDE_GUI_POINTER);
}

TIDE_TEST(gui_use_comes_before_a_claim)
{
    start();
    // On a button of the GUI's, the pointer is the GUI's, whatever a view claimed.
    for (int frame = 0; frame < 3; frame++) {
        mouse(20.0f, 20.0f, frame > 0);
        begin();
        tide_gui_view(&gui, 1);
        if (frame == 2) TIDE_CHECK(!gui.devices.mouse.left.pressed && !gui.devices.pointer.press.pressed);
        tide_gui_claim_pointer(&gui);
        tide_gui_layout_button(&gui, 10, "Play");
        end();
    }
    TIDE_CHECK(gui.active == 10);
}

TIDE_TEST(gui_claimed_keyboard_keeps_tab_and_the_arrows)
{
    start();
    TIDE_CHECK(two_buttons() == 0); // Lays them out, for the order
    // A view has the keyboard: Tab and the arrows are its own, and don't start moving the focus.
    for (int frame = 0; frame < 3; frame++) {
        key(&devices.keyboard.tab, frame == 1);
        key(&devices.keyboard.downArrow, frame == 2);
        begin();
        tide_gui_view(&gui, 1);
        tide_gui_claim_keyboard(&gui);
        if (frame == 1) TIDE_CHECK(gui.devices.keyboard.tab.down);
        tide_gui_layout_button(&gui, 10, "Play");
        tide_gui_layout_button(&gui, 20, "Quit");
        end();
        TIDE_CHECK(gui.focus == 0);
    }
    key(&devices.keyboard.downArrow, false);
    // Once it lets go, Tab moves the focus again, a frame later: the claim stood for one more.
    TIDE_CHECK(two_buttons() == 0);
    key(&devices.keyboard.tab, true);
    TIDE_CHECK(two_buttons() == 0);
    TIDE_CHECK(gui.focus == 10);
}

TIDE_TEST(gui_views_show_the_phone_keyboard)
{
    start();
    begin();
    end();
    TIDE_CHECK(!tide_gui_typing(&gui));
    begin();
    tide_gui_show_keyboard(&gui);
    end();
    TIDE_CHECK(tide_gui_typing(&gui)); // While a view asks, each frame
    begin();
    end();
    TIDE_CHECK(!tide_gui_typing(&gui));
}

TIDE_TEST(gui_claims_past_the_limit_still_hide)
{
    start();
    mouse(900.0f, 900.0f, true);
    for (int frame = 0; frame < 2; frame++) {
        begin();
        for (uint32_t view = 1; view <= TIDE_GUI_MAX_CLAIMS + 1; view++) {
            tide_gui_view(&gui, view);
            // The views that fit keep reading it; the one past them doesn't
            if (frame == 1) TIDE_CHECK(gui.devices.mouse.left.pressed == (view <= TIDE_GUI_MAX_CLAIMS));
            tide_gui_claim_pointer(&gui);
        }
        end();
    }
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.mouse.left.pressed);
}
