#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

static bool text_is(const purr_text t, const char *expected)
{
    const purr_str s = purr_text_read(&world.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

// The result after one tick. C keeps its counters between ticks, so the world
// ticks once, whichever test runs first.
static const Result *result(void)
{
    static bool ticked;
    if (!ticked) {
        purr_world_init(&world, 1.0f); // Loads Main, which spawns the result
        purr_world_tick(&world);
        ticked = true;
    }
    return purr_get_Result(&world, (purr_entity){1, 1});
}

PURR_TEST(extern_functions_call_c)
{
    const Result *r = result();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->sum == 4.5f);
    PURR_CHECK(r->twice == 42);
    PURR_CHECK(r->scaled.x == 2.0f && r->scaled.y == 4.0f && r->scaled.z == 6.0f);
    PURR_CHECK(r->swapped.a == 3.0f && r->swapped.b == 7);
    PURR_CHECK(r->counter == 6); // By address: the field itself
    PURR_CHECK(r->bumped.a == 0.5f && r->bumped.b == 1);
    PURR_CHECK(r->mode == Mode_Fast);
    PURR_CHECK(r->even);
    PURR_CHECK(r->half == 2.5f);
    PURR_CHECK(r->local == 2); // A local by address
    PURR_CHECK(r->calls == 2); // Add, twice, through Triple
}

PURR_TEST(extern_functions_take_pointers)
{
    const Result *r = result();
    PURR_REQUIRE(r != NULL);
    // `in`: a field by address, a computed value and a converted one through copies
    PURR_CHECK(r->length == 5.0f);
    PURR_CHECK(r->computedLength == 10.0f);
    PURR_CHECK(r->convertedLength == 7.0f);
    PURR_CHECK(r->pairSum == 10.0f);
    // Lists: their elements, changed in place with `mut`, NULL when empty
    PURR_CHECK(r->weightSum == 7.0f);
    PURR_CHECK(r->literalSum == 30.0f);
    PURR_CHECK(r->countsAfter == 246); // 2, 4, 6
    PURR_CHECK(r->emptyIsNull);
}

PURR_TEST(extern_functions_take_and_return_text)
{
    const Result *r = result();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->bytes == 6); // UTF-8: é is two bytes
    PURR_CHECK(r->formattedBytes == 3);
    PURR_CHECK(text_is(r->greeting, "hello, cat")); // Copied out of C's buffer
    PURR_CHECK(r->fieldBytes == 10);
    PURR_CHECK(text_is(r->nothing, "")); // NULL is empty text
}

PURR_TEST(extern_calls_run_left_to_right)
{
    const Result *r = result();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->order == 12);       // Not 21, where C takes arguments right to left
    PURR_CHECK(r->viaFunction == 34); // Through a function that calls C
    PURR_CHECK(r->rounds == 3);       // 12, 34, 56, then 78: a loop's condition, each round
    PURR_CHECK(r->returned == 12);    // A return's value
}

PURR_TEST(extern_calls_in_parts_that_run_sometimes)
{
    const Result *r = result();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->picked);            // 12 on the right of &&
    PURR_CHECK(r->mixed == 34);       // Not 43: the ?: side runs after the call before it
    PURR_CHECK(!r->never);            // Its right side never ran...
    PURR_CHECK(r->after == 5);        // ...so this is the fifth call
    PURR_CHECK(r->either);            // 67 on the right of ||
    PURR_CHECK(r->rounds2 == 3);      // A loop's condition with &&
    PURR_CHECK(r->operatorOrder == 12); // An operator that calls C, then the call after it
    PURR_CHECK(r->spawnCalls == 2);   // The spawn's call ran once
    PURR_CHECK(r->eachOrder == 12);   // A foreach's list
}
