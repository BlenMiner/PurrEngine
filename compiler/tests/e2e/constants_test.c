#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static bool reads(const tide_world *w, const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&w->heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

TIDE_TEST(constants_set_defaults_and_enum_values)
{
    TIDE_CHECK(Level_Low == 10);
    TIDE_CHECK(Level_High == 11);
    TIDE_CHECK(Level_Top == 103);
    tide_world_init(&world, 1.0f);
    const Totals *t = &world.Totals;
    TIDE_CHECK(t->health == 20);
    TIDE_CHECK(t->speed == 3.0f);
    TIDE_CHECK(t->origin.x == 1.0f && t->origin.y == 2.0f);
    TIDE_CHECK(t->originY == 2.0f);
    TIDE_CHECK(t->stats.armor == 5);
    TIDE_CHECK(t->stats.speed == 3.0f); // The struct's own default, which is a constant too
    TIDE_CHECK(t->level == Level_Low);
    TIDE_CHECK(reads(&world, t->greeting, "hello"));
    TIDE_CHECK(t->enabled);
    tide_world_free(&world);
}

TIDE_TEST(constants_read_in_code)
{
    tide_world_init(&world, 1.0f);
    tide_world_set_server_input(&world, (Controls){.power = 99}); // Clamped to LIMIT
    tide_world_tick(&world);
    const Totals *t = &world.Totals;
    TIDE_CHECK(t->health == 23);
    TIDE_CHECK(t->switched == 1);
    TIDE_CHECK(t->levelValue == 1);
    TIDE_CHECK(t->wrapped == 1); // The case label and the run time agree
    TIDE_CHECK(t->quotient == -3);
    TIDE_CHECK(t->half == 0.0f);
    TIDE_CHECK(t->third == 1.0f / 3.0f);
    TIDE_CHECK(t->tintGreen == 0.5f);
    TIDE_CHECK(t->speedTimesLimit == 9.0f);
    tide_world_tick(&world);
    TIDE_CHECK(world.Totals.switched == 2);
    tide_world_tick(&world);
    TIDE_CHECK(world.Totals.switched == 3);
    tide_world_free(&world);
}
