#pragma once

#include <stdint.h>

// Monotonic wall-clock time in nanoseconds, for benchmarks and the view layer.
// Not deterministic: simulation code must never read it.
uint64_t tide_time_now_ns(void);

// Gives the rest of this thread's time slice to another thread that's ready.
void tide_yield(void);
