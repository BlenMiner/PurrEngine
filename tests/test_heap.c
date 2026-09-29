#include <string.h>

#include "purr/text.h"
#include "purr_test.h"

// A stand-in for a generated world: a heap, and fields that are its memory.
typedef struct fake_world {
    purr_text name;
    purr_text title;
    purr_heap heap;
} fake_world;

static fake_world world;

static bool reads(const purr_text t, const char *expected)
{
    const purr_str s = purr_text_view(t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

static purr_str text(const char *s)
{
    return purr_str_from_cstr(s);
}

static void start(void)
{
    memset(&world, 0, sizeof world);
    purr_text_use(&world.heap, &world, sizeof world, NULL, NULL, 0);
}

PURR_TEST(heap_blocks_are_reused_in_order)
{
    start();
    const uint32_t a = purr_heap_alloc(&world.heap, 10);
    const uint32_t b = purr_heap_alloc(&world.heap, 10);
    PURR_CHECK(a != 0 && b != 0 && a != b);
    purr_heap_release(&world.heap, a);
    PURR_CHECK(purr_heap_alloc(&world.heap, 10) != a); // Not until the flush
    purr_heap_flush(&world.heap);
    const uint32_t c = purr_heap_alloc(&world.heap, 10);
    PURR_CHECK(c == a);
    PURR_CHECK(purr_heap_alloc(&world.heap, PURR_HEAP_BYTES) == 0); // Too big: nothing, and it says so
    PURR_CHECK(world.heap.failed == 1);
}

PURR_TEST(heap_text_fields_own_their_text)
{
    start();
    const uint32_t mark = purr_scratch_mark();
    purr_text_set(&world.name, text("cat"));
    PURR_CHECK(reads(world.name, "cat"));
    PURR_CHECK(strcmp(purr_text_read(&world.heap, world.name).ptr, "cat") == 0);

    // A copy made before a change keeps the old text until the code running is done.
    const purr_text copy = world.name;
    purr_text_set(&world.name, text("dog"));
    PURR_CHECK(reads(world.name, "dog"));
    PURR_CHECK(reads(copy, "cat"));

    // Outside the world, text borrows: a copy in the scratch area.
    purr_text local = {0};
    purr_text_set(&local, text("bird"));
    PURR_CHECK(reads(local, "bird"));
    PURR_CHECK(world.heap.pending != 0);
    purr_heap_flush(&world.heap);
    PURR_CHECK(world.heap.pending == 0);

    // A value copied into the world takes its own copy of borrowed text.
    world.title = purr_text_temp(text("fish"));
    purr_text_own(&world.title);
    purr_scratch_reset(mark);
    PURR_CHECK(reads(world.title, "fish"));

    purr_text_release(&world.title);
    PURR_CHECK(world.title.at == 0);
    purr_text_set(&world.name, PURR_STR_EMPTY);
    PURR_CHECK(reads(world.name, ""));
    purr_heap_flush(&world.heap);
}

PURR_TEST(heap_full_keeps_old_text)
{
    start();
    purr_text_set(&world.name, text("kept"));
    char big[PURR_HEAP_BYTES / 2 + 1];
    memset(big, 'x', sizeof big);
    const purr_str huge = {big, (int32_t)sizeof big, (int32_t)sizeof big};
    purr_text_set(&world.title, huge); // Fits once
    purr_text_set(&world.name, huge);  // Not twice
    PURR_CHECK(reads(world.name, "kept"));
    PURR_CHECK(world.heap.failed > 0);
}
