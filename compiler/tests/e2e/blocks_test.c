#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_local local;
static purr_draw_list draw;
static purr_gui gui;
static purr_devices devices;

static const Counter *counter(void)
{
    return purr_get_Counter(&world, (purr_entity){1, 1});
}

PURR_TEST(blocks_run_as_the_callers_code)
{
    purr_world_init(&world, 1.0f); // Loads Main, which spawns the counter
    purr_world_tick(&world);
    const Counter *c = counter();
    PURR_REQUIRE(c != NULL);
    PURR_CHECK(c->twice == 2);
    PURR_CHECK(c->never == 0);
    PURR_CHECK(c->when == 10);
    PURR_CHECK(c->seen == 5); // The caller's x, not Shadow's
    PURR_CHECK(c->bumps == 1 && c->seenBumps == 1);
    PURR_CHECK(c->outer == 2);
    PURR_CHECK(c->picked == 1 && c->afterPick == 0); // The break left the system's switch
    PURR_CHECK(c->returned == 1 && c->after == 0);   // The return left the system
}

static const purr_draw_command *find(const char *text)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        const purr_draw_command *c = &draw.commands[i];
        if (c->kind == PURR_DRAW_TEXT && strcmp(draw.text + c->text, text) == 0) return c;
    }
    return NULL;
}

PURR_TEST(blocks_close_what_they_leave)
{
    purr_local_init(&local);
    purr_draw_reset(&draw);
    purr_gui_begin(&gui, &devices, purr_f2(1920.0f, 1080.0f), NULL);
    purr_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    PURR_CHECK(gui.depth == 0);
    purr_gui_end(&gui, &draw);

    const purr_draw_command *inside = find("inside");
    const purr_draw_command *between = find("between");
    const purr_draw_command *after = find("after");
    PURR_REQUIRE(inside && between && after);
    PURR_CHECK(inside->a.x > 500.0f);
    PURR_CHECK(find("unreached") == NULL);
    PURR_CHECK(between->a.x == 0.0f); // Back on the screen, out of the areas
    PURR_CHECK(after->a.x == 0.0f && after->a.y > between->a.y);
}
