#include <math.h>
#include <stdio.h>

#include "purr/math.h"
#include "purr_test.h"

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
        const float x = purr_exp2_f(rnd(lo_exp, hi_exp));
        const double e = ulp_error(f(x), ref(x));
        if (e > worst) worst = e;
    }
    return worst;
}

static double ref_exp2(const double x) { return exp2(x); }

#define MAX_ULP 1.0

PURR_TEST(math_sin_cos_accuracy)
{
    PURR_CHECK(max_error_unary(purr_sin_f, sin, -10.0f, 10.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_cos_f, cos, -10.0f, 10.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_sin_f, sin, -1e5f, 1e5f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_cos_f, cos, -1e5f, 1e5f, 100000) <= MAX_ULP);
    // Large arguments go through Payne-Hanek reduction.
    PURR_CHECK(max_error_unary_log(purr_sin_f, sin, 19.0f, 127.0f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary_log(purr_cos_f, cos, 19.0f, 127.0f, 100000) <= MAX_ULP);
}

PURR_TEST(math_tan_accuracy)
{
    PURR_CHECK(max_error_unary(purr_tan_f, tan, -1.5f, 1.5f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_tan_f, tan, -1000.0f, 1000.0f, 100000) <= MAX_ULP);
}

PURR_TEST(math_inverse_trig_accuracy)
{
    PURR_CHECK(max_error_unary(purr_asin_f, asin, -1.0f, 1.0f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_acos_f, acos, -1.0f, 1.0f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_atan_f, atan, -100.0f, 100.0f, 100000) <= MAX_ULP);
    PURR_CHECK(max_error_unary_log(purr_atan_f, atan, -60.0f, 60.0f, 100000) <= MAX_ULP);

    double worst = 0.0;
    for (int i = 0; i < 200000; i++) {
        const float y = rnd(-50.0f, 50.0f);
        const float x = rnd(-50.0f, 50.0f);
        const double e = ulp_error(purr_atan2_f(y, x), atan2(y, x));
        if (e > worst) worst = e;
    }
    PURR_CHECK(worst <= MAX_ULP);
}

PURR_TEST(math_exp_log_accuracy)
{
    PURR_CHECK(max_error_unary(purr_exp_f, exp, -100.0f, 88.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary(purr_exp2_f, ref_exp2, -140.0f, 127.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary_log(purr_log_f, log, -140.0f, 127.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary_log(purr_log2_f, log2, -140.0f, 127.0f, 200000) <= MAX_ULP);
    PURR_CHECK(max_error_unary_log(purr_log10_f, log10, -140.0f, 127.0f, 200000) <= MAX_ULP);

    double worst = 0.0;
    for (int i = 0; i < 200000; i++) {
        const float x = purr_exp2_f(rnd(-10.0f, 10.0f));
        const float y = rnd(-10.0f, 10.0f);
        const double e = ulp_error(purr_pow_f(x, y), pow(x, y));
        if (e > worst) worst = e;
    }
    PURR_CHECK(worst <= MAX_ULP);
}

// A clamped value is always in range, even from NaN.
PURR_TEST(math_clamp_nan_gives_the_lower_bound)
{
    PURR_CHECK(purr_clamp_f(NAN, -1.0f, 1.0f) == -1.0f);
    PURR_CHECK(purr_saturate_f(NAN) == 0.0f);
    const purr_float3 v = purr_clamp_f3((purr_float3){NAN, 5.0f, 0.5f}, (purr_float3){-1.0f, -1.0f, -1.0f},
                                        (purr_float3){1.0f, 1.0f, 1.0f});
    PURR_CHECK(v.x == -1.0f && v.y == 1.0f && v.z == 0.5f);
    // Everything else is unchanged, down to the sign of zero.
    PURR_CHECK(signbit(purr_clamp_f(-0.0f, 0.0f, 1.0f)));
    PURR_CHECK(purr_clamp_f(INFINITY, -1.0f, 1.0f) == 1.0f);
}

// With one NaN argument, Min and Max return the other, whichever side it's on.
PURR_TEST(math_min_max_ignore_one_nan)
{
    PURR_CHECK(purr_min_f(NAN, 1.0f) == 1.0f && purr_min_f(1.0f, NAN) == 1.0f);
    PURR_CHECK(purr_max_f(NAN, 1.0f) == 1.0f && purr_max_f(1.0f, NAN) == 1.0f);
    PURR_CHECK(isnan(purr_min_f(NAN, NAN)));
    PURR_CHECK(purr_min_f(2.0f, 1.0f) == 1.0f && purr_max_f(2.0f, 1.0f) == 2.0f);
    PURR_CHECK(!signbit(purr_min_f(-0.0f, 0.0f))); // Unchanged: equal values give the second
    PURR_CHECK(purr_is_finite_f(1.0f) && purr_is_finite_f(-0.0f) && purr_is_finite_f(1e-45f));
    PURR_CHECK(!purr_is_finite_f(NAN) && !purr_is_finite_f(INFINITY) && !purr_is_finite_f(-INFINITY));
}

PURR_TEST(math_special_values)
{
    const float inf = INFINITY;
    PURR_CHECK(isnan(purr_sin_f(NAN)));
    PURR_CHECK(isnan(purr_sin_f(inf)));
    PURR_CHECK(isnan(purr_cos_f(-inf)));
    PURR_CHECK(purr_sin_f(0.0f) == 0.0f);
    PURR_CHECK(purr_log_f(0.0f) == -inf);
    PURR_CHECK(isnan(purr_log_f(-1.0f)));
    PURR_CHECK(purr_log_f(inf) == inf);
    PURR_CHECK(purr_log2_f(1024.0f) == 10.0f);
    PURR_CHECK(purr_exp_f(0.0f) == 1.0f);
    PURR_CHECK(purr_exp_f(1000.0f) == inf);
    PURR_CHECK(purr_exp_f(-1000.0f) == 0.0f);
    PURR_CHECK(purr_exp2_f(10.0f) == 1024.0f);
    PURR_CHECK(isnan(purr_asin_f(1.5f)));
    PURR_CHECK(purr_atan2_f(0.0f, -1.0f) == PURR_PI_F);
    PURR_CHECK(purr_atan2_f(-0.0f, 1.0f) == 0.0f && signbit(purr_atan2_f(-0.0f, 1.0f)));
    PURR_CHECK(purr_atan2_f(1.0f, 0.0f) == PURR_PI_F / 2.0f);
    PURR_CHECK(purr_pow_f(NAN, 0.0f) == 1.0f);
    PURR_CHECK(purr_pow_f(1.0f, NAN) == 1.0f);
    PURR_CHECK(purr_pow_f(-2.0f, 3.0f) == -8.0f);
    PURR_CHECK(isnan(purr_pow_f(-2.0f, 0.5f)));
    PURR_CHECK(purr_pow_f(0.0f, -1.0f) == inf);
    PURR_CHECK(purr_pow_f(-0.0f, -1.0f) == -inf);
    PURR_CHECK(purr_pow_f(0.5f, inf) == 0.0f);
    PURR_CHECK(purr_pow_f(2.0f, -inf) == 0.0f);
    PURR_CHECK(purr_pow_f(-inf, 3.0f) == -inf);
}

PURR_TEST(math_rounding)
{
    PURR_CHECK(purr_round_f(2.5f) == 2.0f);   // Ties to even
    PURR_CHECK(purr_round_f(3.5f) == 4.0f);
    PURR_CHECK(purr_round_f(-2.5f) == -2.0f);
    PURR_CHECK(purr_round_f(-0.3f) == 0.0f && signbit(purr_round_f(-0.3f)));
    PURR_CHECK(purr_floor_f(-0.5f) == -1.0f);
    PURR_CHECK(purr_floor_f(1.5f) == 1.0f);
    PURR_CHECK(purr_ceil_f(-0.5f) == 0.0f && signbit(purr_ceil_f(-0.5f)));
    PURR_CHECK(purr_ceil_f(1.2f) == 2.0f);
    PURR_CHECK(purr_trunc_f(-1.7f) == -1.0f);
    PURR_CHECK(purr_frac_f(-0.25f) == 0.75f);
    PURR_CHECK(purr_round_f(1e30f) == 1e30f);
    for (int i = 0; i < 100000; i++) {
        const float x = rnd(-1e6f, 1e6f);
        PURR_REQUIRE(purr_floor_f(x) == floorf(x));
        PURR_REQUIRE(purr_ceil_f(x) == ceilf(x));
        PURR_REQUIRE(purr_trunc_f(x) == truncf(x));
        PURR_REQUIRE(purr_round_f(x) == rintf(x));
    }
}

PURR_TEST(math_int_conversion_saturates)
{
    PURR_CHECK(purr_i_from_f(1.9f) == 1);
    PURR_CHECK(purr_i_from_f(-1.9f) == -1);
    PURR_CHECK(purr_i_from_f(1e20f) == INT32_MAX);
    PURR_CHECK(purr_i_from_f(-1e20f) == INT32_MIN);
    PURR_CHECK(purr_i_from_f(NAN) == 0);
    PURR_CHECK(purr_div_i(7, 0) == 0);
    PURR_CHECK(purr_div_i(INT32_MIN, -1) == INT32_MIN);
    PURR_CHECK(purr_abs_i(INT32_MIN) == INT32_MIN);
}

static bool near(const float a, const float b)
{
    return fabsf(a - b) <= 1e-5f * fmaxf(1.0f, fabsf(b));
}

static bool near3(const purr_float3 a, const purr_float3 b)
{
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

static bool near_q(const purr_quaternion a, const purr_quaternion b)
{
    // q and -q are the same rotation.
    const float d = fabsf(purr_dot_q(a, b));
    return near(d, 1.0f);
}

PURR_TEST(math_vectors)
{
    const purr_float3 v = purr_f3(3.0f, 4.0f, 12.0f);
    PURR_CHECK(purr_length_f3(v) == 13.0f);
    PURR_CHECK(near(purr_length_f3(purr_normalize_f3(v)), 1.0f));
    const purr_float3 zero = purr_normalizesafe_f3(purr_f3(0.0f, 0.0f, 0.0f));
    PURR_CHECK(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f);
    const purr_float3 c = purr_cross_f3(purr_f3(1, 0, 0), purr_f3(0, 1, 0));
    PURR_CHECK(c.x == 0.0f && c.y == 0.0f && c.z == 1.0f);
    const purr_float3 r = purr_reflect_f3(purr_f3(1, -1, 0), purr_f3(0, 1, 0));
    PURR_CHECK(r.x == 1.0f && r.y == 1.0f && r.z == 0.0f);
}

PURR_TEST(math_quaternions)
{
    const float half_pi = PURR_PI_F / 2.0f;
    const purr_quaternion yaw = purr_axisangle_q(purr_f3(0, 1, 0), half_pi);
    PURR_CHECK(near3(purr_rotate_q(yaw, purr_f3(1, 0, 0)), purr_f3(0, 0, -1)));
    PURR_CHECK(near3(purr_forward_q(yaw), purr_f3(1, 0, 0)));

    PURR_CHECK(near_q(purr_euler_q(purr_f3(0, half_pi, 0)), yaw));
    PURR_CHECK(near_q(purr_mul_q(yaw, purr_inverse_q(yaw)), purr_identity_q()));

    const purr_quaternion a = purr_euler_q(purr_f3(0.3f, -1.2f, 0.7f));
    const purr_quaternion b = purr_euler_q(purr_f3(-0.9f, 0.4f, 2.1f));
    PURR_CHECK(near_q(purr_slerp_q(a, b, 0.0f), a));
    PURR_CHECK(near_q(purr_slerp_q(a, b, 1.0f), b));
    const float total = purr_angle_q(a, b);
    PURR_CHECK(near(purr_angle_q(a, purr_slerp_q(a, b, 0.5f)), total * 0.5f));

    PURR_CHECK(near_q(purr_q_from_f3x3(purr_f3x3_from_q(a)), a));
    PURR_CHECK(near_q(purr_q_from_f3x3(purr_f3x3_from_q(b)), b));

    const purr_float3 forward = purr_normalize_f3(purr_f3(1, 2, 3));
    PURR_CHECK(near3(purr_forward_q(purr_lookrotation_q(forward, purr_f3(0, 1, 0))), forward));
}

PURR_TEST(math_matrices)
{
    const purr_quaternion r = purr_euler_q(purr_f3(0.5f, 1.0f, -0.25f));
    const purr_float4x4 m = purr_trs_f4x4(purr_f3(1, 2, 3), r, purr_f3(2, 3, 4));

    // Scale, then rotate, then translate.
    const purr_float3 p = purr_f3(0.5f, -1.0f, 2.0f);
    const purr_float3 expected = purr_add_f3(purr_rotate_q(r, purr_mul_f3(p, purr_f3(2, 3, 4))), purr_f3(1, 2, 3));
    PURR_CHECK(near3(purr_transform_f4x4(m, p), expected));

    PURR_CHECK(near(purr_determinant_f4x4(m), 24.0f));

    const purr_float4x4 id = purr_mul_f4x4(purr_inverse_f4x4(m), m);
    const purr_float4x4 want = purr_identity_f4x4();
    const float *got_f = &id.c0.x;
    const float *want_f = &want.c0.x;
    for (int i = 0; i < 16; i++) PURR_CHECK(fabsf(got_f[i] - want_f[i]) < 1e-5f);

    const purr_float3x3 m3 = {{2, 1, 0}, {1, 3, 1}, {0, 1, 4}};
    const purr_float3x3 id3 = purr_mul_f3x3(m3, purr_inverse_f3x3(m3));
    PURR_CHECK(near3(id3.c0, purr_f3(1, 0, 0)) && near3(id3.c1, purr_f3(0, 1, 0)) && near3(id3.c2, purr_f3(0, 0, 1)));

    const purr_float2x2 m2 = {{4, 2}, {7, 6}};
    const purr_float2x2 id2 = purr_mul_f2x2(m2, purr_inverse_f2x2(m2));
    PURR_CHECK(near(id2.c0.x, 1) && near(id2.c0.y, 0) && near(id2.c1.x, 0) && near(id2.c1.y, 1));
}
