#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static bool trail_is(const char *want)
{
    const tide_str s = tide_text_read(&world.heap, world.Log.trail);
    return (size_t)s.bytes == strlen(want) && memcmp(s.ptr, want, (size_t)s.bytes) == 0;
}

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

TIDE_TEST(tasks_flow_wait_wherever_code_goes)
{
    tide_world_init(&world, 0.1f);
    ticks(2); // Tick 0 starts Flow; tick 1 goes on with what it kept
    TIDE_CHECK(trail_is("ab31"));
    ticks(4); // Later(0) to Later(3), a tick each
    TIDE_CHECK(world.Log.steps == 3);
    TIDE_CHECK(world.Log.ands == 0);
    ticks(1);
    TIDE_CHECK(world.Log.ands == 1);
    ticks(1);
    TIDE_CHECK(world.Log.fallback == 42);
    ticks(4); // The for loop: its step and a switch wait
    TIDE_CHECK(world.Log.sum == 12);
    ticks(2);
    TIDE_CHECK(world.Log.failed == 1);
    ticks(2);
    TIDE_CHECK(world.Log.sum == 17);
    ticks(1);
    TIDE_CHECK(world.Log.failed == 2);
    ticks(1);
    TIDE_CHECK(world.Log.either == 0);
    ticks(1); // Either's tenth of a second: one tick of 0.1
    TIDE_CHECK(world.Log.either == 2);
    TIDE_CHECK(trail_is("ab31|done"));
    TIDE_CHECK(world.Log.children == 0); // The child it started waits on
    ticks(1);
    TIDE_CHECK(world.Log.children == 1);
    TIDE_CHECK(world.tide_tasks_function_Flow.count == 0 && world.tide_tasks_function_Child.count == 0);
    tide_world_free(&world);
}

TIDE_TEST(tasks_flow_run_in_either_world)
{
    tide_local_init(&local);
    for (int i = 0; i < 2; i++) {
        tide_local_frame_time(&local, 0.05f);
        tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    }
    TIDE_CHECK(local.Seen.either == -1);
    tide_local_frame_time(&local, 0.05f);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    TIDE_CHECK(local.Seen.either == 6);
    tide_local_free(&local);
}
