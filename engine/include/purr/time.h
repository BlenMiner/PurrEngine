#pragma once

#include <stdint.h>

// Monotonic wall-clock time in nanoseconds, for benchmarks and the view layer.
// Not deterministic: simulation code must never read it.
uint64_t purr_time_now_ns(void);
