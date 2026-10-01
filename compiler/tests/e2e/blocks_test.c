#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;
static tide_devices devices;

static const Counter *counter(void)
{
    return tide_get_Counter(&world, (tide_entity){1, 1});
}

TIDE_TEST(blocks_run_as_the_callers_code)
{
    tide_world_init(&world, 1.0f); // Loads Main, which spawns the counter
    tide_world_tick(&world);
    const Counter *c = counter();
    TIDE_REQUIRE(c != NULL);
    TIDE_CHECK(c->twice == 2);
    TIDE_CHECK(c->never == 0);
    TIDE_CHECK(c->when == 10);
    TIDE_CHECK(c->seen == 5); // The caller's x, not Shadow's
    TIDE_CHECK(c->bumps == 1 && c->seenBumps == 1);
    TIDE_CHECK(c->outer == 2);
    TIDE_CHECK(c->picked == 1 && c->afterPick == 0); // The break left the system's switch
    TIDE_CHECK(c->returned == 1 && c->after == 0);   // The return left the system
}

static const tide_draw_command *find(const char *text)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const tide_draw_command *c = &draw.commands[i];
        if (c->kind == TIDE_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0) return c;
    }
    return NULL;
}

TIDE_TEST(blocks_close_what_they_leave)
{
    tide_local_init(&local);
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    TIDE_CHECK(gui.depth == 0);
    tide_gui_end(&gui, &draw);

    const tide_draw_command *inside = find("inside");
    const tide_draw_command *between = find("between");
    const tide_draw_command *after = find("after");
    TIDE_REQUIRE(inside && between && after);
    TIDE_CHECK(inside->a.x > 500.0f);
    TIDE_CHECK(find("unreached") == NULL);
    TIDE_CHECK(between->a.x == 0.0f); // Back on the screen, out of the areas
    TIDE_CHECK(after->a.x == 0.0f && after->a.y > between->a.y);
}
