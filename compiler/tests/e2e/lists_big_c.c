// lists_big.tide's C functions: lists come as their elements, side by side.

#include <stdbool.h>
#include <stddef.h>

float c_big_sum(const float *values, const int count)
{
    float sum = 0;
    for (int i = 0; i < count; i++) sum += values[i];
    return sum;
}

void c_big_double(float *values, const int count)
{
    for (int i = 0; i < count; i++) values[i] *= 2;
}

bool c_big_is_null(const float *values)
{
    return values == NULL;
}
