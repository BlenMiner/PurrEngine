#include <string.h>

#include "tide/list.h"
#include "tide/text.h"
#include "tide_test.h"

// The scratch area grows as it needs (see tide/text.h): what code makes along
// the way is whole however big it is, and the same whatever else was made
// before it.

// A stand-in for a generated world: a list that's part of it, and its heap.
typedef struct scratch_world {
    tide_list numbers;
    tide_heap heap;
} scratch_world;

static scratch_world world;

#define BIG_COUNT 300000 // Ints: 1.2 MB, more than a slot of the area (TIDE_SCRATCH_BYTES)

static uint32_t value_at(const int32_t i)
{
    return (uint32_t)i * 2654435761u; // Every byte of it changes from one element to the next
}

// The world's list, filled with BIG_COUNT elements
static void start(void)
{
    tide_heap_free(&world.heap);
    memset(&world, 0, sizeof world);
    tide_text_use(&world.heap, NULL);
    for (int32_t i = 0; i < BIG_COUNT; i++) {
        uint32_t *slot = tide_list_add(&world.numbers, sizeof *slot, TIDE_IN_MATCH); // A world's list grows with its heap
        *slot = value_at(i);
    }
}

// How many of `l`'s first `count` elements aren't value_at(i).
static int32_t wrong(const tide_list l, const int32_t count)
{
    int32_t n = 0;
    for (int32_t i = 0; i < count; i++) {
        const uint32_t *p = tide_list_at(l, i, sizeof *p);
        n += !p || *p != value_at(i);
    }
    return n;
}

static bool is(const tide_str s, const char *expected)
{
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0 && s.ptr[s.bytes] == '\0';
}

// Whether `s` is `count` copies of the 16 bytes "0123456789abcdef", then `tail`.
static bool is_pattern(const tide_str s, const int32_t count, const char *tail)
{
    if (s.bytes != count * 16 + (int32_t)strlen(tail) || s.chars != s.bytes) return false;
    bool same = true;
    for (int32_t i = 0; i < count; i++) same &= memcmp(s.ptr + (size_t)i * 16u, "0123456789abcdef", 16) == 0;
    return same && memcmp(s.ptr + (size_t)count * 16u, tail, strlen(tail)) == 0 && s.ptr[s.bytes] == '\0';
}

// A list read as a value is whole, however big: tide_list_copy, tide_list_from
// and growing a copy all take more than a slot of the area.
TIDE_TEST(scratch_big_lists_are_whole)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    tide_list copy = tide_list_copy(world.numbers, sizeof(uint32_t));
    TIDE_CHECK(copy.at >> 30 == TIDE_IN_SCRATCH);
    TIDE_CHECK(tide_list_count(copy) == BIG_COUNT);
    TIDE_CHECK(wrong(copy, BIG_COUNT) == 0);
    TIDE_CHECK(tide_scratch_mark() - mark > BIG_COUNT * sizeof(uint32_t));

    // Its own: changing it leaves the world's
    *(uint32_t *)tide_list_at_mut(copy, 0, sizeof(uint32_t)) = 7u;
    TIDE_CHECK(wrong(world.numbers, BIG_COUNT) == 0);
    TIDE_CHECK(wrong(copy, BIG_COUNT) == 1);

    // Growing it past its block moves it whole
    for (int32_t i = BIG_COUNT; i < BIG_COUNT + 100; i++) {
        uint32_t *slot = tide_list_add(&copy, sizeof *slot, TIDE_IN_SCRATCH);
        TIDE_REQUIRE(slot != NULL);
        *slot = value_at(i);
    }
    *(uint32_t *)tide_list_at_mut(copy, 0, sizeof(uint32_t)) = value_at(0);
    TIDE_CHECK(tide_list_count(copy) == BIG_COUNT + 100 && wrong(copy, BIG_COUNT + 100) == 0);

    // [a, b, c] of that size, from memory of the area's own
    uint32_t *raw = tide_scratch_memory(BIG_COUNT * sizeof *raw);
    TIDE_CHECK(((uintptr_t)raw & 15u) == 0);
    for (int32_t i = 0; i < BIG_COUNT; i++) raw[i] = value_at(i);
    const tide_list made = tide_list_from(raw, BIG_COUNT, sizeof *raw);
    TIDE_CHECK(made.at >> 30 == TIDE_IN_SCRATCH && tide_list_count(made) == BIG_COUNT && wrong(made, BIG_COUNT) == 0);
    TIDE_CHECK(wrong(copy, BIG_COUNT + 100) == 0); // Still there

    tide_scratch_reset(mark);
    TIDE_CHECK(tide_scratch_mark() == mark);
    tide_heap_free(&world.heap);
}

// Text grows past a slot: in place while its piece has room, then in a copy,
// and it's whole either way.
TIDE_TEST(scratch_big_text_is_whole)
{
    const uint32_t mark = tide_scratch_mark();
    const int32_t n = 100000; // 16 bytes each: 1.6 MB
    tide_str s = TIDE_STR_EMPTY;
    for (int32_t i = 0; i < n; i++) s = tide_str_add_cstr(s, "0123456789abcdef");
    TIDE_CHECK(is_pattern(s, n, ""));

    // Newer text in the way: the next growth is a copy, of all of it
    const tide_str b = tide_str_add_int(TIDE_STR_EMPTY, 12345, 0);
    s = tide_str_add_cstr(s, "end");
    TIDE_CHECK(is_pattern(s, n, "end"));
    TIDE_CHECK(is(b, "12345"));
    TIDE_CHECK(tide_str_ends_with(s, tide_str_from_cstr("fend")));
    TIDE_CHECK(is(tide_str_substring(s, (int32_t)TIDE_SCRATCH_BYTES - 6, 12), "abcdef012345")); // Across the first slot's end

    // A field's copy of it (a struct literal's), read back
    const tide_text temp = tide_text_temp(s);
    const tide_str back = tide_text_view(temp);
    TIDE_CHECK(temp.at >> 30 == TIDE_IN_SCRATCH && back.ptr != s.ptr);
    TIDE_CHECK(back.bytes == s.bytes && back.chars == s.chars && memcmp(back.ptr, s.ptr, (size_t)s.bytes) == 0);
    const tide_str upper = tide_str_to_upper(back); // A copy of all of it, changed
    TIDE_CHECK(upper.bytes == s.bytes && !tide_str_eq(upper, s) && tide_str_eq(tide_str_to_lower(upper), s));
    tide_scratch_reset(mark);
    TIDE_CHECK(tide_scratch_mark() == mark);
}

// What's made is the same whatever is in the area before it: only where it
// goes changes. The copy is made after each of several amounts, around a
// slot's end and past it.
TIDE_TEST(scratch_results_are_the_same_whatever_came_first)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    const uint32_t befores[] = {0u, TIDE_SCRATCH_BYTES - 100u, TIDE_SCRATCH_BYTES - 17u, TIDE_SCRATCH_BYTES + 5u,
                                2u * TIDE_SCRATCH_BYTES + TIDE_SCRATCH_BYTES / 2u};
    for (size_t k = 0; k < sizeof befores / sizeof befores[0]; k++) {
        char *filler = tide_scratch_memory(befores[k]);
        memset(filler, 'f', befores[k]);
        const tide_list copy = tide_list_copy(world.numbers, sizeof(uint32_t));
        TIDE_CHECK(tide_list_count(copy) == BIG_COUNT && wrong(copy, BIG_COUNT) == 0);
        tide_str s = TIDE_STR_EMPTY;
        for (int32_t i = 0; i < 70000; i++) s = tide_str_add_cstr(s, "0123456789abcdef");
        TIDE_CHECK(is_pattern(s, 70000, ""));
        TIDE_CHECK(wrong(copy, BIG_COUNT) == 0);
        bool same = true;
        for (uint32_t i = 0; i < befores[k]; i++) same &= filler[i] == 'f';
        TIDE_CHECK(same);
        tide_scratch_reset(mark);
    }
    tide_heap_free(&world.heap);
}

// Going back to a mark lets the pieces made since go, so the area doesn't grow
// with each big thing, and the next one goes where the first went: where
// something goes depends on the mark, never on what came and went before.
TIDE_TEST(scratch_pieces_go_back_with_the_mark)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    uint32_t first = 0;
    for (int round = 0; round < 50; round++) {
        const tide_list copy = tide_list_copy(world.numbers, sizeof(uint32_t));
        if (round == 0) first = copy.at;
        TIDE_CHECK(copy.at == first);
        TIDE_CHECK(tide_list_count(copy) == BIG_COUNT && wrong(copy, BIG_COUNT) == 0);
        tide_scratch_reset(mark);
        TIDE_CHECK(tide_scratch_mark() == mark);
    }

    // A mark taken after a piece was made keeps what was in it before
    const tide_list copy = tide_list_copy(world.numbers, sizeof(uint32_t));
    const uint32_t inner = tide_scratch_mark();
    char *more = tide_scratch_memory(2u * TIDE_SCRATCH_BYTES);
    memset(more, 1, 2u * TIDE_SCRATCH_BYTES);
    tide_scratch_reset(inner);
    TIDE_CHECK(tide_scratch_mark() == inner);
    TIDE_CHECK(wrong(copy, BIG_COUNT) == 0);
    const uint32_t most = 3u * TIDE_SCRATCH_BYTES - 32u; // A piece of its own again, with 32 bytes to spare
    char *again = tide_scratch_memory(most);
    memset(again, 2, most);
    const uint32_t after = tide_scratch_mark();
    TIDE_CHECK(((uintptr_t)again & 15u) == 0 && wrong(copy, BIG_COUNT) == 0);
    char *small = tide_scratch_memory(5u); // Right after it, in the same piece
    TIDE_CHECK(small == again + most);
    TIDE_CHECK(tide_scratch_mark() == after + 5u); // No byte more than asked
    tide_scratch_reset(mark);
    TIDE_CHECK(tide_scratch_mark() == mark);

    // The same requests land at the same offsets whether or not a bigger piece
    // was let go before them: a spare of three slots that a copy of two uses
    // takes only two, so what comes after it goes where it went the first time
    uint32_t marks[2][2];
    for (int round = 0; round < 2; round++) {
        const tide_list two = tide_list_copy(world.numbers, sizeof(uint32_t)); // Two slots
        marks[round][0] = tide_scratch_mark();
        char *three = tide_scratch_memory(most); // Three: a piece of its own after it
        memset(three, 3, most);
        marks[round][1] = tide_scratch_mark();
        TIDE_CHECK(wrong(two, BIG_COUNT) == 0);
        tide_scratch_reset(mark); // The three-slot piece is the spare: the next copy is made of it
    }
    TIDE_CHECK(marks[0][0] == marks[1][0] && marks[0][1] == marks[1][1]);
    tide_heap_free(&world.heap);
}
