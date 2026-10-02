#include <stdint.h>

#include "tide/net.h"
#include "tide_test.h"

// Varints in bits, as inputs pack ints: small numbers take little, every
// value comes back as it was, and bad input can't run on.

static uint32_t varint_bits(const uint32_t v)
{
    uint8_t data[8] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_varint(&b, v);
    return b.bit;
}

TIDE_TEST(bits_varints_are_short_for_small_numbers)
{
    TIDE_CHECK(varint_bits(0) == 8);
    TIDE_CHECK(varint_bits(127) == 8);
    TIDE_CHECK(varint_bits(128) == 16);
    TIDE_CHECK(varint_bits(16383) == 16);
    TIDE_CHECK(varint_bits(16384) == 24);
    TIDE_CHECK(varint_bits(UINT32_MAX) == 40);
    TIDE_CHECK(varint_bits(tide_zigzag(-1)) == 8); // Small negative numbers too
    TIDE_CHECK(varint_bits(tide_zigzag(-64)) == 8);
    TIDE_CHECK(varint_bits(tide_zigzag(-65)) == 16);
}

TIDE_TEST(bits_varints_come_back_as_they_were)
{
    static const uint32_t values[] = {0, 1, 127, 128, 255, 16383, 16384, 2097151, 2097152, 268435455, 268435456,
                                      0x7FFFFFFFu, 0x80000000u, UINT32_MAX};
    static const int32_t signs[] = {0, 1, -1, 63, -64, 64, -65, 1000, -1000, INT32_MAX, INT32_MIN};
    uint8_t data[256] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_bool(&b, true); // Not on a byte boundary
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) tide_bits_put_varint(&b, values[i]);
    for (size_t i = 0; i < sizeof signs / sizeof signs[0]; i++) tide_bits_put_varint(&b, tide_zigzag(signs[i]));
    tide_bits_put_changed_varint(&b, 5, 5);
    tide_bits_put_changed_varint(&b, 300, 5);
    const uint32_t size = tide_bits_end(&b);
    TIDE_REQUIRE(size > 0 && !b.overflow);

    tide_bits r = {data, size, 0, false};
    TIDE_CHECK(tide_bits_get_bool(&r));
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) TIDE_CHECK(tide_bits_get_varint(&r) == values[i]);
    for (size_t i = 0; i < sizeof signs / sizeof signs[0]; i++) {
        TIDE_CHECK(tide_unzigzag(tide_bits_get_varint(&r)) == signs[i]);
    }
    TIDE_CHECK(!tide_bits_get_bool(&r)); // Unchanged: one bit
    TIDE_CHECK(tide_bits_get_bool(&r) && tide_bits_get_varint(&r) == 300);
    TIDE_CHECK(!r.overflow);
}

TIDE_TEST(bits_varints_stop_on_bad_input)
{
    // Every group says another follows: reading takes five, and goes no further
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = 0xFF;
    tide_bits r = {data, sizeof data, 0, false};
    (void)tide_bits_get_varint(&r);
    TIDE_CHECK(r.bit == 40 && !r.overflow);

    // Cut short: past the end, as any read there
    tide_bits cut = {data, 2, 0, false};
    TIDE_CHECK(tide_bits_get_varint(&cut) == 0 || cut.overflow);
    TIDE_CHECK(cut.overflow);
}

// Deltas from the value before: ints by how much they moved, floats by their
// XOR with what they were. Both come back bit for bit, whatever the values.

static uint32_t difference_bits(const uint32_t now, const uint32_t was)
{
    uint8_t data[8] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_changed_difference(&b, now, was);
    return b.bit;
}

static uint32_t xor_bits(const float now, const float was)
{
    uint8_t data[8] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_changed_xor(&b, tide_f32_bits(now), tide_f32_bits(was));
    return b.bit;
}

TIDE_TEST(bits_deltas_are_short_for_small_changes)
{
    TIDE_CHECK(difference_bits(7, 7) == 1);
    TIDE_CHECK(difference_bits(1000001, 1000000) == 9); // A big number moving a little
    TIDE_CHECK(difference_bits((uint32_t)-5, 3) == 9);
    TIDE_CHECK(difference_bits((uint32_t)INT32_MIN, (uint32_t)INT32_MAX) == 9); // Wraps around
    TIDE_CHECK(xor_bits(1.5f, 1.5f) == 1);
    TIDE_CHECK(xor_bits(1.0f, 0.5f) == 2 + 10 + 1);  // Only the exponent's last bit
    TIDE_CHECK(xor_bits(-1.0f, 1.0f) == 2 + 10 + 1); // Only the sign
    TIDE_CHECK(xor_bits(0.1f, 0.7f) == 2 + 32);      // Nothing in common: its own bits
}

TIDE_TEST(bits_deltas_come_back_as_they_were)
{
    static const uint32_t ints[] = {0, 1, (uint32_t)-1, 127, 128, 1000000, 0x7FFFFFFFu, 0x80000000u, UINT32_MAX};
    static const uint32_t floats[] = {
        0x00000000u, 0x80000000u, // 0 and -0
        0x3F800000u, 0xBF800000u, 0x3F800001u, 0x7F7FFFFFu, // 1, -1, just past 1, the largest
        0x00000001u, 0x007FFFFFu, // Denormals
        0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00001u, 0xFFFFFFFFu, // Infinities and NaNs
    };
    const size_t n_ints = sizeof ints / sizeof ints[0];
    const size_t n_floats = sizeof floats / sizeof floats[0];
    uint8_t data[2048] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_bool(&b, true); // Not on a byte boundary
    for (size_t i = 0; i < n_ints; i++) {
        for (size_t j = 0; j < n_ints; j++) tide_bits_put_changed_difference(&b, ints[i], ints[j]);
    }
    for (size_t i = 0; i < n_floats; i++) {
        for (size_t j = 0; j < n_floats; j++) tide_bits_put_changed_xor(&b, floats[i], floats[j]);
    }
    const uint32_t size = tide_bits_end(&b);
    TIDE_REQUIRE(size > 0 && !b.overflow);

    tide_bits r = {data, size, 0, false};
    TIDE_CHECK(tide_bits_get_bool(&r));
    for (size_t i = 0; i < n_ints; i++) {
        for (size_t j = 0; j < n_ints; j++) {
            const uint32_t got = tide_bits_get_bool(&r) ? tide_bits_get_difference(&r, ints[j]) : ints[j];
            TIDE_CHECK(got == ints[i]);
        }
    }
    for (size_t i = 0; i < n_floats; i++) {
        for (size_t j = 0; j < n_floats; j++) {
            const uint32_t got = tide_bits_get_bool(&r) ? tide_bits_get_xor(&r, floats[j]) : floats[j];
            TIDE_CHECK(got == floats[i]);
        }
    }
    TIDE_CHECK(!r.overflow);
}

TIDE_TEST(bits_float_deltas_come_back_for_any_bits)
{
    // Pairs of random bits, and pairs that differ in a few bits at random places
    uint8_t data[4096];
    uint64_t seed = 0x243F6A8885A308D3ull;
    for (int round = 0; round < 64; round++) {
        uint32_t nows[64], wases[64];
        for (int i = 0; i < 64; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const uint32_t was = (uint32_t)(seed >> 32);
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const uint32_t noise = (uint32_t)(seed >> 32);
            const uint32_t mask = (uint32_t)i < 32 ? noise : noise & noise >> 7 & noise >> 13;
            wases[i] = was;
            nows[i] = was ^ (round & 1 ? mask : mask >> (uint32_t)(round % 31));
        }
        tide_bits b = {data, sizeof data, 0, false};
        for (int i = 0; i < 64; i++) tide_bits_put_changed_xor(&b, nows[i], wases[i]);
        const uint32_t size = tide_bits_end(&b);
        TIDE_REQUIRE(size > 0 && !b.overflow);
        tide_bits r = {data, size, 0, false};
        for (int i = 0; i < 64; i++) {
            const uint32_t got = tide_bits_get_bool(&r) ? tide_bits_get_xor(&r, wases[i]) : wases[i];
            TIDE_CHECK(got == nows[i]);
        }
        TIDE_CHECK(!r.overflow);
    }
}

TIDE_TEST(bits_float_deltas_refuse_a_window_past_the_end)
{
    // A window that starts at bit 30 and is 5 long: no float packs to it
    uint8_t data[8] = {0};
    tide_bits b = {data, sizeof data, 0, false};
    tide_bits_put_bool(&b, true);
    tide_bits_put(&b, 30, 5);
    tide_bits_put(&b, 4, 5);
    tide_bits_put(&b, 0x1F, 5);
    const uint32_t size = tide_bits_end(&b);
    tide_bits r = {data, size, 0, false};
    TIDE_CHECK(tide_bits_get_xor(&r, 0x3F800000u) == 0x3F800000u);
    TIDE_CHECK(r.overflow);
}
