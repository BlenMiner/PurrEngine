#include <stdbool.h>

#include "game.h"
#include "purr_test.h"

static purr_local local;
static purr_draw_list draw;

static void frame(void)
{
    purr_draw_reset(&draw);
    purr_frame(NULL, &local, &draw); // No match
}

PURR_TEST(scenes_local_main_opens_a_menu)
{
#ifdef PURR_MAIN_IS_LOCAL
    const bool main_is_local = true;
#else
    const bool main_is_local = false;
#endif
    PURR_CHECK(main_is_local);
    purr_local_init(&local);
    PURR_CHECK(purr_local_entity_count(&local) == 4); // Main, the menu and its two buttons
    frame();
    PURR_CHECK(purr_local_entity_count(&local) == 4);
    frame();
    PURR_CHECK(purr_local_entity_count(&local) == 1); // The menu closed, with its buttons
}
