#include <string.h>

#include "game.h"
#include "tide_test.h"

// Frames of pause.tide's views on a 1920 x 1080 window, played with made-up
// devices, as in gui_test.c.

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
}

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
    tide_local_init(&local);
    frame();
}

static void press(tide_button *key)
{
    tide_button_set(key, true);
    frame();
    tide_button_set(key, false);
    frame();
}

static const tide_draw_command *find(const char *text)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const tide_draw_command *c = &draw.commands[i];
        if (c->kind == TIDE_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0) return c;
    }
    return NULL;
}

// Moves to the text, presses and lets go, a frame each.
static bool click(const char *text)
{
    const tide_draw_command *c = find(text);
    if (!c) return false;
    devices.mouse.position = tide_f2(c->a.x + 4.0f, 1080.0f - (c->a.y + c->b.x * 0.5f));
    frame();
    tide_button_set(&devices.mouse.left, true);
    frame();
    tide_button_set(&devices.mouse.left, false);
    frame();
    return true;
}

TIDE_TEST(pause_views_read_each_frame)
{
    start();
    tide_button_set(&devices.keyboard.escape, true);
    frame();
    TIDE_CHECK(local.Pause.open && local.Pause.opened == 1);
    frame(); // Still held: it went down once
    TIDE_CHECK(local.Pause.opened == 1);
    tide_button_set(&devices.keyboard.escape, false);
    frame();
    TIDE_CHECK(find("Paused") != NULL);
}

TIDE_TEST(pause_modal_takes_the_focus_and_back_closes_it)
{
    start();
    TIDE_REQUIRE(click("Behind"));
    TIDE_CHECK(local.Pause.behind == 1);

    press(&devices.keyboard.escape);
    TIDE_CHECK(local.Pause.open);
    frame();
    // The focus is on Resume, the modal's first widget, and stays in the modal.
    press(&devices.keyboard.tab);
    press(&devices.keyboard.tab);
    press(&devices.keyboard.enter);
    TIDE_CHECK(local.Pause.resumed == 1 && !local.Pause.open);
    TIDE_CHECK(local.Pause.behind == 1);

    // Back closes it; the view that opens it doesn't see the Escape.
    press(&devices.keyboard.escape);
    TIDE_CHECK(local.Pause.open && local.Pause.opened == 2);
    press(&devices.keyboard.escape);
    TIDE_CHECK(!local.Pause.open && local.Pause.opened == 2);
    frame();
    TIDE_CHECK(find("Paused") == NULL);
}

TIDE_TEST(pause_modal_blocks_what_is_behind_it)
{
    start();
    press(&devices.gamepad.start);
    TIDE_CHECK(local.Pause.open);
    TIDE_REQUIRE(click("Behind"));
    TIDE_CHECK(local.Pause.behind == 0);

    // The game gets nothing while it's up.
    tide_devices sampled = devices;
    tide_button_set(&sampled.keyboard.w, true);
    sampled.mouse.delta = tide_f2(3.0f, 4.0f);
    tide_gui_hide(&gui, &sampled);
    TIDE_CHECK(!sampled.keyboard.w.pressed && sampled.mouse.delta.x == 0.0f);

    TIDE_REQUIRE(click("Resume"));
    TIDE_CHECK(!local.Pause.open && local.Pause.resumed == 1);
    frame();
    TIDE_REQUIRE(click("Behind"));
    TIDE_CHECK(local.Pause.behind == 1);
}
