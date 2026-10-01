#include <stdlib.h>
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
    tide_heap_free(&world.heap);
    memset(&world, 0, sizeof world);
    tide_text_use(&world.heap, NULL);
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
}

TIDE_TEST(heap_text_fields_own_their_text)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    tide_text_set(&world.name, text("cat"), TIDE_IN_MATCH);
    TIDE_CHECK(reads(world.name, "cat"));
    TIDE_CHECK(strcmp(tide_text_read(&world.heap, world.name).ptr, "cat") == 0);

    // A copy made before a change keeps the old text until the code running is done.
    const tide_text copy = world.name;
    tide_text_set(&world.name, text("dog"), TIDE_IN_MATCH);
    TIDE_CHECK(reads(world.name, "dog"));
    TIDE_CHECK(reads(copy, "cat"));

    // Outside the world, text borrows: a copy in the scratch area.
    tide_text local = {0};
    tide_text_set(&local, text("bird"), TIDE_IN_SCRATCH);
    TIDE_CHECK(reads(local, "bird"));
    TIDE_CHECK(world.heap.pending != 0);
    tide_heap_flush(&world.heap);
    TIDE_CHECK(world.heap.pending == 0);

    // A value copied into the world takes its own copy of borrowed text.
    world.title = tide_text_temp(text("fish"));
    tide_text_own(&world.title, TIDE_IN_MATCH);
    tide_scratch_reset(mark);
    TIDE_CHECK(reads(world.title, "fish"));

    tide_text_release(&world.title, TIDE_IN_MATCH);
    TIDE_CHECK(world.title.at == 0);
    tide_text_set(&world.name, TIDE_STR_EMPTY, TIDE_IN_MATCH);
    TIDE_CHECK(reads(world.name, ""));
    tide_heap_flush(&world.heap);
}

// The heap grows as it needs: text bigger than a page has one of its own.
TIDE_TEST(heap_grows_past_its_pages)
{
    start();
    tide_text_set(&world.name, text("kept"), TIDE_IN_MATCH);
    const uint32_t size = 3u << TIDE_HEAP_PAGE_SHIFT;
    char *big = malloc(size);
    memset(big, 'x', size);
    const tide_str huge = {big, (int32_t)size, (int32_t)size};
    tide_text_set(&world.title, huge, TIDE_IN_MATCH);
    TIDE_CHECK(tide_text_view(world.title).bytes == (int32_t)size);
    TIDE_CHECK(reads(world.name, "kept"));
    TIDE_CHECK(world.heap.pages >= 5); // The first page, then the big block's, as big as it needs
    for (int i = 0; i < 2000; i++) tide_heap_alloc(&world.heap, 100); // Page after page
    TIDE_CHECK(tide_text_view(world.title).ptr[size - 1u] == 'x');
    free(big);
}

// A snapshot shares the heap's pages: text written after it, in either,
// leaves the other as it was, and both hash as what they hold.
TIDE_TEST(heap_snapshots_share_until_changed)
{
    start();
    tide_text_set(&world.name, text("cat"), TIDE_IN_MATCH);
    fake_world snapshot = {0};
    snapshot.name = world.name;
    tide_heap_copy(&snapshot.heap, &world.heap);
    TIDE_CHECK(snapshot.heap.page[0] == world.heap.page[0]);
    TIDE_CHECK(tide_heap_hash(1, &snapshot.heap) == tide_heap_hash(1, &world.heap));

    tide_text_set(&world.name, text("dog"), TIDE_IN_MATCH);
    tide_heap_flush(&world.heap);
    TIDE_CHECK(snapshot.heap.page[0] != world.heap.page[0]);
    TIDE_CHECK(strcmp(tide_text_read(&snapshot.heap, snapshot.name).ptr, "cat") == 0);
    TIDE_CHECK(strcmp(tide_text_read(&world.heap, world.name).ptr, "dog") == 0);
    TIDE_CHECK(tide_heap_hash(1, &snapshot.heap) != tide_heap_hash(1, &world.heap));

    // Packed and unpacked: the same heap
    const uint32_t size = tide_heap_packed_size(&world.heap);
    uint8_t *bytes = malloc(size);
    tide_writer w = {bytes, size, 0, false};
    tide_heap_pack(&world.heap, &w);
    TIDE_CHECK(!w.overflow && w.size == size);
    tide_heap back = {0};
    tide_reader r = {bytes, size, 0, false};
    TIDE_REQUIRE(tide_heap_unpack(&back, &r));
    TIDE_CHECK(tide_heap_hash(1, &back) == tide_heap_hash(1, &world.heap));
    TIDE_CHECK(strcmp(tide_text_read(&back, world.name).ptr, "dog") == 0);
    free(bytes);
    tide_heap_free(&back);
    tide_heap_free(&snapshot.heap);
}
