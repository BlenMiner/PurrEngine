#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static void ticks(int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(&world);
}

static bool said(const tide_text t, const char *want)
{
    const tide_str s = tide_text_read(&world.heap, t);
    return (size_t)s.bytes == strlen(want) && memcmp(s.ptr, want, (size_t)s.bytes) == 0;
}

static uint32_t task_count(void)
{
    return world.tide_tasks_handler_Countdown.count + world.tide_tasks_function_Dash.count
         + world.tide_tasks_function_Chain.count;
}

TIDE_TEST(tasks_a_handler_waits_a_tick_at_a_time)
{
    tide_world_init(&world, 0.1f);
    // It started as Main loaded, and waits for the next tick
    TIDE_CHECK(world.Log.countdown == 3);
    TIDE_CHECK(world.tide_tasks_handler_Countdown.count == 1);
    ticks(1); // Tick 0, the one Main loaded in
    TIDE_CHECK(world.Log.countdown == 3);
    ticks(1);
    TIDE_CHECK(world.Log.countdown == 2);
    ticks(1);
    TIDE_CHECK(world.Log.countdown == 1);
    TIDE_CHECK(world.Log.counted == 0);
    ticks(1);
    TIDE_CHECK(world.Log.countdown == 0);
    TIDE_CHECK(world.Log.counted == 1);
    TIDE_CHECK(world.tide_tasks_handler_Countdown.count == 0);
}

TIDE_TEST(tasks_get_their_component_again_after_waiting)
{
    tide_world_init(&world, 0.1f);
    ticks(1); // StartDash starts it: 10 at once
    const Body body = TIDE_AT(&world, arch0_Body, Body, 0);
    TIDE_CHECK(body.speed == 10.0f);
    TIDE_CHECK(world.tide_tasks_function_Dash.count == 1);
    ticks(1);
    TIDE_CHECK(TIDE_AT(&world, arch0_Body, Body, 0).speed == 10.0f);
    ticks(1); // Two ticks after it started
    TIDE_CHECK(TIDE_AT(&world, arch0_Body, Body, 0).speed == 1.0f);
    TIDE_CHECK(TIDE_AT(&world, arch0_Body, Body, 0).dashes == 1);
    TIDE_CHECK(world.tide_tasks_function_Dash.count == 0);
}

TIDE_TEST(tasks_await_values_and_errors)
{
    tide_world_init(&world, 0.1f);
    ticks(1); // Chain starts, and Doubled(3) waits a tick
    TIDE_CHECK(world.tide_tasks_function_Chain.count == 1);
    ticks(2); // 6, then 13
    TIDE_CHECK(world.Log.doubled == 13);
    TIDE_CHECK(said(world.Log.said, ""));
    ticks(5); // Half a second: 5 ticks of 0.1
    TIDE_CHECK(said(world.Log.said, "hi ana"));
    TIDE_CHECK(world.Log.failures == 1); // Greet("") failed at once
    ticks(3); // A tick per name
    TIDE_CHECK(said(world.Log.trail, "xyz"));
    TIDE_CHECK(world.tide_tasks_function_Chain.count == 0);
    TIDE_CHECK(task_count() == 0);
}

TIDE_TEST(tasks_wait_in_snapshots_and_go_on_the_same)
{
    tide_world_init(&world, 0.1f);
    ticks(3);
    static tide_world snapshot;
    tide_world_copy(&snapshot, &world);
    TIDE_CHECK(tide_world_hash(&snapshot) == tide_world_hash(&world));
    ticks(10);
    const uint64_t later = tide_world_hash(&world);
    // The snapshot's tasks go on the same way
    for (int i = 0; i < 10; i++) tide_world_tick(&snapshot);
    TIDE_CHECK(tide_world_hash(&snapshot) == later);
    TIDE_CHECK(said(world.Log.trail, "xyz"));
    tide_world_free(&snapshot);
}

TIDE_TEST(tasks_end_with_their_owner)
{
    tide_world_init(&world, 0.1f);
    ticks(1);
    TIDE_CHECK(world.tide_tasks_function_Linger.count == 1);
    ticks(3); // Destroyed at its third tick
    TIDE_CHECK(world.tide_tasks_function_Linger.count == 0);
    ticks(10);
    TIDE_CHECK(world.Lingered.times == 0);
}

TIDE_TEST(tasks_wait_in_a_packed_world)
{
    tide_world_init(&world, 0.1f);
    ticks(4);
    const uint32_t size = tide_world_pack(&world, NULL, 0);
    uint8_t *bytes = malloc(size);
    TIDE_REQUIRE(tide_world_pack(&world, bytes, size) == size);
    static tide_world copy;
    TIDE_REQUIRE(tide_world_unpack(&copy, bytes, size));
    free(bytes);
    TIDE_CHECK(tide_world_hash(&copy) == tide_world_hash(&world));
    ticks(10);
    for (int i = 0; i < 10; i++) tide_world_tick(&copy);
    TIDE_CHECK(tide_world_hash(&copy) == tide_world_hash(&world));
    tide_world_free(&copy);
}
