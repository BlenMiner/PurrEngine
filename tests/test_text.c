#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "purr/text.h"
#include "purr_test.h"

static bool is(const purr_str s, const char *expected)
{
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

static purr_str f(const float v, const int32_t format)
{
    return purr_str_add_float(PURR_STR_EMPTY, v, format);
}

static purr_str i(const int32_t v, const int32_t format)
{
    return purr_str_add_int(PURR_STR_EMPTY, v, format);
}

PURR_TEST(text_floats_shortest)
{
    const uint32_t mark = purr_scratch_mark();
    PURR_CHECK(is(f(0.0f, 0), "0"));
    PURR_CHECK(is(f(-0.0f, 0), "0"));
    PURR_CHECK(is(f(1.0f, 0), "1"));
    PURR_CHECK(is(f(0.1f, 0), "0.1"));
    PURR_CHECK(is(f(0.3f, 0), "0.3"));
    PURR_CHECK(is(f(-2.5f, 0), "-2.5"));
    PURR_CHECK(is(f(100.0f, 0), "100"));
    PURR_CHECK(is(f(3.14159265f, 0), "3.1415927"));
    PURR_CHECK(is(f(123456789.0f, 0), "123456790"));
    PURR_CHECK(is(f(16777216.0f, 0), "16777216"));
    PURR_CHECK(is(f(1e-6f, 0), "0.000001"));
    PURR_CHECK(is(f(1e-7f, 0), "1E-7"));
    PURR_CHECK(is(f(1e20f, 0), "100000000000000000000"));
    PURR_CHECK(is(f(1e21f, 0), "1E+21"));
    PURR_CHECK(is(f(FLT_MAX, 0), "3.4028235E+38"));
    PURR_CHECK(is(f(FLT_MIN, 0), "1.1754944E-38"));
    PURR_CHECK(is(f(1.4e-45f, 0), "1E-45"));
    PURR_CHECK(is(f(0.2f + 0.1f, 0), "0.3")); // In float, it rounds to the same as 0.3
    purr_scratch_reset(mark);
}

PURR_TEST(text_floats_read_back)
{
    // Every text reads back as the same float: a sweep over exponents and mantissas.
    const uint32_t mark = purr_scratch_mark();
    for (uint32_t bits = 1u; bits < 0x7F800000u; bits += 0x00F0F0F1u) {
        float v;
        memcpy(&v, &bits, sizeof v);
        const purr_str s = f(v, 0);
        char text[64];
        memcpy(text, s.ptr, (size_t)s.bytes);
        text[s.bytes] = '\0';
        const float back = strtof(text, NULL);
        PURR_CHECK(memcmp(&back, &v, sizeof v) == 0);
        purr_scratch_reset(mark);
    }
}

PURR_TEST(text_floats_fixed)
{
    const uint32_t mark = purr_scratch_mark();
    const int32_t f2 = PURR_FORMAT('F', 2);
    PURR_CHECK(is(f(1.5f, f2), "1.50"));
    PURR_CHECK(is(f(2.675f, f2), "2.67")); // 2.67499995... as a float
    PURR_CHECK(is(f(0.125f, f2), "0.13"));  // An exact half goes away from zero
    PURR_CHECK(is(f(-0.125f, f2), "-0.13"));
    PURR_CHECK(is(f(-0.001f, f2), "0.00"));
    PURR_CHECK(is(f(1234.5f, PURR_FORMAT('F', 0)), "1235"));
    PURR_CHECK(is(f(0.5f, PURR_FORMAT('F', 3)), "0.500"));
    PURR_CHECK(is(f(1e10f, PURR_FORMAT('F', 1)), "10000000000.0"));
    PURR_CHECK(is(f(1.0f / 3.0f, PURR_FORMAT('F', 9)), "0.333333343"));
    purr_scratch_reset(mark);
}

PURR_TEST(text_ints)
{
    const uint32_t mark = purr_scratch_mark();
    PURR_CHECK(is(i(0, 0), "0"));
    PURR_CHECK(is(i(-42, 0), "-42"));
    PURR_CHECK(is(i(INT32_MIN, 0), "-2147483648"));
    PURR_CHECK(is(i(42, PURR_FORMAT('D', 5)), "00042"));
    PURR_CHECK(is(i(-42, PURR_FORMAT('D', 5)), "-00042"));
    PURR_CHECK(is(i(255, PURR_FORMAT('X', 0)), "FF"));
    PURR_CHECK(is(i(255, PURR_FORMAT('x', 4)), "00ff"));
    PURR_CHECK(is(i(-1, PURR_FORMAT('X', 0)), "FFFFFFFF"));
    PURR_CHECK(is(i(5, PURR_FORMAT('F', 2)), "5.00"));
    purr_scratch_reset(mark);
}

PURR_TEST(text_joining)
{
    const uint32_t mark = purr_scratch_mark();
    purr_str x = purr_str_add(PURR_STR_EMPTY, purr_str_from_cstr("ab"));
    const purr_str y = x; // Grows in place below: y keeps its own length
    x = purr_str_add_cstr(x, "c");
    x = purr_str_add_int(x, 7, 0);
    PURR_CHECK(is(x, "abc7") && x.chars == 4);
    PURR_CHECK(is(y, "ab"));
    PURR_CHECK(strcmp(purr_str_c(y), "ab") == 0); // A copy: the byte after it isn't a NUL anymore
    PURR_CHECK(strcmp(purr_str_c(x), "abc7") == 0);
    PURR_CHECK(is(purr_str_add_bool(PURR_STR_EMPTY, false), "false"));
    PURR_CHECK(is(purr_str_add_f2(PURR_STR_EMPTY, purr_f2(1.0f, 0.5f), 0), "(1, 0.5)"));
    PURR_CHECK(is(purr_str_add_color(PURR_STR_EMPTY, PURR_COLOR_RED, 0), "RGBA(1, 0, 0, 1)"));
    PURR_CHECK(is(purr_str_add_entity(PURR_STR_EMPTY, (purr_entity){3, 1}, false), "Entity(3:1)"));
    PURR_CHECK(is(purr_str_add_player(PURR_STR_EMPTY, purr_player_from_index(0)), "PlayerID(0)"));
    purr_scratch_reset(mark);
}

PURR_TEST(text_characters)
{
    const uint32_t mark = purr_scratch_mark();
    const purr_str word = purr_str_from_cstr("h\xC3\xA9llo"); // héllo
    PURR_CHECK(word.chars == 5 && word.bytes == 6);
    PURR_CHECK(is(purr_str_substring(word, 1, 2), "\xC3\xA9l"));
    PURR_CHECK(is(purr_str_substring(word, 3, 99), "lo"));
    PURR_CHECK(is(purr_str_substring(word, -5, 1), "h"));
    PURR_CHECK(is(purr_str_substring_from(word, 9), ""));
    PURR_CHECK(purr_str_index_of(word, purr_str_from_cstr("l")) == 2);
    PURR_CHECK(purr_str_index_of(word, purr_str_from_cstr("z")) == -1);
    PURR_CHECK(purr_str_contains(word, purr_str_from_cstr("\xC3\xA9")));
    PURR_CHECK(purr_str_starts_with(word, purr_str_from_cstr("h")));
    PURR_CHECK(purr_str_ends_with(word, purr_str_from_cstr("lo")));
    PURR_CHECK(is(purr_str_to_upper(word), "H\xC3\xA9LLO"));
    PURR_CHECK(is(purr_str_trim(purr_str_from_cstr("  hi \n")), "hi"));
    PURR_CHECK(is(purr_str_replace(purr_str_from_cstr("a-b-c"), purr_str_from_cstr("-"), purr_str_from_cstr("+=")), "a+=b+=c"));
    PURR_CHECK(purr_str_eq(purr_str_from_cstr("cat"), purr_str_add_cstr(PURR_STR_EMPTY, "cat")));
    purr_scratch_reset(mark);
}
