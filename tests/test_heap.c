#include <string.h>

#include "tide/text.h"
#include "tide_test.h"

// A stand-in for a generated world: a heap, and fields that are its memory.
typedef struct fake_world {
    tide_text name;
    tide_text title;
    tide_heap heap;
} fake_world;

static fake_world world;

static bool reads(const tide_text t, const char *expected)
{
    const tide_str s = tide_text_view(t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

static tide_str text(const char *s)
{
    return tide_str_from_cstr(s);
}

static void start(void)
{
    memset(&world, 0, sizeof world);
    tide_text_use(&world.heap, &world, sizeof world, NULL, NULL, 0);
}

TIDE_TEST(heap_blocks_are_reused_in_order)
{
    start();
    const uint32_t a = tide_heap_alloc(&world.heap, 10);
    const uint32_t b = tide_heap_alloc(&world.heap, 10);
    TIDE_CHECK(a != 0 && b != 0 && a != b);
    tide_heap_release(&world.heap, a);
    TIDE_CHECK(tide_heap_alloc(&world.heap, 10) != a); // Not until the flush
    tide_heap_flush(&world.heap);
    const uint32_t c = tide_heap_alloc(&world.heap, 10);
    TIDE_CHECK(c == a);
    TIDE_CHECK(tide_heap_alloc(&world.heap, TIDE_HEAP_BYTES) == 0); // Too big: nothing, and it says so
    TIDE_CHECK(world.heap.failed == 1);
}

TIDE_TEST(heap_text_fields_own_their_text)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    tide_text_set(&world.name, text("cat"));
    TIDE_CHECK(reads(world.name, "cat"));
    TIDE_CHECK(strcmp(tide_text_read(&world.heap, world.name).ptr, "cat") == 0);

    // A copy made before a change keeps the old text until the code running is done.
    const tide_text copy = world.name;
    tide_text_set(&world.name, text("dog"));
    TIDE_CHECK(reads(world.name, "dog"));
    TIDE_CHECK(reads(copy, "cat"));

    // Outside the world, text borrows: a copy in the scratch area.
    tide_text local = {0};
    tide_text_set(&local, text("bird"));
    TIDE_CHECK(reads(local, "bird"));
    TIDE_CHECK(world.heap.pending != 0);
    tide_heap_flush(&world.heap);
    TIDE_CHECK(world.heap.pending == 0);

    // A value copied into the world takes its own copy of borrowed text.
    world.title = tide_text_temp(text("fish"));
    tide_text_own(&world.title);
    tide_scratch_reset(mark);
    TIDE_CHECK(reads(world.title, "fish"));

    tide_text_release(&world.title);
    TIDE_CHECK(world.title.at == 0);
    tide_text_set(&world.name, TIDE_STR_EMPTY);
    TIDE_CHECK(reads(world.name, ""));
    tide_heap_flush(&world.heap);
}

TIDE_TEST(heap_full_keeps_old_text)
{
    start();
    tide_text_set(&world.name, text("kept"));
    char big[TIDE_HEAP_BYTES / 2 + 1];
    memset(big, 'x', sizeof big);
    const tide_str huge = {big, (int32_t)sizeof big, (int32_t)sizeof big};
    tide_text_set(&world.title, huge); // Fits once
    tide_text_set(&world.name, huge);  // Not twice
    TIDE_CHECK(reads(world.name, "kept"));
    TIDE_CHECK(world.heap.failed > 0);
}
