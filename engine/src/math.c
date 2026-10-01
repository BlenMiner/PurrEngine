#include "tide/math.h"

// Transcendental functions for simulation code.
//
// Each takes and returns float but computes in double, using only IEEE 754
// basic operations (+ - * / and sqrt) in a fixed order. That makes results
// bit-identical on every platform. They are accurate to about 1 ulp of float
// but not correctly rounded; a correctly rounded library such as CORE-MATH can
// replace them later without changing any caller.
//
// Constants that must be exact were computed with integer arithmetic and are
// written as hex floats.

#pragma STDC FP_CONTRACT OFF

static const double PIO2_1 = 0x1.921fb54400000p+0;   // First 33 bits of pi/2
static const double PIO2_1T = 0x1.0b4611a626331p-34; // pi/2 - PIO2_1
static const double TWO_OVER_PI = 0x1.45f306dc9c883p-1;
static const double PIO2 = 0x1.921fb54442d18p+0;
static const double PI_D = 0x1.921fb54442d18p+1;
static const double LN2_HI = 0x1.62e42ff000000p-1; // First 32 bits of ln 2
static const double LN2_LO = -0x1.718432a1b0e26p-35; // ln 2 - LN2_HI
static const double LN2 = 0x1.62e42fefa39efp-1;
static const double INV_LN2 = 0x1.71547652b82fep+0;
static const double INV_LN10 = 0x1.bcb7b1526e50ep-2;
static const double SQRT2 = 0x1.6a09e667f3bcdp+0;

// Bits of 2/pi after the binary point, for reducing large arguments.
static const uint32_t TWO_OVER_PI_BITS[10] = {
    0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599,
    0x3C439041, 0xFE5163AB, 0xDEBBC561, 0xB7246E3A, 0x424DD2E0,
};

static uint64_t d_bits(const double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u;
}

static double d_from_bits(const uint64_t u)
{
    double x;
    memcpy(&x, &u, sizeof x);
    return x;
}

static double d_abs(const double x)
{
    return d_from_bits(d_bits(x) & 0x7FFFFFFFFFFFFFFFull);
}

static bool d_signbit(const double x)
{
    return (d_bits(x) >> 63) != 0;
}

// Nearest integer, ties to even. Valid for |x| < 2^51.
static double round_nearest(const double x)
{
    const double magic = 0x1.8p52;
    return (x + magic) - magic;
}

// 2^k for -1022 <= k <= 1023.
static double pow2i(const int k)
{
    return d_from_bits((uint64_t)(k + 1023) << 52);
}

// ---------------------------------------------------------------------------
// Argument reduction: x = n * pi/2 + r with r in [-pi/4, pi/4]. Returns n mod 4.

// The 32 bits of 2/pi starting `offset` bits after the binary point.
static uint32_t two_over_pi_at(const int offset)
{
    const int word = offset / 32;
    const int shift = offset % 32;
    const uint64_t pair = ((uint64_t)TWO_OVER_PI_BITS[word] << 32) | TWO_OVER_PI_BITS[word + 1];
    return (uint32_t)(pair >> (32 - shift));
}

// Payne-Hanek reduction for large |x|: exact integer arithmetic on the float's
// 24-bit mantissa and a 96-bit window of 2/pi's bits.
static int reduce_large(const float ax, double *r)
{
    const uint32_t u = tide_f_bits(ax);
    const int e = (int)(u >> 23) - 150;                    // ax = m * 2^e
    const uint64_t m = (u & 0x7FFFFFu) | 0x800000u;       // 24-bit mantissa

    // Bits of 2/pi weighing 2^(e - i) with e - i >= 2 only add multiples of 4 to
    // x * 2/pi, which don't change the quadrant, so the window starts at bit e - 1.
    const int first = e - 1 > 1 ? e - 1 : 1;               // 1-based bit index
    const uint64_t w0 = two_over_pi_at(first - 1);
    const uint64_t w1 = two_over_pi_at(first - 1 + 32);
    const uint64_t w2 = two_over_pi_at(first - 1 + 64);

    // N = m * window, as a 128-bit number hi:lo.
    const uint64_t p0 = m * w0, p1 = m * w1, p2 = m * w2;
    const uint64_t lo = p2 + (p1 << 32);
    const uint64_t carry = lo < p2 ? 1 : 0;
    const uint64_t hi = p0 + (p1 >> 32) + carry;

    // N * 2^-f is x * 2/pi (mod 4), where f is the number of fraction bits.
    const int f = first + 95 - e;                          // 94..100 for the inputs we get here
    const int quadrant = (int)(hi >> (f - 64)) & 3;
    const int k = 128 - f;
    const uint64_t fraction = (hi << k) | (lo >> (64 - k)); // Top 64 bits of the fraction

    // As a signed number the fraction is in [-0.5, 0.5); a negative one means
    // rounding up to the next quadrant.
    *r = (double)(int64_t)fraction * 0x1p-64 * PIO2;
    return (quadrant + (int)(fraction >> 63)) & 3;
}

static int reduce_pio2(const float x, double *r)
{
    const float ax = tide_abs_f(x);
    int n;
    if (ax <= 0.785398f) {
        *r = x;
        return 0;
    }
    if (ax < 524288.0f) {
        // Cody-Waite: k * PIO2_1 is exact because k has at most 20 bits.
        const double k = round_nearest((double)ax * TWO_OVER_PI);
        *r = ((double)ax - k * PIO2_1) - k * PIO2_1T;
        n = (int)k & 3;
    } else {
        n = reduce_large(ax, r);
    }
    if (x < 0.0f) {
        *r = -*r;
        n = (4 - n) & 3;
    }
    return n;
}

// Taylor series on [-pi/4, pi/4]; the first omitted terms are below 1e-16.
static double sin_kernel(const double r)
{
    const double r2 = r * r;
    return r + r * r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0
        + r2 * (-1.0 / 39916800.0 + r2 * (1.0 / 6227020800.0 + r2 * (-1.0 / 1307674368000.0)))))));
}

static double cos_kernel(const double r)
{
    const double r2 = r * r;
    return 1.0 + r2 * (-1.0 / 2.0 + r2 * (1.0 / 24.0 + r2 * (-1.0 / 720.0 + r2 * (1.0 / 40320.0
        + r2 * (-1.0 / 3628800.0 + r2 * (1.0 / 479001600.0 + r2 * (-1.0 / 87178291200.0
        + r2 * (1.0 / 20922789888000.0))))))));
}

float tide_sin_f(const float x)
{
    if (!(tide_abs_f(x) <= 3.4028235e38f)) return x - x; // inf or NaN -> NaN
    double r;
    const int n = reduce_pio2(x, &r);
    switch (n) {
    case 0: return (float)sin_kernel(r);
    case 1: return (float)cos_kernel(r);
    case 2: return (float)-sin_kernel(r);
    default: return (float)-cos_kernel(r);
    }
}

float tide_cos_f(const float x)
{
    if (!(tide_abs_f(x) <= 3.4028235e38f)) return x - x;
    double r;
    const int n = reduce_pio2(x, &r);
    switch (n) {
    case 0: return (float)cos_kernel(r);
    case 1: return (float)-sin_kernel(r);
    case 2: return (float)-cos_kernel(r);
    default: return (float)sin_kernel(r);
    }
}

float tide_tan_f(const float x)
{
    if (!(tide_abs_f(x) <= 3.4028235e38f)) return x - x;
    double r;
    const int n = reduce_pio2(x, &r);
    const double s = sin_kernel(r);
    const double c = cos_kernel(r);
    return (float)((n & 1) ? -c / s : s / c);
}

// ---------------------------------------------------------------------------
// Inverse trigonometry

// atan for any double, including infinities.
static double atan_d(const double x)
{
    double t = d_abs(x);
    const bool invert = t > 1.0;
    if (invert) t = 1.0 / t;
    // Two halvings, atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))), bring t below
    // tan(pi/16), where the series converges fast.
    t = t / (1.0 + __builtin_sqrt(1.0 + t * t));
    t = t / (1.0 + __builtin_sqrt(1.0 + t * t));
    const double t2 = t * t;
    double p = 1.0 / 23.0;
    p = 1.0 / 21.0 - t2 * p;
    p = 1.0 / 19.0 - t2 * p;
    p = 1.0 / 17.0 - t2 * p;
    p = 1.0 / 15.0 - t2 * p;
    p = 1.0 / 13.0 - t2 * p;
    p = 1.0 / 11.0 - t2 * p;
    p = 1.0 / 9.0 - t2 * p;
    p = 1.0 / 7.0 - t2 * p;
    p = 1.0 / 5.0 - t2 * p;
    p = 1.0 / 3.0 - t2 * p;
    p = 1.0 - t2 * p;
    double a = 4.0 * (t * p);
    if (invert) a = PIO2 - a;
    return d_signbit(x) ? -a : a;
}

// atan2 with the C99 special cases for zeros and infinities.
static double atan2_d(const double y, const double x)
{
    if (y != y || x != x) return y + x;
    const bool y_neg = d_signbit(y);
    if (y == 0.0) {
        if (d_signbit(x)) return y_neg ? -PI_D : PI_D; // x is -0 or negative
        return y;                                     // +-0
    }
    if (x == 0.0) return y_neg ? -PIO2 : PIO2;
    const bool x_inf = d_abs(x) > 1.7976931348623157e308;
    const bool y_inf = d_abs(y) > 1.7976931348623157e308;
    double a;
    if (x_inf && y_inf) {
        a = d_signbit(x) ? 3.0 * PIO2 / 2.0 : PIO2 / 2.0;
    } else if (y_inf) {
        a = PIO2;
    } else if (x_inf) {
        a = d_signbit(x) ? PI_D : 0.0;
    } else {
        a = atan_d(d_abs(y / x));
        if (x < 0.0) a = PI_D - a;
    }
    return y_neg ? -a : a;
}

float tide_atan_f(const float x)
{
    return (float)atan_d(x);
}

float tide_atan2_f(const float y, const float x)
{
    return (float)atan2_d(y, x);
}

float tide_asin_f(const float x)
{
    if (!(tide_abs_f(x) <= 1.0f)) return __builtin_nanf(""); // NaN
    const double d = x;
    return (float)atan2_d(d, __builtin_sqrt((1.0 - d) * (1.0 + d)));
}

float tide_acos_f(const float x)
{
    if (!(tide_abs_f(x) <= 1.0f)) return __builtin_nanf("");
    const double d = x;
    return (float)atan2_d(__builtin_sqrt((1.0 - d) * (1.0 + d)), d);
}

// ---------------------------------------------------------------------------
// Exponentials and logarithms

// e^r for |r| <= ln(2)/2; Taylor series whose first omitted term is below 1e-17.
static double exp_kernel(const double r)
{
    return 1.0 + r * (1.0 + r * (1.0 / 2.0 + r * (1.0 / 6.0 + r * (1.0 / 24.0 + r * (1.0 / 120.0
        + r * (1.0 / 720.0 + r * (1.0 / 5040.0 + r * (1.0 / 40320.0 + r * (1.0 / 362880.0
        + r * (1.0 / 3628800.0 + r * (1.0 / 39916800.0 + r * (1.0 / 479001600.0
        + r * (1.0 / 6227020800.0)))))))))))));
}

// e^t for the range float results need; outside it the result over- or underflows.
static double exp_d(const double t)
{
    if (t != t) return t;
    if (t > 100.0) return __builtin_inf();
    if (t < -110.0) return 0.0;
    const double k = round_nearest(t * INV_LN2);
    const double r = (t - k * LN2_HI) - k * LN2_LO; // k * LN2_HI is exact for |k| < 2^21
    return exp_kernel(r) * pow2i((int)k);
}

// ln(x) for positive finite x, split as e * ln 2 + p with p = ln(m), m in [sqrt(1/2), sqrt(2)].
static void log_parts(const double x, int *e, double *p)
{
    const uint64_t u = d_bits(x);
    int exponent = (int)(u >> 52) - 1023; // Floats are always normal as doubles
    double m = d_from_bits((u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
    if (m > SQRT2) {
        m *= 0.5;
        exponent += 1;
    }
    // ln(m) = 2 atanh(s), s = (m - 1) / (m + 1), with |s| < 0.172.
    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    double q = 2.0 / 21.0;
    q = 2.0 / 19.0 + s2 * q;
    q = 2.0 / 17.0 + s2 * q;
    q = 2.0 / 15.0 + s2 * q;
    q = 2.0 / 13.0 + s2 * q;
    q = 2.0 / 11.0 + s2 * q;
    q = 2.0 / 9.0 + s2 * q;
    q = 2.0 / 7.0 + s2 * q;
    q = 2.0 / 5.0 + s2 * q;
    q = 2.0 / 3.0 + s2 * q;
    q = 2.0 + s2 * q;
    *e = exponent;
    *p = s * q;
}

static double log_d(const double x)
{
    int e;
    double p;
    log_parts(x, &e, &p);
    return (double)e * LN2 + p;
}

// NaN for negative inputs, -inf for zero, +inf for +inf; otherwise 0 and
// the caller computes the logarithm.
static bool log_special(const float x, float *result)
{
    if (x != x) {
        *result = x;
        return true;
    }
    if (x < 0.0f) {
        *result = __builtin_nanf("");
        return true;
    }
    if (x == 0.0f) {
        *result = -__builtin_inff();
        return true;
    }
    if (x > 3.4028235e38f) {
        *result = x;
        return true;
    }
    return false;
}

float tide_exp_f(const float x)
{
    return (float)exp_d(x);
}

float tide_exp2_f(const float x)
{
    if (x != x) return x;
    if (x > 130.0f) return __builtin_inff();
    if (x < -160.0f) return 0.0f;
    const double k = round_nearest(x);
    const double r = (double)x - k; // Exact
    return (float)(exp_kernel(r * LN2) * pow2i((int)k));
}

float tide_log_f(const float x)
{
    float special;
    if (log_special(x, &special)) return special;
    return (float)log_d(x);
}

float tide_log2_f(const float x)
{
    float special;
    if (log_special(x, &special)) return special;
    int e;
    double p;
    log_parts(x, &e, &p);
    return (float)((double)e + p * INV_LN2); // Exact for powers of two
}

float tide_log10_f(const float x)
{
    float special;
    if (log_special(x, &special)) return special;
    return (float)(log_d(x) * INV_LN10);
}

// Is y an integer, and if so, an odd one?
static bool is_integer_f(const float y, bool *odd)
{
    if (tide_trunc_f(y) != y) return false;
    // Every float at or above 2^24 is an even integer.
    *odd = tide_abs_f(y) < 16777216.0f && ((int32_t)y & 1) != 0;
    return true;
}

// pow with the C99 special cases.
float tide_pow_f(const float x, const float y)
{
    if (y == 0.0f) return 1.0f;
    if (x == 1.0f) return 1.0f;
    if (x != x || y != y) return x + y;

    bool odd = false;
    const bool y_integer = is_integer_f(y, &odd);
    const float ax = tide_abs_f(x);
    const bool y_inf = tide_abs_f(y) > 3.4028235e38f;

    if (y_inf) {
        if (ax == 1.0f) return 1.0f;
        return (ax < 1.0f) == (y < 0.0f) ? __builtin_inff() : 0.0f;
    }
    if (x == 0.0f) {
        const float magnitude = y < 0.0f ? __builtin_inff() : 0.0f;
        return odd ? tide_copysign_f(magnitude, x) : magnitude;
    }
    if (ax > 3.4028235e38f) { // x is +-inf
        const float magnitude = y < 0.0f ? 0.0f : __builtin_inff();
        return (x < 0.0f && odd) ? -magnitude : magnitude;
    }
    if (x < 0.0f && !y_integer) return __builtin_nanf("");

    const double r = exp_d((double)y * log_d(ax));
    return (float)((x < 0.0f && odd) ? -r : r);
}
