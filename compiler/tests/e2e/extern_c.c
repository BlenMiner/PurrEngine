// The C side of extern.purr: plain C, as a library would be, declaring its
// own types with the same fields as the game's.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct vec3 {
    float x, y, z;
} vec3;

typedef struct pair {
    float a;
    int b;
} pair;

static int calls;

float c_add(const float a, const float b)
{
    calls++;
    return a + b;
}

int c_twice(const int x)
{
    return x * 2;
}

vec3 c_scale(const vec3 v, const float s)
{
    return (vec3){v.x * s, v.y * s, v.z * s};
}

pair c_swap(const pair p)
{
    return (pair){(float)p.b, (int)p.a};
}

void c_bump(int *counter, pair *p)
{
    *counter += 1;
    p->a += 0.5f;
    p->b += 1;
}

int c_next(const int mode)
{
    return mode + 1;
}

bool c_is_even(const int x)
{
    return x % 2 == 0;
}

float half_of(const float x)
{
    return x * 0.5f;
}

int c_calls(void)
{
    return calls;
}

// Only basic operations: 3-4-5 triangles have exact lengths.
float c_length(const vec3 *v)
{
    const float squared = v->x * v->x + v->y * v->y + v->z * v->z;
    float length = 0;
    while (length * length < squared) length += 1;
    return length;
}

float c_pair_sum(const pair *p)
{
    return p->a + (float)p->b;
}

float c_sum(const float *values, const int count)
{
    float sum = 0;
    for (int i = 0; i < count; i++) sum += values[i];
    return sum;
}

void c_double_all(int *values, const int count)
{
    for (int i = 0; i < count; i++) values[i] *= 2;
}

bool c_is_null(const int *values)
{
    return values == NULL;
}

int c_text_bytes(const char *text)
{
    return (int)strlen(text);
}

const char *c_greet(const char *name)
{
    static char buffer[64];
    snprintf(buffer, sizeof buffer, "hello, %s", name);
    return buffer;
}

const char *c_nothing(void)
{
    return NULL;
}

static int ticks;
static int loop_ticks;
static int return_ticks;

int c_tick(void)
{
    return ++ticks;
}

int c_digits(const int a, const int b)
{
    return a * 10 + b;
}

int c_loop_tick(void)
{
    return ++loop_ticks;
}

int c_return_tick(void)
{
    return ++return_ticks;
}

static int unit_ticks;
static int loop2_ticks;
static int op_ticks;

int c_unit_tick(void)
{
    return ++unit_ticks;
}

int c_loop2_tick(void)
{
    return ++loop2_ticks;
}

int c_op_tick(void)
{
    return ++op_ticks;
}

static int spawn_ticks;
static int each_ticks;

int c_spawn_tick(void)
{
    return ++spawn_ticks;
}

int c_each_tick(void)
{
    return ++each_ticks;
}
