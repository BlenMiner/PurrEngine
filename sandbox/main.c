#include <stdio.h>

#include "purr/time.h"

// Playground for experiments.
int main(void)
{
    const uint64_t start = purr_time_now_ns();

    printf("PurrEngine sandbox\n");

    const uint64_t elapsed = purr_time_now_ns() - start;
    printf("took %llu ns\n", (unsigned long long)elapsed);
    return 0;
}
