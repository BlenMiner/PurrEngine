#include "purr_test.h"

// Guards the float rules in AGENTS.md. A failure here means a compiler flag is wrong.

PURR_TEST(fp_no_fma_contraction)
{
    // x*x is exactly 1 + 2^-29 + 2^-60. A separate multiply rounds the 2^-60 away,
    // so x*x - y is 0. A fused multiply-add keeps it and gives 2^-60.
    // volatile stops the compiler from folding this at compile time.
    volatile double vdx = 1.0 + 0x1p-30;
    volatile double vdy = 1.0 + 0x1p-29;
    double dx = vdx, dy = vdy;
    PURR_CHECK(dx * dx - dy == 0.0);

    // Same idea in single precision: x*x is 1 + 2^-12 + 2^-26.
    volatile float vfx = 1.0f + 0x1p-13f;
    volatile float vfy = 1.0f + 0x1p-12f;
    float fx = vfx, fy = vfy;
    PURR_CHECK(fx * fx - fy == 0.0f);
}
