#include <string.h>

#include "game.h"
#include "tide_test.h"

// Frames of gui.tide's views on a 1920 x 1080 window, played with made-up
// devices. Widgets are found by their text in the draw list.

static tide_local local;
static tide_draw_list draw;
static tide_gui gui;
static tide_devices devices;

static void frame(void)
{
    tide_draw_reset(&draw);
    tide_pointer_poll(&devices, true); // As the platform polls it: the pointer follows the mouse
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
    devices.text.count = 0;
}

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
    tide_local_init(&local);
    frame();
}

// The `nth` text reading `text` the last frame drew, or NULL.
static const tide_draw_command *find(const char *text, int nth)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const tide_draw_command *c = &draw.commands[i];
        if (c->kind == TIDE_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0 && nth-- == 0) return c;
    }
    return NULL;
}

// Puts the mouse `dx` right of where the text starts, in its middle.
static bool point_at(const char *text, const int nth, const float dx)
{
    const tide_draw_command *c = find(text, nth);
    if (!c) return false;
    devices.mouse.position = tide_f2(c->a.x + dx, 1080.0f - (c->a.y + c->b.x * 0.5f));
    return true;
}

// Moves there, presses and lets go, a frame each, then draws what that did.
static bool click_at(const char *text, const int nth, const float dx)
{
    if (!point_at(text, nth, dx)) return false;
    frame();
    tide_button_set(&devices.mouse.left, true);
    frame();
    tide_button_set(&devices.mouse.left, false);
    frame();
    frame();
    return true;
}

static bool click(const char *text, const int nth)
{
    return click_at(text, nth, 4.0f);
}

static void press(tide_button *key)
{
    tide_button_set(key, true);
    frame();
    tide_button_set(key, false);
    frame();
}

TIDE_TEST(gui_buttons_change_local_state)
{
    start();
    TIDE_REQUIRE(click("Play", 0));
    TIDE_CHECK(local.Menu.plays == 1);
    TIDE_CHECK(local.Menu.page == Page_Options);
    TIDE_CHECK(find("Play", 0) == NULL);
    TIDE_REQUIRE(click("Back", 0));
    TIDE_CHECK(local.Menu.page == Page_Title);
    TIDE_CHECK(local.Menu.plays == 1);
}

TIDE_TEST(gui_rects_use_the_screen)
{
    start();
    const tide_draw_command *corner = find("Corner", 0);
    TIDE_REQUIRE(corner != NULL);
    TIDE_CHECK(corner->a.x > 1920.0f - 210.0f && corner->a.x < 1920.0f - 10.0f);
    TIDE_REQUIRE(click("Corner", 0));
    TIDE_CHECK(local.Menu.corner == 1);
}

TIDE_TEST(gui_functions_that_draw_have_their_own_widgets)
{
    start();
    TIDE_REQUIRE(click("Quit", 0));
    TIDE_CHECK(local.Menu.quits == 1);
    TIDE_CHECK(local.Menu.plays == 0);
    TIDE_CHECK(local.Menu.page == Page_Title);
}

TIDE_TEST(gui_containers_of_our_own)
{
    start();
    TIDE_REQUIRE(click("Play", 0));
    TIDE_CHECK(find("Volume", 0) == NULL); // Audio is folded
    TIDE_REQUIRE(click("Audio", 0));
    TIDE_CHECK(local.Menu.audioOpen);
    TIDE_CHECK(find("Volume", 0) != NULL);

    // Twice ran its block twice, side by side: two buttons, each its own.
    const tide_draw_command *first = find("Again", 0);
    const tide_draw_command *second = find("Again", 1);
    TIDE_REQUIRE(first && second);
    TIDE_CHECK(first->a.y == second->a.y && first->a.x < second->a.x);
    TIDE_REQUIRE(click("Again", 1));
    TIDE_CHECK(local.Menu.twice == 1);
}

TIDE_TEST(gui_entities_have_their_own_widgets)
{
    start();
    TIDE_REQUIRE(click("Tag", 1));
    const Tag first = tide_get_Tag(&local, (tide_entity){1, 1});
    const Tag second = tide_get_Tag(&local, (tide_entity){2, 1});
    TIDE_REQUIRE(tide_has_Tag(&local, (tide_entity){1, 1}) && tide_has_Tag(&local, (tide_entity){2, 1}));
    TIDE_CHECK(first.clicks == 0);
    TIDE_CHECK(second.clicks == 1);
}

TIDE_TEST(gui_loops_make_widgets_of_their_own)
{
    start();
    TIDE_CHECK(find("Row", 2) != NULL);
    TIDE_CHECK(find("Rows: 0", 0) != NULL);
    TIDE_REQUIRE(click("Row", 1));
    TIDE_CHECK(local.Menu.rows == 1);
    TIDE_CHECK(find("Rows: 1", 0) != NULL); // A label with a value in it
}

TIDE_TEST(gui_typing_text)
{
    start();
    TIDE_CHECK(find("Hello, cat", 0) != NULL);
    TIDE_REQUIRE(click_at("Name", 0, 250.0f)); // Past the label, in the field
    devices.text.count = 3;
    devices.text.chars[0] = 's';
    devices.text.chars[1] = 0xE9; // e with an acute accent: two bytes in UTF-8
    devices.text.chars[2] = '!';
    frame();
    frame();
    const tide_str name = tide_text_read(&local.heap, local.Menu.player);
    TIDE_CHECK(name.bytes == 7 && name.chars == 6 && memcmp(name.ptr, "cats\xC3\xA9!", 7) == 0);
    TIDE_CHECK(find("Hello, cats\xC3\xA9!", 0) != NULL);
    press(&devices.keyboard.backspace);
    press(&devices.keyboard.backspace); // The accent's two bytes go as one character
    TIDE_CHECK(tide_text_read(&local.heap, local.Menu.player).bytes == 4);
}

TIDE_TEST(gui_typing_into_fields)
{
    start();
    TIDE_REQUIRE(click("Play", 0));
    TIDE_REQUIRE(click("Audio", 0));
    TIDE_REQUIRE(click_at("Players", 0, 250.0f)); // Past the label, in the field
    devices.text.count = 1;
    devices.text.chars[0] = '7';
    frame();
    TIDE_CHECK(local.Menu.players == 2); // Kept on Enter
    press(&devices.keyboard.enter);
    TIDE_CHECK(local.Menu.players == 7);

    // A float2 field is two fields, showing 0 and 0: the second is y.
    TIDE_REQUIRE(click("0", 1));
    devices.text.count = 2;
    devices.text.chars[0] = '1';
    devices.text.chars[1] = '5';
    frame();
    press(&devices.keyboard.enter);
    TIDE_CHECK(local.Menu.offset.x == 0.0f && local.Menu.offset.y == 15.0f);
}

TIDE_TEST(gui_keyboard_moves_between_widgets)
{
    start();
    // In order: Corner, then Play and Quit, then the tags.
    press(&devices.keyboard.tab);
    press(&devices.keyboard.tab);
    press(&devices.keyboard.enter);
    TIDE_CHECK(local.Menu.corner == 0);
    TIDE_CHECK(local.Menu.plays == 1);
}

TIDE_TEST(gui_hides_what_it_uses_from_the_game)
{
    start();
    TIDE_REQUIRE(point_at("Play", 0, 4.0f));
    frame();
    tide_devices sampled = devices;
    tide_button_set(&sampled.mouse.left, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.mouse.left.pressed);

    devices.mouse.position = tide_f2(5.0f, 5.0f); // Nothing there
    frame();
    sampled = devices;
    tide_button_set(&sampled.mouse.left, true);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(sampled.mouse.left.pressed);
}

TIDE_TEST(gui_disabled_blocks_gray_out_their_widgets)
{
    start();
    TIDE_REQUIRE(click("Connect", 0));
    TIDE_CHECK(local.Menu.connects == 1 && local.Menu.waiting);
    TIDE_REQUIRE(click("Connect", 0)); // Waiting: grayed out, and clicks do nothing
    TIDE_CHECK(local.Menu.connects == 1);
    const tide_draw_command *text = find("Connect", 0);
    TIDE_REQUIRE(text != NULL);
    TIDE_CHECK(text->color.a == 0.5f);
    local.Menu.waiting = false;
    TIDE_REQUIRE(click("Connect", 0));
    TIDE_CHECK(local.Menu.connects == 2);
}
