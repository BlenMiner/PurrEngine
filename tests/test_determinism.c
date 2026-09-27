#include "purr_test.h"

// Guards the float rules in AGENTS.md. A failure here means a compiler flag is wrong.

PURR_TEST(fp_no_fma_contraction)
{
    // x*x is exactly 1 + 2^-29 + 2^-60. A separate multiply rounds the 2^-60 away,
    // so x*x - y is 0. A fused multiply-add keeps it and gives 2^-60.
    // volatile stops the compiler from folding this at compile time.
    const volatile double vdx = 1.0 + 0x1p-30;
    const volatile double vdy = 1.0 + 0x1p-29;
    double dx = vdx, dy = vdy;
    PURR_CHECK(dx * dx - dy == 0.0);

    // Same idea in single precision: x*x is 1 + 2^-12 + 2^-26.
    const volatile float vfx = 1.0f + 0x1p-13f;
    const volatile float vfy = 1.0f + 0x1p-12f;
    float fx = vfx, fy = vfy;
    PURR_CHECK(fx * fx - fy == 0.0f);
}

// Denormals must not be flushed to zero on any platform: WebAssembly can't
// flush them, so everyone keeps them (see AGENTS.md).
PURR_TEST(fp_denormals_are_kept)
{
    const volatile float smallest_normal = 0x1p-126f;
    const volatile double smallest_normal_d = 0x1p-1022;
    PURR_CHECK(smallest_normal / 2.0f != 0.0f);
    PURR_CHECK(smallest_normal_d / 2.0 != 0.0);
}
