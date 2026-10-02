#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/math.h"
#include "tide_test.h"

// Accuracy is measured against the C library in double precision. That's fine
// in tests: only simulation code must avoid the platform math library.

// Error of `got` in units in the last place of the float nearest to `want`.
static double ulp_error(const float got, const double want)
{
    if (isnan(want)) return isnan(got) ? 0.0 : 1e9;
    if (isinf(want)) return (isinf(got) && (got > 0) == (want > 0)) ? 0.0 : 1e9;
    const float ref = (float)want;
    float ulp = nextafterf(fabsf(ref), INFINITY) - fabsf(ref);
    if (ulp == 0.0f || isinf(ulp)) ulp = 0x1p-149f;
    return fabs((double)got - want) / ulp;
}

// Deterministic pseudo-random floats in [lo, hi].
static uint32_t rng_state = 12345;
static float rnd(const float lo, const float hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng_state >> 8) * 0x1p-24f;
}

typedef float (*unary_fn)(float);
typedef double (*unary_ref)(double);

static double max_error_unary(const unary_fn f, const unary_ref ref, const float lo, const float hi, const int samples)
{
    double worst = 0.0;
    for (int i = 0; i < samples; i++) {
        const float x = rnd(lo, hi);
        const double e = ulp_error(f(x), ref(x));
        if (e > worst) worst = e;
    }
    return worst;
}

// Samples spread over many orders of magnitude, for logs and large arguments.
static double max_error_unary_log(const unary_fn f, const unary_ref ref, const float lo_exp, const float hi_exp, const int samples)
{
    double worst = 0.0;
    for (int i = 0; i < samples; i++) {
        const float x = tide_exp2_f(rnd(lo_exp, hi_exp));
        const double e = ulp_error(f(x), ref(x));
        if (e > worst) worst = e;
    }
    return worst;
}

static double ref_exp2(const double x) { return exp2(x); }

#define MAX_ULP 1.0

TIDE_TEST(math_sin_cos_accuracy)
{
    TIDE_CHECK(max_error_unary(tide_sin_f, sin, -10.0f, 10.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_cos_f, cos, -10.0f, 10.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_sin_f, sin, -1e5f, 1e5f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_cos_f, cos, -1e5f, 1e5f, 100000) <= MAX_ULP);
    // Large arguments go through Payne-Hanek reduction.
    TIDE_CHECK(max_error_unary_log(tide_sin_f, sin, 19.0f, 127.0f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary_log(tide_cos_f, cos, 19.0f, 127.0f, 100000) <= MAX_ULP);
}

TIDE_TEST(math_tan_accuracy)
{
    TIDE_CHECK(max_error_unary(tide_tan_f, tan, -1.5f, 1.5f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_tan_f, tan, -1000.0f, 1000.0f, 100000) <= MAX_ULP);
}

TIDE_TEST(math_inverse_trig_accuracy)
{
    TIDE_CHECK(max_error_unary(tide_asin_f, asin, -1.0f, 1.0f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_acos_f, acos, -1.0f, 1.0f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_atan_f, atan, -100.0f, 100.0f, 100000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary_log(tide_atan_f, atan, -60.0f, 60.0f, 100000) <= MAX_ULP);

    double worst = 0.0;
    for (int i = 0; i < 200000; i++) {
        const float y = rnd(-50.0f, 50.0f);
        const float x = rnd(-50.0f, 50.0f);
        const double e = ulp_error(tide_atan2_f(y, x), atan2(y, x));
        if (e > worst) worst = e;
    }
    TIDE_CHECK(worst <= MAX_ULP);
}

TIDE_TEST(math_exp_log_accuracy)
{
    TIDE_CHECK(max_error_unary(tide_exp_f, exp, -100.0f, 88.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary(tide_exp2_f, ref_exp2, -140.0f, 127.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary_log(tide_log_f, log, -140.0f, 127.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary_log(tide_log2_f, log2, -140.0f, 127.0f, 200000) <= MAX_ULP);
    TIDE_CHECK(max_error_unary_log(tide_log10_f, log10, -140.0f, 127.0f, 200000) <= MAX_ULP);

    double worst = 0.0;
    for (int i = 0; i < 200000; i++) {
        const float x = tide_exp2_f(rnd(-10.0f, 10.0f));
        const float y = rnd(-10.0f, 10.0f);
        const double e = ulp_error(tide_pow_f(x, y), pow(x, y));
        if (e > worst) worst = e;
    }
    TIDE_CHECK(worst <= MAX_ULP);
}

// A clamped value is always in range, even from NaN.
TIDE_TEST(math_clamp_nan_gives_the_lower_bound)
{
    TIDE_CHECK(tide_clamp_f(NAN, -1.0f, 1.0f) == -1.0f);
    TIDE_CHECK(tide_saturate_f(NAN) == 0.0f);
    const tide_float3 v = tide_clamp_f3((tide_float3){NAN, 5.0f, 0.5f}, (tide_float3){-1.0f, -1.0f, -1.0f},
                                        (tide_float3){1.0f, 1.0f, 1.0f});
    TIDE_CHECK(v.x == -1.0f && v.y == 1.0f && v.z == 0.5f);
    // Everything else is unchanged, down to the sign of zero.
    TIDE_CHECK(signbit(tide_clamp_f(-0.0f, 0.0f, 1.0f)));
    TIDE_CHECK(tide_clamp_f(INFINITY, -1.0f, 1.0f) == 1.0f);
}

// With one NaN argument, Min and Max return the other, whichever side it's on.
TIDE_TEST(math_min_max_ignore_one_nan)
{
    TIDE_CHECK(tide_min_f(NAN, 1.0f) == 1.0f && tide_min_f(1.0f, NAN) == 1.0f);
    TIDE_CHECK(tide_max_f(NAN, 1.0f) == 1.0f && tide_max_f(1.0f, NAN) == 1.0f);
    TIDE_CHECK(isnan(tide_min_f(NAN, NAN)));
    TIDE_CHECK(tide_min_f(2.0f, 1.0f) == 1.0f && tide_max_f(2.0f, 1.0f) == 2.0f);
    TIDE_CHECK(!signbit(tide_min_f(-0.0f, 0.0f))); // Unchanged: equal values give the second
    TIDE_CHECK(tide_is_finite_f(1.0f) && tide_is_finite_f(-0.0f) && tide_is_finite_f(1e-45f));
    TIDE_CHECK(!tide_is_finite_f(NAN) && !tide_is_finite_f(INFINITY) && !tide_is_finite_f(-INFINITY));
}

TIDE_TEST(math_special_values)
{
    const float inf = INFINITY;
    TIDE_CHECK(isnan(tide_sin_f(NAN)));
    TIDE_CHECK(isnan(tide_sin_f(inf)));
    TIDE_CHECK(isnan(tide_cos_f(-inf)));
    TIDE_CHECK(tide_sin_f(0.0f) == 0.0f);
    TIDE_CHECK(tide_log_f(0.0f) == -inf);
    TIDE_CHECK(isnan(tide_log_f(-1.0f)));
    TIDE_CHECK(tide_log_f(inf) == inf);
    TIDE_CHECK(tide_log2_f(1024.0f) == 10.0f);
    TIDE_CHECK(tide_exp_f(0.0f) == 1.0f);
    TIDE_CHECK(tide_exp_f(1000.0f) == inf);
    TIDE_CHECK(tide_exp_f(-1000.0f) == 0.0f);
    TIDE_CHECK(tide_exp2_f(10.0f) == 1024.0f);
    TIDE_CHECK(isnan(tide_asin_f(1.5f)));
    TIDE_CHECK(tide_atan2_f(0.0f, -1.0f) == TIDE_PI_F);
    TIDE_CHECK(tide_atan2_f(-0.0f, 1.0f) == 0.0f && signbit(tide_atan2_f(-0.0f, 1.0f)));
    TIDE_CHECK(tide_atan2_f(1.0f, 0.0f) == TIDE_PI_F / 2.0f);
    TIDE_CHECK(tide_pow_f(NAN, 0.0f) == 1.0f);
    TIDE_CHECK(tide_pow_f(1.0f, NAN) == 1.0f);
    TIDE_CHECK(tide_pow_f(-2.0f, 3.0f) == -8.0f);
    TIDE_CHECK(isnan(tide_pow_f(-2.0f, 0.5f)));
    TIDE_CHECK(tide_pow_f(0.0f, -1.0f) == inf);
    TIDE_CHECK(tide_pow_f(-0.0f, -1.0f) == -inf);
    TIDE_CHECK(tide_pow_f(0.5f, inf) == 0.0f);
    TIDE_CHECK(tide_pow_f(2.0f, -inf) == 0.0f);
    TIDE_CHECK(tide_pow_f(-inf, 3.0f) == -inf);
}

TIDE_TEST(math_rounding)
{
    TIDE_CHECK(tide_round_f(2.5f) == 2.0f);   // Ties to even
    TIDE_CHECK(tide_round_f(3.5f) == 4.0f);
    TIDE_CHECK(tide_round_f(-2.5f) == -2.0f);
    TIDE_CHECK(tide_round_f(-0.3f) == 0.0f && signbit(tide_round_f(-0.3f)));
    TIDE_CHECK(tide_floor_f(-0.5f) == -1.0f);
    TIDE_CHECK(tide_floor_f(1.5f) == 1.0f);
    TIDE_CHECK(tide_ceil_f(-0.5f) == 0.0f && signbit(tide_ceil_f(-0.5f)));
    TIDE_CHECK(tide_ceil_f(1.2f) == 2.0f);
    TIDE_CHECK(tide_trunc_f(-1.7f) == -1.0f);
    TIDE_CHECK(tide_frac_f(-0.25f) == 0.75f);
    TIDE_CHECK(tide_round_f(1e30f) == 1e30f);
    for (int i = 0; i < 100000; i++) {
        const float x = rnd(-1e6f, 1e6f);
        TIDE_REQUIRE(tide_floor_f(x) == floorf(x));
        TIDE_REQUIRE(tide_ceil_f(x) == ceilf(x));
        TIDE_REQUIRE(tide_trunc_f(x) == truncf(x));
        TIDE_REQUIRE(tide_round_f(x) == rintf(x));
    }
}

TIDE_TEST(math_int_conversion_saturates)
{
    TIDE_CHECK(tide_i_from_f(1.9f) == 1);
    TIDE_CHECK(tide_i_from_f(-1.9f) == -1);
    TIDE_CHECK(tide_i_from_f(1e20f) == INT32_MAX);
    TIDE_CHECK(tide_i_from_f(-1e20f) == INT32_MIN);
    TIDE_CHECK(tide_i_from_f(NAN) == 0);
    TIDE_CHECK(tide_div_i(7, 0) == 0);
    TIDE_CHECK(tide_div_i(INT32_MIN, -1) == INT32_MIN);
    TIDE_CHECK(tide_abs_i(INT32_MIN) == INT32_MIN);
}

static bool near(const float a, const float b)
{
    return fabsf(a - b) <= 1e-5f * fmaxf(1.0f, fabsf(b));
}

static bool near3(const tide_float3 a, const tide_float3 b)
{
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

static bool near_q(const tide_quaternion a, const tide_quaternion b)
{
    // q and -q are the same rotation.
    const float d = fabsf(tide_dot_q(a, b));
    return near(d, 1.0f);
}

TIDE_TEST(math_vectors)
{
    const tide_float3 v = tide_f3(3.0f, 4.0f, 12.0f);
    TIDE_CHECK(tide_length_f3(v) == 13.0f);
    TIDE_CHECK(near(tide_length_f3(tide_normalize_f3(v)), 1.0f));
    const tide_float3 zero = tide_normalizesafe_f3(tide_f3(0.0f, 0.0f, 0.0f));
    TIDE_CHECK(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f);
    const tide_float3 c = tide_cross_f3(tide_f3(1, 0, 0), tide_f3(0, 1, 0));
    TIDE_CHECK(c.x == 0.0f && c.y == 0.0f && c.z == 1.0f);
    const tide_float3 r = tide_reflect_f3(tide_f3(1, -1, 0), tide_f3(0, 1, 0));
    TIDE_CHECK(r.x == 1.0f && r.y == 1.0f && r.z == 0.0f);
}

TIDE_TEST(math_quaternions)
{
    const float half_pi = TIDE_PI_F / 2.0f;
    const tide_quaternion yaw = tide_axisangle_q(tide_f3(0, 1, 0), half_pi);
    TIDE_CHECK(near3(tide_rotate_q(yaw, tide_f3(1, 0, 0)), tide_f3(0, 0, -1)));
    TIDE_CHECK(near3(tide_forward_q(yaw), tide_f3(1, 0, 0)));

    TIDE_CHECK(near_q(tide_euler_q(tide_f3(0, half_pi, 0)), yaw));
    TIDE_CHECK(near_q(tide_mul_q(yaw, tide_inverse_q(yaw)), tide_identity_q()));

    const tide_quaternion a = tide_euler_q(tide_f3(0.3f, -1.2f, 0.7f));
    const tide_quaternion b = tide_euler_q(tide_f3(-0.9f, 0.4f, 2.1f));
    TIDE_CHECK(near_q(tide_slerp_q(a, b, 0.0f), a));
    TIDE_CHECK(near_q(tide_slerp_q(a, b, 1.0f), b));
    const float total = tide_angle_q(a, b);
    TIDE_CHECK(near(tide_angle_q(a, tide_slerp_q(a, b, 0.5f)), total * 0.5f));

    TIDE_CHECK(near_q(tide_q_from_f3x3(tide_f3x3_from_q(a)), a));
    TIDE_CHECK(near_q(tide_q_from_f3x3(tide_f3x3_from_q(b)), b));

    const tide_float3 forward = tide_normalize_f3(tide_f3(1, 2, 3));
    TIDE_CHECK(near3(tide_forward_q(tide_lookrotation_q(forward, tide_f3(0, 1, 0))), forward));
}

TIDE_TEST(math_matrices)
{
    const tide_quaternion r = tide_euler_q(tide_f3(0.5f, 1.0f, -0.25f));
    const tide_float4x4 m = tide_trs_f4x4(tide_f3(1, 2, 3), r, tide_f3(2, 3, 4));

    // Scale, then rotate, then translate.
    const tide_float3 p = tide_f3(0.5f, -1.0f, 2.0f);
    const tide_float3 expected = tide_add_f3(tide_rotate_q(r, tide_mul_f3(p, tide_f3(2, 3, 4))), tide_f3(1, 2, 3));
    TIDE_CHECK(near3(tide_transform_f4x4(m, p), expected));

    TIDE_CHECK(near(tide_determinant_f4x4(m), 24.0f));

    const tide_float4x4 id = tide_mul_f4x4(tide_inverse_f4x4(m), m);
    const tide_float4x4 want = tide_identity_f4x4();
    const float *got_f = &id.c0.x;
    const float *want_f = &want.c0.x;
    for (int i = 0; i < 16; i++) TIDE_CHECK(fabsf(got_f[i] - want_f[i]) < 1e-5f);

    const tide_float3x3 m3 = {{2, 1, 0}, {1, 3, 1}, {0, 1, 4}};
    const tide_float3x3 id3 = tide_mul_f3x3(m3, tide_inverse_f3x3(m3));
    TIDE_CHECK(near3(id3.c0, tide_f3(1, 0, 0)) && near3(id3.c1, tide_f3(0, 1, 0)) && near3(id3.c2, tide_f3(0, 0, 1)));

    const tide_float2x2 m2 = {{4, 2}, {7, 6}};
    const tide_float2x2 id2 = tide_mul_f2x2(m2, tide_inverse_f2x2(m2));
    TIDE_CHECK(near(id2.c0.x, 1) && near(id2.c0.y, 0) && near(id2.c1.x, 0) && near(id2.c1.y, 1));
}

// xxHash32 as its spec writes it, byte by byte, for Math.Hash's versions to
// agree with.
static uint32_t xxh32_read(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t xxh32(const void *data, const size_t size, const uint32_t seed)
{
    const uint8_t *p = data;
    const uint8_t *const end = p + size;
    uint32_t h;
    if (size >= 16) {
        uint32_t lanes[4] = {seed + TIDE_XXH_P1 + TIDE_XXH_P2, seed + TIDE_XXH_P2, seed, seed - TIDE_XXH_P1};
        for (; end - p >= 16; p += 16) {
            for (int i = 0; i < 4; i++) {
                lanes[i] = tide_xxh_rotl(lanes[i] + xxh32_read(p + 4 * i) * TIDE_XXH_P2, 13) * TIDE_XXH_P1;
            }
        }
        h = tide_xxh_rotl(lanes[0], 1) + tide_xxh_rotl(lanes[1], 7) + tide_xxh_rotl(lanes[2], 12)
          + tide_xxh_rotl(lanes[3], 18);
    } else {
        h = seed + TIDE_XXH_P5;
    }
    h += (uint32_t)size;
    for (; end - p >= 4; p += 4) h = tide_xxh_rotl(h + xxh32_read(p) * TIDE_XXH_P3, 17) * TIDE_XXH_P4;
    for (; p < end; p++) h = tide_xxh_rotl(h + *p * TIDE_XXH_P5, 11) * TIDE_XXH_P1;
    h ^= h >> 15;
    h *= TIDE_XXH_P2;
    h ^= h >> 13;
    h *= TIDE_XXH_P3;
    h ^= h >> 16;
    return h;
}

// The value's bytes, little-endian whatever the machine's order.
static int32_t hash_reference(const int32_t *words, const int count)
{
    uint8_t bytes[16];
    for (int i = 0; i < count; i++) {
        const uint32_t u = (uint32_t)words[i];
        for (int b = 0; b < 4; b++) bytes[4 * i + b] = (uint8_t)(u >> (8 * b));
    }
    return (int32_t)(xxh32(bytes, (size_t)count * 4, 0) & 0x7FFFFFFFu);
}

TIDE_TEST(math_hash_is_xxhash32)
{
    // The reference against xxHash's own: the short path, the bytes at the
    // end, and stripes, words and bytes together (39 bytes).
    TIDE_CHECK(xxh32("", 0, 0) == 0x02CC5D05u);
    TIDE_CHECK(xxh32("abc", 3, 0) == 0x32D153FFu);
    const char *nobody = "Nobody inspects the spammish repetition";
    TIDE_CHECK(xxh32(nobody, strlen(nobody), 0) == 0xE2293B2Fu);

    rng_state = 777;
    for (int i = 0; i < 1000; i++) {
        int32_t w[4];
        for (int k = 0; k < 4; k++) {
            rng_state = rng_state * 1664525u + 1013904223u;
            w[k] = (int32_t)rng_state;
        }
        TIDE_CHECK(tide_hash_i(w[0]) == hash_reference(w, 1));
        TIDE_CHECK(tide_hash_i2(tide_i2(w[0], w[1])) == hash_reference(w, 2));
        TIDE_CHECK(tide_hash_i3(tide_i3(w[0], w[1], w[2])) == hash_reference(w, 3));
        TIDE_CHECK(tide_hash_i4(tide_i4(w[0], w[1], w[2], w[3])) == hash_reference(w, 4));
    }
    TIDE_CHECK(tide_hash_i(INT32_MIN) >= 0 && tide_hash_i(-1) >= 0);
}

// Cells next to each other give unrelated numbers: every one of the low bits
// is set about half the time, as one bit of a neighbor changes.
TIDE_TEST(math_hash_of_cells_has_no_stripes)
{
    int ones[8] = {0};
    int differ[8] = {0};
    const int size = 128;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const int32_t h = tide_hash_i2(tide_i2(x, y));
            const int32_t right = tide_hash_i2(tide_i2(x + 1, y));
            for (int b = 0; b < 8; b++) {
                ones[b] += (h >> b) & 1;
                differ[b] += ((h ^ right) >> b) & 1;
            }
        }
    }
    const int half = size * size / 2;
    for (int b = 0; b < 8; b++) {
        TIDE_CHECK(abs(ones[b] - half) < half / 20);
        TIDE_CHECK(abs(differ[b] - half) < half / 20);
    }
}
