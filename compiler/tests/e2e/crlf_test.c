#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static bool text_is(const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&world.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

// crlf.tide has Windows line endings: its text reads as written, with no '\r'.
TIDE_TEST(crlf_text_ends_where_the_line_does)
{
    tide_world_init(&world, 1.0f);
    uint32_t labels = 0;
    bool plain = false;
    bool formatted = false;
    for (uint32_t i = 0; i < world.entities.next_unused; i++) {
        const tide_entity e = {i, world.entities.slots[i].generation};
        const Label *label = tide_get_Label(&world, e);
        if (!label) continue;
        labels++;
        plain |= text_is(label->text, "line end");
        formatted |= text_is(label->text, "score 007 done");
    }
    TIDE_CHECK(labels == 2);
    TIDE_CHECK(plain);
    TIDE_CHECK(formatted);
}
