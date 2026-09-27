#if !defined(_WIN32)
#define _POSIX_C_SOURCE 199309L
#endif

#include "purr/time.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t purr_time_now_ns(void)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    // Split into whole seconds and remainder so the multiply can't overflow.
    uint64_t f = (uint64_t)freq.QuadPart;
    uint64_t c = (uint64_t)now.QuadPart;
    return (c / f) * 1000000000ull + (c % f) * 1000000000ull / f;
}

#else

#include <time.h>

uint64_t purr_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
