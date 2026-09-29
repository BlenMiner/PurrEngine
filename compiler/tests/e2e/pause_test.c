#include <string.h>

#include "game.h"
#include "purr_test.h"

// Frames of pause.purr's views on a 1920 x 1080 window, played with made-up
// devices, as in gui_test.c.

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
}

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
    purr_local_init(&local);
    frame();
}

static void press(purr_button *key)
{
    purr_button_set(key, true);
    frame();
    purr_button_set(key, false);
    frame();
}

static const purr_draw_command *find(const char *text)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const purr_draw_command *c = &draw.commands[i];
        if (c->kind == PURR_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0) return c;
    }
    return NULL;
}

// Moves to the text, presses and lets go, a frame each.
static bool click(const char *text)
{
    const purr_draw_command *c = find(text);
    if (!c) return false;
    devices.mouse.position = purr_f2(c->a.x + 4.0f, 1080.0f - (c->a.y + c->b.x * 0.5f));
    frame();
    purr_button_set(&devices.mouse.left, true);
    frame();
    purr_button_set(&devices.mouse.left, false);
    frame();
    return true;
}

PURR_TEST(pause_views_read_each_frame)
{
    start();
    purr_button_set(&devices.keyboard.escape, true);
    frame();
    PURR_CHECK(local.Pause.open && local.Pause.opened == 1);
    frame(); // Still held: it went down once
    PURR_CHECK(local.Pause.opened == 1);
    purr_button_set(&devices.keyboard.escape, false);
    frame();
    PURR_CHECK(find("Paused") != NULL);
}

PURR_TEST(pause_modal_takes_the_focus_and_back_closes_it)
{
    start();
    PURR_REQUIRE(click("Behind"));
    PURR_CHECK(local.Pause.behind == 1);

    press(&devices.keyboard.escape);
    PURR_CHECK(local.Pause.open);
    frame();
    // The focus is on Resume, the modal's first widget, and stays in the modal.
    press(&devices.keyboard.tab);
    press(&devices.keyboard.tab);
    press(&devices.keyboard.enter);
    PURR_CHECK(local.Pause.resumed == 1 && !local.Pause.open);
    PURR_CHECK(local.Pause.behind == 1);

    // Back closes it; the view that opens it doesn't see the Escape.
    press(&devices.keyboard.escape);
    PURR_CHECK(local.Pause.open && local.Pause.opened == 2);
    press(&devices.keyboard.escape);
    PURR_CHECK(!local.Pause.open && local.Pause.opened == 2);
    frame();
    PURR_CHECK(find("Paused") == NULL);
}

PURR_TEST(pause_modal_blocks_what_is_behind_it)
{
    start();
    press(&devices.gamepad.start);
    PURR_CHECK(local.Pause.open);
    PURR_REQUIRE(click("Behind"));
    PURR_CHECK(local.Pause.behind == 0);

    // The game gets nothing while it's up.
    purr_devices sampled = devices;
    purr_button_set(&sampled.keyboard.w, true);
    sampled.mouse.delta = purr_f2(3.0f, 4.0f);
    purr_gui_hide(&gui, &sampled);
    PURR_CHECK(!sampled.keyboard.w.pressed && sampled.mouse.delta.x == 0.0f);

    PURR_REQUIRE(click("Resume"));
    PURR_CHECK(!local.Pause.open && local.Pause.resumed == 1);
    frame();
    PURR_REQUIRE(click("Behind"));
    PURR_CHECK(local.Pause.behind == 1);
}
