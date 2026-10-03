#include <string.h>

#include "game.h"
#include "tide_test.h"

// Frames of claims.tide's views on a 1920 x 1080 window, and the samples a
// host takes before each, played with made-up devices. Toolbox is a widget a
// view draws itself, in the window's lower left corner; Look is another view,
// and the input's Sample is the game.

static tide_local local;
static tide_draw_list draw;
static tide_gui gui;
static tide_devices devices;

// This machine's input, as a host samples it before a frame: what the GUI
// uses and what views claimed is hidden.
static tide_input sample(void)
{
    tide_pointer_poll(&devices, true); // As the platform polls it: the pointer follows the mouse
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled);
    return tide_input_sample(&sampled, &local);
}

static void frame(void)
{
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
    devices.keyboard.text.count = 0; // Typed characters last one frame
}

// A sample, then a frame, as a host's loop goes.
static tide_input step(void)
{
    const tide_input in = sample();
    frame();
    return in;
}

static void start(void)
{
    memset(&gui, 0, sizeof gui);
    memset(&devices, 0, sizeof devices);
    tide_local_init(&local);
    devices.mouse.position = tide_f2(900.0f, 500.0f); // Away from the toolbox
    step();
}

static void type(const char *text)
{
    for (const char *c = text; *c; c++) devices.keyboard.text.chars[devices.keyboard.text.count++] = (uint32_t)(unsigned char)*c;
}

static bool reads(const tide_text field, const char *text)
{
    const tide_str s = tide_text_read(&local.heap, field);
    return (size_t)s.bytes == strlen(text) && memcmp(s.ptr, text, (size_t)s.bytes) == 0;
}

TIDE_TEST(claims_keep_a_click_from_the_game_and_other_views)
{
    start();
    // Away from the toolbox, a press is the game's and every view's.
    tide_button_set(&devices.mouse.left, true);
    TIDE_CHECK(step().fire);
    TIDE_CHECK(local.Camera.drags == 1 && local.Tools.clicks == 0);
    tide_button_set(&devices.mouse.left, false);
    step();

    // The pointer comes onto the toolbox, which claims it; then it's pressed.
    devices.mouse.position = tide_f2(100.0f, 100.0f);
    step();
    tide_button_set(&devices.mouse.left, true);
    const tide_input in = step();
    TIDE_CHECK(!in.fire);                           // Not the game's
    TIDE_CHECK(in.aim.x == 100.0f);                 // Where the pointer is isn't hidden
    TIDE_CHECK(local.Tools.clicks == 1);            // The view that claimed it read the press
    TIDE_CHECK(local.Camera.drags == 1);            // The other view didn't

    // Dragged off the toolbox, it stays the toolbox's until it's let go.
    devices.mouse.position = tide_f2(900.0f, 500.0f);
    TIDE_CHECK(!step().fire);
    TIDE_CHECK(local.Tools.dragging && local.Camera.drags == 1);
    tide_button_set(&devices.mouse.left, false);
    step();
    TIDE_CHECK(!local.Tools.dragging);

    // Let go, the pointer is everyone's again.
    step();
    tide_button_set(&devices.mouse.left, true);
    TIDE_CHECK(step().fire);
    TIDE_CHECK(local.Camera.drags == 2 && local.Tools.clicks == 1);
}

TIDE_TEST(claims_keep_typing_from_the_game_and_other_views)
{
    start();
    // Every view reads what's typed, while nothing has the keyboard.
    type("hi");
    tide_button_set(&devices.keyboard.w, true);
    TIDE_CHECK(step().forward);
    TIDE_CHECK(reads(local.Camera.typed, "hi") && local.Camera.count == 2 && local.Camera.keys == 1);
    TIDE_CHECK(reads(local.Tools.name, ""));
    TIDE_CHECK(!tide_gui_typing(&gui));
    tide_button_set(&devices.keyboard.w, false);
    step();

    // A click on the toolbox starts renaming: it claims the keyboard, and phones show theirs.
    devices.mouse.position = tide_f2(100.0f, 100.0f);
    step();
    tide_button_set(&devices.mouse.left, true);
    step();
    tide_button_set(&devices.mouse.left, false);
    step();
    TIDE_CHECK(local.Tools.renaming && tide_gui_typing(&gui));

    // W, and what it types with an accent after it: the toolbox's, not the game's or the camera's.
    tide_button_set(&devices.keyboard.w, true);
    devices.keyboard.text.count = 2;
    devices.keyboard.text.chars[0] = 'w';
    devices.keyboard.text.chars[1] = 0xE9; // e with an acute accent: two bytes in UTF-8
    TIDE_CHECK(!step().forward);
    TIDE_CHECK(reads(local.Tools.name, "w\xC3\xA9") && local.Tools.keys == 1);
    TIDE_CHECK(reads(local.Camera.typed, "hi") && local.Camera.count == 2 && local.Camera.keys == 1);

    // Enter stops renaming. The claim stands for the frame after, and then the keyboard is everyone's.
    tide_button_set(&devices.keyboard.enter, true);
    step();
    tide_button_set(&devices.keyboard.enter, false);
    TIDE_CHECK(!local.Tools.renaming);
    type("x");
    TIDE_CHECK(!step().forward);
    TIDE_CHECK(local.Camera.keys == 1 && reads(local.Camera.typed, "hi"));
    TIDE_CHECK(!tide_gui_typing(&gui));
    type("y");
    TIDE_CHECK(step().forward);
    TIDE_CHECK(local.Camera.keys == 2 && reads(local.Camera.typed, "hiy") && local.Camera.count == 3);
    TIDE_CHECK(reads(local.Tools.name, "w\xC3\xA9"));
}
