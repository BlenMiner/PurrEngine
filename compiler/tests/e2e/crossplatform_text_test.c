#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

// The expected hash was produced natively; WebAssembly (and every other
// platform) must match it bit for bit. Every byte of the world is hashed, so
// this covers the heap's allocations and the text numbers become.

static tide_world world;

TIDE_TEST(crossplatform_text_and_lists)
{
    tide_world_init(&world, 1.0f / 60.0f);
    for (int i = 0; i < 300; i++) tide_world_tick(&world);
    TIDE_CHECK(world.heap.failed == 0);

    const unsigned char *bytes = (const unsigned char *)&world;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < sizeof world; i++) h = (h ^ bytes[i]) * 0x100000001B3ull;
    const uint64_t expected = 0x13811EBFEAA65549ull;
    if (h != expected) {
        const tide_str last = tide_text_read(&world.heap, world.Log.last);
        printf("    world: hash is 0x%016" PRIX64 ", expected 0x%016" PRIX64 "\n", h, expected);
        printf("    last text: %.*s\n", (int)last.bytes, last.ptr);
    }
    TIDE_CHECK(h == expected);
}
