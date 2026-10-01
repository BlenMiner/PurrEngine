#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static void frames(const tide_world *w, const int n, const float seconds)
{
    for (int i = 0; i < n; i++) {
        tide_local_frame_time(&local, seconds);
        tide_draw_reset(&draw);
        tide_frame(w, NULL, 1.0f, &local, &draw, &gui);
    }
}

static bool text_is(const tide_text t, const char *want)
{
    const tide_str s = tide_text_read(&local.heap, t);
    return (size_t)s.bytes == strlen(want) && memcmp(s.ptr, want, (size_t)s.bytes) == 0;
}

static void name(const char *text)
{
    tide_text_use(NULL, &local.heap);
    tide_text_set(&local.Account.name, tide_str_from_cstr(text), TIDE_IN_LOCAL);
}

TIDE_TEST(tasks_local_wait_for_seconds_frame_by_frame)
{
    tide_local_init(&local);
    frames(NULL, 1, 0.1f);
    TIDE_CHECK(local.Account.state == 0);
    name("ana");
    local.Account.go = true;
    frames(NULL, 1, 0.1f); // Login starts SignIn, which waits half a second
    TIDE_CHECK(local.Account.state == 1);
    TIDE_CHECK(local.tide_tasks_function_SignIn.count == 1);
    frames(NULL, 4, 0.1f);
    TIDE_CHECK(local.Account.state == 1);
    frames(NULL, 1, 0.1f);
    TIDE_CHECK(local.Account.state == 2);
    TIDE_CHECK(text_is(local.Account.token, "token-ana"));
    TIDE_CHECK(local.Account.tries == 1);
    TIDE_CHECK(local.tide_tasks_function_SignIn.count == 0);
    // Its toast showed for a frame, then a handler's task took it away
    TIDE_CHECK(tide_local_entity_count(&local) == 1);
    frames(NULL, 3, 0.1f);
    TIDE_CHECK(tide_local_entity_count(&local) == 0);
    TIDE_CHECK(local.tide_tasks_handler_Fade.count == 0);
    tide_local_free(&local);
}

TIDE_TEST(tasks_local_fail_with_what_awaits_them)
{
    tide_local_init(&local);
    local.Account.go = true; // With no name
    frames(NULL, 1, 0.1f);
    TIDE_CHECK(local.Account.state == 3); // Fetch failed at once, so SignIn never waited
    TIDE_CHECK(local.tide_tasks_function_SignIn.count == 0);
    TIDE_CHECK(tide_local_entity_count(&local) == 0);
    tide_local_free(&local);
}

TIDE_TEST(tasks_local_read_the_match_and_end_with_its_entities)
{
    tide_world_init(&world, 1.0f);
    tide_local_init(&local);
    frames(&world, 1, 0.1f); // Balls starts Watch for the ball
    TIDE_CHECK(local.Account.frames == -1);
    TIDE_CHECK(local.tide_tasks_function_Watch.count == 1);
    frames(&world, 2, 0.1f);
    TIDE_CHECK(local.Account.frames == 11);
    TIDE_CHECK(local.tide_tasks_function_Watch.count == 0);

    // Without the match, a task that reads it ends
    local.Account.frames = 0;
    frames(&world, 1, 0.1f);
    TIDE_CHECK(local.tide_tasks_function_Watch.count == 1);
    frames(NULL, 3, 0.1f);
    TIDE_CHECK(local.tide_tasks_function_Watch.count == 0);
    TIDE_CHECK(local.Account.frames == -1);
    tide_local_free(&local);
    tide_world_free(&world);
}
