#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static bool text_is(const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&world.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

// The result after one tick. C keeps its counters between ticks, so the world
// ticks once, whichever test runs first.
static const Result *result(void)
{
    static bool ticked;
    if (!ticked) {
        tide_world_init(&world, 1.0f); // Loads Main, which spawns the result
        tide_world_tick(&world);
        ticked = true;
    }
    return tide_get_Result(&world, (tide_entity){1, 1});
}

// Starting a match calls C: a machine joining one never starts it itself
// to be sent only what it lacks (tide_game.pure_start).
TIDE_TEST(extern_starting_a_match_that_calls_c_isnt_pure)
{
    TIDE_CHECK(!tide_game_api.pure_start);
}

TIDE_TEST(extern_functions_call_c)
{
    const Result *r = result();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->sum == 4.5f);
    TIDE_CHECK(r->twice == 42);
    TIDE_CHECK(r->scaled.x == 2.0f && r->scaled.y == 4.0f && r->scaled.z == 6.0f);
    TIDE_CHECK(r->swapped.a == 3.0f && r->swapped.b == 7);
    TIDE_CHECK(r->counter == 6); // By address: the field itself
    TIDE_CHECK(r->bumped.a == 0.5f && r->bumped.b == 1);
    TIDE_CHECK(r->mode == Mode_Fast);
    TIDE_CHECK(r->even);
    TIDE_CHECK(r->half == 2.5f);
    TIDE_CHECK(r->local == 2); // A local by address
    TIDE_CHECK(r->calls == 2); // Add, twice, through Triple
}

TIDE_TEST(extern_functions_take_pointers)
{
    const Result *r = result();
    TIDE_REQUIRE(r != NULL);
    // `in`: a field by address, a computed value and a converted one through copies
    TIDE_CHECK(r->length == 5.0f);
    TIDE_CHECK(r->computedLength == 10.0f);
    TIDE_CHECK(r->convertedLength == 7.0f);
    TIDE_CHECK(r->pairSum == 10.0f);
    // Lists: their elements, changed in place with `mut`, NULL when empty
    TIDE_CHECK(r->weightSum == 7.0f);
    TIDE_CHECK(r->literalSum == 30.0f);
    TIDE_CHECK(r->countsAfter == 246); // 2, 4, 6
    TIDE_CHECK(r->emptyIsNull);
}

TIDE_TEST(extern_functions_take_and_return_text)
{
    const Result *r = result();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->bytes == 6); // UTF-8: é is two bytes
    TIDE_CHECK(r->formattedBytes == 3);
    TIDE_CHECK(text_is(r->greeting, "hello, cat")); // Copied out of C's buffer
    TIDE_CHECK(r->fieldBytes == 10);
    TIDE_CHECK(text_is(r->nothing, "")); // NULL is empty text
}

TIDE_TEST(extern_calls_run_left_to_right)
{
    const Result *r = result();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->order == 12);       // Not 21, where C takes arguments right to left
    TIDE_CHECK(r->viaFunction == 34); // Through a function that calls C
    TIDE_CHECK(r->rounds == 3);       // 12, 34, 56, then 78: a loop's condition, each round
    TIDE_CHECK(r->returned == 12);    // A return's value
}

TIDE_TEST(extern_calls_in_parts_that_run_sometimes)
{
    const Result *r = result();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->picked);            // 12 on the right of &&
    TIDE_CHECK(r->mixed == 34);       // Not 43: the ?: side runs after the call before it
    TIDE_CHECK(!r->never);            // Its right side never ran...
    TIDE_CHECK(r->after == 5);        // ...so this is the fifth call
    TIDE_CHECK(r->either);            // 67 on the right of ||
    TIDE_CHECK(r->rounds2 == 3);      // A loop's condition with &&
    TIDE_CHECK(r->operatorOrder == 12); // An operator that calls C, then the call after it
    TIDE_CHECK(r->spawnCalls == 2);   // The spawn's call ran once
    TIDE_CHECK(r->eachOrder == 12);   // A foreach's list
}
