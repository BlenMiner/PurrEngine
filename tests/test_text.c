#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "tide/text.h"
#include "tide_test.h"

static bool is(const tide_str s, const char *expected)
{
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

static tide_str f(const float v, const int32_t format)
{
    return tide_str_add_float(TIDE_STR_EMPTY, v, format);
}

static tide_str i(const int32_t v, const int32_t format)
{
    return tide_str_add_int(TIDE_STR_EMPTY, v, format);
}

TIDE_TEST(text_floats_shortest)
{
    const uint32_t mark = tide_scratch_mark();
    TIDE_CHECK(is(f(0.0f, 0), "0"));
    TIDE_CHECK(is(f(-0.0f, 0), "0"));
    TIDE_CHECK(is(f(1.0f, 0), "1"));
    TIDE_CHECK(is(f(0.1f, 0), "0.1"));
    TIDE_CHECK(is(f(0.3f, 0), "0.3"));
    TIDE_CHECK(is(f(-2.5f, 0), "-2.5"));
    TIDE_CHECK(is(f(100.0f, 0), "100"));
    TIDE_CHECK(is(f(3.14159265f, 0), "3.1415927"));
    TIDE_CHECK(is(f(123456789.0f, 0), "123456790"));
    TIDE_CHECK(is(f(16777216.0f, 0), "16777216"));
    TIDE_CHECK(is(f(1e-6f, 0), "0.000001"));
    TIDE_CHECK(is(f(1e-7f, 0), "1E-7"));
    TIDE_CHECK(is(f(1e20f, 0), "100000000000000000000"));
    TIDE_CHECK(is(f(1e21f, 0), "1E+21"));
    TIDE_CHECK(is(f(FLT_MAX, 0), "3.4028235E+38"));
    TIDE_CHECK(is(f(FLT_MIN, 0), "1.1754944E-38"));
    TIDE_CHECK(is(f(1.4e-45f, 0), "1E-45"));
    TIDE_CHECK(is(f(0.2f + 0.1f, 0), "0.3")); // In float, it rounds to the same as 0.3
    tide_scratch_reset(mark);
}

TIDE_TEST(text_floats_read_back)
{
    // Every text reads back as the same float: a sweep over exponents and mantissas.
    const uint32_t mark = tide_scratch_mark();
    for (uint32_t bits = 1u; bits < 0x7F800000u; bits += 0x00F0F0F1u) {
        float v;
        memcpy(&v, &bits, sizeof v);
        const tide_str s = f(v, 0);
        char text[64];
        memcpy(text, s.ptr, (size_t)s.bytes);
        text[s.bytes] = '\0';
        const float back = strtof(text, NULL);
        TIDE_CHECK(memcmp(&back, &v, sizeof v) == 0);
        tide_scratch_reset(mark);
    }
}

TIDE_TEST(text_floats_fixed)
{
    const uint32_t mark = tide_scratch_mark();
    const int32_t f2 = TIDE_FORMAT('F', 2);
    TIDE_CHECK(is(f(1.5f, f2), "1.50"));
    TIDE_CHECK(is(f(2.675f, f2), "2.67")); // 2.67499995... as a float
    TIDE_CHECK(is(f(0.125f, f2), "0.13"));  // An exact half goes away from zero
    TIDE_CHECK(is(f(-0.125f, f2), "-0.13"));
    TIDE_CHECK(is(f(-0.001f, f2), "0.00"));
    TIDE_CHECK(is(f(1234.5f, TIDE_FORMAT('F', 0)), "1235"));
    TIDE_CHECK(is(f(0.5f, TIDE_FORMAT('F', 3)), "0.500"));
    TIDE_CHECK(is(f(1e10f, TIDE_FORMAT('F', 1)), "10000000000.0"));
    TIDE_CHECK(is(f(1.0f / 3.0f, TIDE_FORMAT('F', 9)), "0.333333343"));
    tide_scratch_reset(mark);
}

TIDE_TEST(text_ints)
{
    const uint32_t mark = tide_scratch_mark();
    TIDE_CHECK(is(i(0, 0), "0"));
    TIDE_CHECK(is(i(-42, 0), "-42"));
    TIDE_CHECK(is(i(INT32_MIN, 0), "-2147483648"));
    TIDE_CHECK(is(i(42, TIDE_FORMAT('D', 5)), "00042"));
    TIDE_CHECK(is(i(-42, TIDE_FORMAT('D', 5)), "-00042"));
    TIDE_CHECK(is(i(255, TIDE_FORMAT('X', 0)), "FF"));
    TIDE_CHECK(is(i(255, TIDE_FORMAT('x', 4)), "00ff"));
    TIDE_CHECK(is(i(-1, TIDE_FORMAT('X', 0)), "FFFFFFFF"));
    TIDE_CHECK(is(i(5, TIDE_FORMAT('F', 2)), "5.00"));
    tide_scratch_reset(mark);
}

TIDE_TEST(text_joining)
{
    const uint32_t mark = tide_scratch_mark();
    tide_str x = tide_str_add(TIDE_STR_EMPTY, tide_str_from_cstr("ab"));
    const tide_str y = x; // Grows in place below: y keeps its own length
    x = tide_str_add_cstr(x, "c");
    x = tide_str_add_int(x, 7, 0);
    TIDE_CHECK(is(x, "abc7") && x.chars == 4);
    TIDE_CHECK(is(y, "ab"));
    TIDE_CHECK(strcmp(tide_str_c(y), "ab") == 0); // A copy: the byte after it isn't a NUL anymore
    TIDE_CHECK(strcmp(tide_str_c(x), "abc7") == 0);
    TIDE_CHECK(is(tide_str_add_bool(TIDE_STR_EMPTY, false), "false"));
    TIDE_CHECK(is(tide_str_add_f2(TIDE_STR_EMPTY, tide_f2(1.0f, 0.5f), 0), "(1, 0.5)"));
    TIDE_CHECK(is(tide_str_add_color(TIDE_STR_EMPTY, TIDE_COLOR_RED, 0), "RGBA(1, 0, 0, 1)"));
    TIDE_CHECK(is(tide_str_add_entity(TIDE_STR_EMPTY, (tide_entity){3, 1}, false), "Entity(3:1)"));
    TIDE_CHECK(is(tide_str_add_player(TIDE_STR_EMPTY, tide_player_from_index(0)), "PlayerID(0)"));
    tide_scratch_reset(mark);
}

TIDE_TEST(text_characters)
{
    const uint32_t mark = tide_scratch_mark();
    const tide_str word = tide_str_from_cstr("h\xC3\xA9llo"); // héllo
    TIDE_CHECK(word.chars == 5 && word.bytes == 6);
    TIDE_CHECK(is(tide_str_substring(word, 1, 2), "\xC3\xA9l"));
    TIDE_CHECK(is(tide_str_substring(word, 3, 99), "lo"));
    TIDE_CHECK(is(tide_str_substring(word, -5, 1), "h"));
    TIDE_CHECK(is(tide_str_substring_from(word, 9), ""));
    TIDE_CHECK(tide_str_index_of(word, tide_str_from_cstr("l")) == 2);
    TIDE_CHECK(tide_str_index_of(word, tide_str_from_cstr("z")) == -1);
    TIDE_CHECK(tide_str_contains(word, tide_str_from_cstr("\xC3\xA9")));
    TIDE_CHECK(tide_str_starts_with(word, tide_str_from_cstr("h")));
    TIDE_CHECK(tide_str_ends_with(word, tide_str_from_cstr("lo")));
    TIDE_CHECK(is(tide_str_to_upper(word), "H\xC3\xA9LLO"));
    TIDE_CHECK(is(tide_str_trim(tide_str_from_cstr("  hi \n")), "hi"));
    TIDE_CHECK(is(tide_str_replace(tide_str_from_cstr("a-b-c"), tide_str_from_cstr("-"), tide_str_from_cstr("+=")), "a+=b+=c"));
    TIDE_CHECK(tide_str_eq(tide_str_from_cstr("cat"), tide_str_add_cstr(TIDE_STR_EMPTY, "cat")));
    tide_scratch_reset(mark);
}
