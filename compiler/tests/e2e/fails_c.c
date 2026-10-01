// The C side of fails.tide: a counter, so the test sees which calls ran.
#include <stdint.h>

static int32_t ticks;

int32_t Tick(void)
{
    return ++ticks;
}

// Odd numbers fail.
int32_t TickFails(const int32_t n)
{
    return n % 2;
}

int32_t TickCount(void)
{
    return ticks;
}
