#include <string.h>

#include "game.h"
#include "tide_test.h"

int32_t TickCount(void); // fails_c.c

static tide_world world;

static bool text_is(const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&world.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

// The results after one tick. C keeps its counter between ticks, so the world
// ticks once, whichever test runs first.
static const Results *results(void)
{
    static bool ticked;
    if (!ticked) {
        tide_world_init(&world, 1.0f);
        tide_world_tick(&world);
        ticked = true;
    }
    return &world.Results;
}

TIDE_TEST(fails_falls_back)
{
    const Results *r = results();
    TIDE_CHECK(r->fallback == 7);
    TIDE_CHECK(r->parsed == 42);
    TIDE_CHECK(r->doubled == 42);
    TIDE_CHECK(r->doubledBad == -1);
    TIDE_CHECK(r->chained == 9);
    TIDE_CHECK(r->widened == 0.5f);
}

TIDE_TEST(fails_carries_on_with_the_default)
{
    const Results *r = results();
    TIDE_CHECK(r->sum == 3);
    TIDE_CHECK(r->sumBad == 0);
    TIDE_CHECK(r->statsHp == 5);
    TIDE_CHECK(r->defaultHp == 10); // Stats' own default, not zero
}

TIDE_TEST(fails_is)
{
    const Results *r = results();
    TIDE_CHECK(r->emptyError);
    TIDE_CHECK(r->why == ParseError_NotANumber);
    TIDE_CHECK(r->fromIs == 5);
    TIDE_CHECK(r->noValue);
    TIDE_CHECK(r->refusedCode == 3);
    TIDE_CHECK(text_is(r->refusedMessage, "busy"));
    TIDE_CHECK(r->validated == 11); // Validate(-1) and Checked(-2) fail, Checked(2) doesn't
    TIDE_CHECK(r->both == 7);
}

TIDE_TEST(fails_optional)
{
    const Results *r = results();
    TIDE_CHECK(r->found == 1);
    TIDE_CHECK(r->notFound == -1);
    TIDE_CHECK(r->missing);
    TIDE_CHECK(r->present);
    TIDE_CHECK(text_is(r->name, "zero"));
    TIDE_CHECK(text_is(r->noName, "none"));
    TIDE_CHECK(r->optionalLocal == 4);
    TIDE_CHECK(r->orZero == 8); // 6 + 0 + 2
}

TIDE_TEST(fails_loops_and_locals)
{
    const Results *r = results();
    TIDE_CHECK(r->loopSum == 10); // 4 + 3 + 2 + 1 + 0
    TIDE_CHECK(r->forFound == 5); // 3 is at 1, 8 at 2
    TIDE_CHECK(r->held == 9);
    TIDE_CHECK(r->digits == 3);
    TIDE_CHECK(r->noDigits == 0);
    TIDE_CHECK(r->spent == 15);
    TIDE_CHECK(r->overspent == -1);
}

TIDE_TEST(fails_where_only_some_code_runs)
{
    const Results *r = results();
    TIDE_CHECK(r->firstA == 12);
    TIDE_CHECK(r->firstB == -1);
    TIDE_CHECK(r->firstC == 0);
    TIDE_CHECK(r->firstD == 1);
    TIDE_CHECK(r->countTo == 5);
    TIDE_CHECK(r->countFail == -1);
}

TIDE_TEST(fails_closes_containers_it_leaves)
{
    static tide_local local;
    static tide_draw_list draw;
    static tide_gui gui;
    static tide_devices devices;
    tide_local_init(&local);
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    TIDE_CHECK(gui.depth == 0);
    tide_gui_end(&gui, &draw);
    TIDE_CHECK(local.Picked.good == 1);
    TIDE_CHECK(local.Picked.bad == -1);
    TIDE_CHECK(local.Picked.big == -1);
}

TIDE_TEST(fails_c_calls_in_order)
{
    const Results *r = results();
    // Tick() gave 1, which fails, so ?? ran Tick() again; Counted(0) didn't fail.
    TIDE_CHECK(r->ticks == 2);
    TIDE_CHECK(r->lazy == 0);
    TIDE_CHECK(TickCount() == 2);
}
