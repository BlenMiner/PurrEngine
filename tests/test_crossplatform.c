#include <inttypes.h>
#include <stdio.h>

#include "tide/math.h"
#include "tide_test.h"

// Cross-platform determinism. Each test hashes the exact bits of many results,
// and the hash must be the same on every platform: native x64, WebAssembly, and
// ARM64 later. A mismatch means some platform computes a different bit.
//
// If math code changes on purpose, the test prints the new hash. Update the
// expected value, then confirm the other platforms agree with it.

static uint64_t hash_bits(uint64_t h, const uint32_t bits)
{
    for (int i = 0; i < 4; i++) {
        h ^= (bits >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull; // FNV-1a
    }
    return h;
}

// NaN payloads aren't portable (WebAssembly doesn't specify them), so every NaN
// hashes the same. NaN in simulation state is a bug anyway.
static uint64_t hash_float(const uint64_t h, const float x)
{
    return hash_bits(h, x != x ? 0x7FC00000u : tide_f_bits(x));
}

static uint64_t hash_int(const uint64_t h, const int32_t x)
{
    return hash_bits(h, (uint32_t)x);
}

static uint64_t hash_f3(const uint64_t h, const tide_float3 v)
{
    return hash_float(hash_float(hash_float(h, v.x), v.y), v.z);
}

static uint32_t rng;

static uint32_t next_bits(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

// Any float at all: every exponent, NaN and infinity included.
static float any_float(void)
{
    return tide_f_from_bits(next_bits());
}

// A typical game value, in [-128, 128).
static float game_float(void)
{
    return ((float)(next_bits() >> 8) - 8388608.0f) * 0x1p-16f;
}

// C leaves the order of a call's arguments unspecified (clang goes right to left
// on Windows, left to right on WebAssembly), so random inputs are always drawn
// into locals first, one statement at a time.
static tide_float3 game_f3(void)
{
    const float x = game_float();
    const float y = game_float();
    const float z = game_float();
    return tide_f3(x, y, z);
}

static void expect_hash(const char *what, const uint64_t got, const uint64_t want)
{
    if (got != want) printf("    %s: hash is 0x%016" PRIX64 ", expected 0x%016" PRIX64 "\n", what, got, want);
    TIDE_CHECK(got == want);
}

#define SAMPLES 100000

TIDE_TEST(crossplatform_transcendentals)
{
    typedef float (*unary)(float);
    static const unary functions[] = {
        tide_sin_f, tide_cos_f, tide_tan_f, tide_asin_f, tide_acos_f, tide_atan_f,
        tide_exp_f, tide_exp2_f, tide_log_f, tide_log2_f, tide_log10_f,
    };
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t f = 0; f < sizeof functions / sizeof functions[0]; f++) {
        rng = (uint32_t)f + 1;
        for (int i = 0; i < SAMPLES; i++) {
            h = hash_float(h, functions[f](any_float()));
            h = hash_float(h, functions[f](game_float()));
        }
    }
    rng = 100;
    for (int i = 0; i < SAMPLES; i++) {
        const float y1 = game_float();
        const float x1 = game_float();
        h = hash_float(h, tide_atan2_f(y1, x1));
        const float y2 = any_float();
        const float x2 = any_float();
        h = hash_float(h, tide_atan2_f(y2, x2));
        const float base = tide_abs_f(game_float());
        const float exponent = game_float() * 0.25f;
        h = hash_float(h, tide_pow_f(base, exponent));
        const float any_base = any_float();
        const float any_exponent = any_float();
        h = hash_float(h, tide_pow_f(any_base, any_exponent));
    }
    expect_hash("transcendentals", h, 0x03B04AA14C16B94Bull);
}

TIDE_TEST(crossplatform_basic_operations)
{
    uint64_t h = 0xCBF29CE484222325ull;
    rng = 200;
    for (int i = 0; i < SAMPLES; i++) {
        const float a = any_float();
        const float b = game_float();
        h = hash_float(h, tide_sqrt_f(a));
        h = hash_float(h, tide_rsqrt_f(b));
        h = hash_float(h, tide_round_f(a));
        h = hash_float(h, tide_floor_f(b));
        h = hash_float(h, tide_ceil_f(a));
        h = hash_float(h, tide_frac_f(b));
        h = hash_float(h, tide_smoothstep_f(-1.0f, 1.0f, b));
        h = hash_int(h, tide_i_from_f(a));
        const int32_t x = (int32_t)next_bits();
        const int32_t y = (int32_t)next_bits() >> 20;
        h = hash_int(h, tide_div_i(x, y));
        h = hash_int(h, tide_mod_i(x, y));
        h = hash_int(h, tide_mul_i(x, y));
        h = hash_int(h, tide_shr_i(x, y));
        h = hash_int(h, tide_shl_i(x, y));
    }
    expect_hash("basic operations", h, 0x5A04D500E2C9D82Aull);
}

TIDE_TEST(crossplatform_geometry)
{
    uint64_t h = 0xCBF29CE484222325ull;
    rng = 300;
    tide_quaternion q = tide_identity_q();
    for (int i = 0; i < SAMPLES; i++) {
        const tide_float3 v = game_f3();
        const tide_float3 w = game_f3();
        h = hash_f3(h, tide_normalize_f3(v));
        h = hash_f3(h, tide_cross_f3(v, w));
        h = hash_float(h, tide_length_f3(v));
        const tide_quaternion r = tide_euler_q(tide_mul_f3(v, tide_f3_splat(0.05f)));
        q = tide_normalize_q(tide_slerp_q(q, r, 0.3f));
        h = hash_f3(h, tide_rotate_q(q, w));
        const tide_float4x4 m = tide_trs_f4x4(v, q, tide_f3(1.5f, 1.0f, 0.5f));
        h = hash_f3(h, tide_transform_f4x4(tide_inverse_f4x4(m), w));
        h = hash_float(h, tide_determinant_f4x4(m));
    }
    expect_hash("geometry", h, 0xA244CB549F91E603ull);
}
