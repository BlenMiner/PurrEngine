#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world before;
static purr_world now;
static purr_local local;
static purr_draw_list draw;
static purr_gui gui;
static purr_devices devices;

static void frame(const purr_world *previous, const float alpha)
{
    purr_draw_reset(&draw);
    purr_gui_begin(&gui, &devices, purr_f2(1920.0f, 1080.0f), NULL);
    purr_frame(&now, previous, alpha, &local, &draw, &gui);
    purr_gui_end(&gui, &draw);
}

static const purr_draw_command *nth(const uint32_t kind, int n)
{
    for (uint32_t i = 0; i < draw.count; i++) {
        if (draw.commands[i].kind == kind && n-- == 0) return &draw.commands[i];
    }
    return NULL;
}

PURR_TEST(blend_views_see_the_match_between_ticks)
{
    purr_local_init(&local);
    purr_world_init(&now, 1.0f / 20.0f);
    purr_world_tick(&now); // Tick 0: the first body moves
    purr_world_copy(&before, &now);
    purr_world_tick(&now); // Tick 1: it moves again, and a second body appears

    frame(&before, 0.25f);
    const purr_draw_command *first = nth(PURR_DRAW_CIRCLE, 0);
    const purr_draw_command *second = nth(PURR_DRAW_CIRCLE, 1);
    PURR_REQUIRE(first && second);
    PURR_CHECK(first->a.x == 5.0f);                   // 4 then 8: a quarter of the way
    PURR_CHECK(first->b.x == 3.0f);                   // [Snap]: as it is, 1 + 2
    PURR_CHECK(first->color.r == 0.625f);             // A struct's floats blend too: 0.5 then 1
    PURR_CHECK(second->a.x == 100.0f);                // It wasn't there last tick: as it is
    const purr_draw_command *camera = nth(PURR_DRAW_CAMERA, 0);
    PURR_REQUIRE(camera != NULL);
    PURR_CHECK(camera->a.y == 10.0f); // A singleton: 8 then 16
    const purr_draw_command *text = nth(PURR_DRAW_TEXT, 0);
    PURR_REQUIRE(text != NULL);
    PURR_CHECK(strcmp(draw.text + text->text, "2 2") == 0); // Ints snap

    // Angle has its own Interpolate: 350 then 10, the short way round
    const purr_draw_command *heading = nth(PURR_DRAW_LINE, 0);
    PURR_REQUIRE(heading != NULL);
    PURR_CHECK(heading->b.x - heading->a.x == 355.0f);
    // What jumped is drawn where it is: an entity that snapped, and a singleton
    const purr_draw_command *jumper = nth(PURR_DRAW_WIRE_RECT, 0);
    PURR_REQUIRE(jumper != NULL);
    PURR_CHECK(jumper->a.x == 1000.0f);
    const purr_draw_command *shot = nth(PURR_DRAW_RECT, 0);
    PURR_REQUIRE(shot != NULL);
    PURR_CHECK(shot->a.y == 500.0f);

    frame(NULL, 0.25f); // No previous tick: as it is
    PURR_CHECK(nth(PURR_DRAW_CIRCLE, 0)->a.x == 8.0f);
    frame(&before, 1.0f);
    PURR_CHECK(nth(PURR_DRAW_CIRCLE, 0)->a.x == 8.0f);
}

// A snapshot copies what's in use, and clears what the world it goes into
// had beyond it: the same bytes as copying the whole world.
PURR_TEST(blend_snapshots_are_what_is_in_use)
{
    purr_world_init(&before, 1.0f / 20.0f);
    for (int i = 0; i < 5; i++) purr_world_tick(&before); // Further along: more entities
    purr_world_init(&now, 1.0f / 20.0f);
    purr_world_tick(&now);
    purr_world_copy(&before, &now);
    PURR_CHECK(memcmp(&before, &now, sizeof now) == 0);
    PURR_CHECK(purr_world_hash(&before) == purr_world_hash(&now));
    purr_world_tick(&before);
    purr_world_tick(&now);
    PURR_CHECK(purr_world_hash(&before) == purr_world_hash(&now));
    PURR_CHECK(memcmp(&before, &now, sizeof now) == 0);
}
