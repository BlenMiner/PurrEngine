// grids.tide's C: Glow calls it for each chunk it wakes for.

#include <stdint.h>

int32_t grids_visits;

void c_visit(const int32_t x, const int32_t y)
{
    (void)x;
    (void)y;
    __atomic_fetch_add(&grids_visits, 1, __ATOMIC_RELAXED);
}
