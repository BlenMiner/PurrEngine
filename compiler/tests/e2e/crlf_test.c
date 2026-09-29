#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

static bool text_is(const purr_text t, const char *expected)
{
    const purr_str s = purr_text_read(&world.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

// crlf.purr has Windows line endings: its text reads as written, with no '\r'.
PURR_TEST(crlf_text_ends_where_the_line_does)
{
    purr_world_init(&world, 1.0f);
    uint32_t labels = 0;
    bool plain = false;
    bool formatted = false;
    for (uint32_t i = 0; i < world.entities.next_unused; i++) {
        const purr_entity e = {i, world.entities.slots[i].generation};
        const Label *label = purr_get_Label(&world, e);
        if (!label) continue;
        labels++;
        plain |= text_is(label->text, "line end");
        formatted |= text_is(label->text, "score 007 done");
    }
    PURR_CHECK(labels == 2);
    PURR_CHECK(plain);
    PURR_CHECK(formatted);
}
