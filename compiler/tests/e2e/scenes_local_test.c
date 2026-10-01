#include <stdbool.h>

#include "game.h"
#include "tide_test.h"

static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static void frame(void)
{
    tide_draw_reset(&draw);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui); // No match
}

TIDE_TEST(scenes_local_main_opens_a_menu)
{
#ifdef TIDE_MAIN_IS_LOCAL
    const bool main_is_local = true;
#else
    const bool main_is_local = false;
#endif
    TIDE_CHECK(main_is_local);
    tide_local_init(&local);
    TIDE_CHECK(tide_local_entity_count(&local) == 4); // Main, the menu and its two buttons
    frame();
    TIDE_CHECK(tide_local_entity_count(&local) == 4);
    frame();
    TIDE_CHECK(tide_local_entity_count(&local) == 1); // The menu closed, with its buttons
    TIDE_CHECK(local.Frames.opened == 1);
    frame();
    TIDE_CHECK(tide_local_entity_count(&local) == 4); // Main left, and loaded again with a menu
    TIDE_CHECK(local.Frames.opened == 2);
}
