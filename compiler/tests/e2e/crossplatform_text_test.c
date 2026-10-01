#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

// The expected hash was produced natively; WebAssembly (and every other
// platform) must match it bit for bit. The world's hash covers all it holds,
// its heap's pages included, so this covers the heap's allocations and the
// text numbers become.

static tide_world world;

TIDE_TEST(crossplatform_text_and_lists)
{
    tide_world_init(&world, 1.0f / 60.0f);
    for (int i = 0; i < 300; i++) tide_world_tick(&world);

    const uint64_t h = tide_world_hash(&world);
    const uint64_t expected = 0xE3682CC030246AB5ull;
    if (h != expected) {
        const tide_str last = tide_text_read(&world.heap, world.Log.last);
        printf("    world: hash is 0x%016" PRIX64 ", expected 0x%016" PRIX64 "\n", h, expected);
        printf("    last text: %.*s\n", (int)last.bytes, last.ptr);
    }
    TIDE_CHECK(h == expected);
    tide_world_free(&world);
}
