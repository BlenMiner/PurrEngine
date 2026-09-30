#include <string.h>

#include "game.h"
#include "purr_test.h"

// Frames of gui.purr's views on a 1920 x 1080 window, played with made-up
// devices. Widgets are found by their text in the draw list.

static purr_local local;
static purr_draw_list draw;
static purr_gui gui;
static purr_devices devices;

static void frame(void)
{
    purr_draw_reset(&draw);
    purr_gui_begin(&gui, &devices, purr_f2(1920.0f, 1080.0f), NULL);
    purr_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    purr_gui_end(&gui, &draw);
    devices.text.count = 0;
}

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
    purr_local_init(&local);
    frame();
}

// The `nth` text reading `text` the last frame drew, or NULL.
static const purr_draw_command *find(const char *text, int nth)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const purr_draw_command *c = &draw.commands[i];
        if (c->kind == PURR_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0 && nth-- == 0) return c;
    }
    return NULL;
}

// Puts the mouse `dx` right of where the text starts, in its middle.
static bool point_at(const char *text, const int nth, const float dx)
{
    const purr_draw_command *c = find(text, nth);
    if (!c) return false;
    devices.mouse.position = purr_f2(c->a.x + dx, 1080.0f - (c->a.y + c->b.x * 0.5f));
    return true;
}

// Moves there, presses and lets go, a frame each, then draws what that did.
static bool click_at(const char *text, const int nth, const float dx)
{
    if (!point_at(text, nth, dx)) return false;
    frame();
    purr_button_set(&devices.mouse.left, true);
    frame();
    purr_button_set(&devices.mouse.left, false);
    frame();
    frame();
    return true;
}

static bool click(const char *text, const int nth)
{
    return click_at(text, nth, 4.0f);
}

static void press(purr_button *key)
{
    purr_button_set(key, true);
    frame();
    purr_button_set(key, false);
    frame();
}

PURR_TEST(gui_buttons_change_local_state)
{
    start();
    PURR_REQUIRE(click("Play", 0));
    PURR_CHECK(local.Menu.plays == 1);
    PURR_CHECK(local.Menu.page == Page_Options);
    PURR_CHECK(find("Play", 0) == NULL);
    PURR_REQUIRE(click("Back", 0));
    PURR_CHECK(local.Menu.page == Page_Title);
    PURR_CHECK(local.Menu.plays == 1);
}

PURR_TEST(gui_rects_use_the_screen)
{
    start();
    const purr_draw_command *corner = find("Corner", 0);
    PURR_REQUIRE(corner != NULL);
    PURR_CHECK(corner->a.x > 1920.0f - 210.0f && corner->a.x < 1920.0f - 10.0f);
    PURR_REQUIRE(click("Corner", 0));
    PURR_CHECK(local.Menu.corner == 1);
}

PURR_TEST(gui_functions_that_draw_have_their_own_widgets)
{
    start();
    PURR_REQUIRE(click("Quit", 0));
    PURR_CHECK(local.Menu.quits == 1);
    PURR_CHECK(local.Menu.plays == 0);
    PURR_CHECK(local.Menu.page == Page_Title);
}

PURR_TEST(gui_containers_of_our_own)
{
    start();
    PURR_REQUIRE(click("Play", 0));
    PURR_CHECK(find("Volume", 0) == NULL); // Audio is folded
    PURR_REQUIRE(click("Audio", 0));
    PURR_CHECK(local.Menu.audioOpen);
    PURR_CHECK(find("Volume", 0) != NULL);

    // Twice ran its block twice, side by side: two buttons, each its own.
    const purr_draw_command *first = find("Again", 0);
    const purr_draw_command *second = find("Again", 1);
    PURR_REQUIRE(first && second);
    PURR_CHECK(first->a.y == second->a.y && first->a.x < second->a.x);
    PURR_REQUIRE(click("Again", 1));
    PURR_CHECK(local.Menu.twice == 1);
}

PURR_TEST(gui_entities_have_their_own_widgets)
{
    start();
    PURR_REQUIRE(click("Tag", 1));
    const Tag *first = purr_get_Tag(&local, (purr_entity){1, 1});
    const Tag *second = purr_get_Tag(&local, (purr_entity){2, 1});
    PURR_REQUIRE(first && second);
    PURR_CHECK(first->clicks == 0);
    PURR_CHECK(second->clicks == 1);
}

PURR_TEST(gui_loops_make_widgets_of_their_own)
{
    start();
    PURR_CHECK(find("Row", 2) != NULL);
    PURR_CHECK(find("Rows: 0", 0) != NULL);
    PURR_REQUIRE(click("Row", 1));
    PURR_CHECK(local.Menu.rows == 1);
    PURR_CHECK(find("Rows: 1", 0) != NULL); // A label with a value in it
}

PURR_TEST(gui_typing_text)
{
    start();
    PURR_CHECK(find("Hello, cat", 0) != NULL);
    PURR_REQUIRE(click_at("Name", 0, 250.0f)); // Past the label, in the field
    devices.text.count = 3;
    devices.text.chars[0] = 's';
    devices.text.chars[1] = 0xE9; // e with an acute accent: two bytes in UTF-8
    devices.text.chars[2] = '!';
    frame();
    frame();
    const purr_str name = purr_text_read(&local.heap, local.Menu.player);
    PURR_CHECK(name.bytes == 7 && name.chars == 6 && memcmp(name.ptr, "cats\xC3\xA9!", 7) == 0);
    PURR_CHECK(find("Hello, cats\xC3\xA9!", 0) != NULL);
    press(&devices.keyboard.backspace);
    press(&devices.keyboard.backspace); // The accent's two bytes go as one character
    PURR_CHECK(purr_text_read(&local.heap, local.Menu.player).bytes == 4);
}

PURR_TEST(gui_typing_into_fields)
{
    start();
    PURR_REQUIRE(click("Play", 0));
    PURR_REQUIRE(click("Audio", 0));
    PURR_REQUIRE(click_at("Players", 0, 250.0f)); // Past the label, in the field
    devices.text.count = 1;
    devices.text.chars[0] = '7';
    frame();
    PURR_CHECK(local.Menu.players == 2); // Kept on Enter
    press(&devices.keyboard.enter);
    PURR_CHECK(local.Menu.players == 7);

    // A float2 field is two fields, showing 0 and 0: the second is y.
    PURR_REQUIRE(click("0", 1));
    devices.text.count = 2;
    devices.text.chars[0] = '1';
    devices.text.chars[1] = '5';
    frame();
    press(&devices.keyboard.enter);
    PURR_CHECK(local.Menu.offset.x == 0.0f && local.Menu.offset.y == 15.0f);
}

PURR_TEST(gui_keyboard_moves_between_widgets)
{
    start();
    // In order: Corner, then Play and Quit, then the tags.
    press(&devices.keyboard.tab);
    press(&devices.keyboard.tab);
    press(&devices.keyboard.enter);
    PURR_CHECK(local.Menu.corner == 0);
    PURR_CHECK(local.Menu.plays == 1);
}

PURR_TEST(gui_hides_what_it_uses_from_the_game)
{
    start();
    PURR_REQUIRE(point_at("Play", 0, 4.0f));
    frame();
    purr_devices sampled = devices;
    purr_button_set(&sampled.mouse.left, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(!sampled.mouse.left.pressed);

    devices.mouse.position = purr_f2(5.0f, 5.0f); // Nothing there
    frame();
    sampled = devices;
    purr_button_set(&sampled.mouse.left, true);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(sampled.mouse.left.pressed);
}
