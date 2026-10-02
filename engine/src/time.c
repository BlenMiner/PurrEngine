// clock_gettime under strict C. macOS declares it anyway, and would hide it.
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 199309L
#endif

#include "tide/time.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t tide_time_now_ns(void)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    // Split into whole seconds and remainder so the multiply can't overflow.
    const uint64_t f = (const uint64_t)freq.QuadPart;
    const uint64_t c = (const uint64_t)now.QuadPart;
    return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
}

void tide_yield(void)
{
    SwitchToThread();
}

#else

#include <sched.h>
#include <time.h>

void tide_yield(void)
{
    sched_yield();
}

uint64_t tide_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
