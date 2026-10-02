// N-body in C: an array for each coordinate, every body's pull worked out on
// as many threads as there are, then every move. Then the same four bodies at
// a time with SIMD, each lane doing exactly what one body does alone, so the
// results stay the same to the bit. (On the web, which is built without
// WebAssembly's SIMD, the four lanes go one at a time.)

#include <stdbool.h>
#include <stdlib.h>

#include "tide/math.h" // tide_hash_i2: the start Tide's Math.Hash gives
#include "tide/page.h" // tide_alloc: memory every thread can use on the web
#include "versus.h"

#define SOFTENING 0.01f

typedef float f4 __attribute__((ext_vector_type(4)));
typedef float f4u __attribute__((ext_vector_type(4), aligned(4))); // At any float

#if __has_builtin(__builtin_elementwise_sqrt)
#define SQRT4(v) __builtin_elementwise_sqrt(v)
#else
static f4 SQRT4(const f4 v)
{
    return (f4){__builtin_sqrtf(v.x), __builtin_sqrtf(v.y), __builtin_sqrtf(v.z), __builtin_sqrtf(v.w)};
}
#endif

typedef struct nbody {
    uint32_t count;
    bool simd;
    float *block; // The arrays, one after the other
    float *x, *y, *z, *vx, *vy, *vz, *mass;
} nbody;

static void *make(const versus_flag *flags, const bool simd)
{
    nbody *b = tide_alloc_zeroed(1, sizeof *b);
    b->count = flags[0].value;
    b->simd = simd;
    return b;
}

static void *make_plain(const versus_flag *flags)
{
    return make(flags, false);
}

static void *make_simd(const versus_flag *flags)
{
    return make(flags, true);
}

static void setup(nbody *b)
{
    // Apart by a few cache lines more than their size (see particles.c)
    const size_t stride = ((size_t)b->count + 15u) / 16u * 16u + 64u * 3u;
    b->block = tide_alloc_zeroed(7u * stride, sizeof(float));
    float **arrays[7] = {&b->x, &b->y, &b->z, &b->vx, &b->vy, &b->vz, &b->mass};
    for (uint32_t a = 0; a < 7u; a++) *arrays[a] = b->block + a * stride;
    for (uint32_t i = 0; i < b->count; i++) {
        const int32_t h0 = tide_hash_i2((tide_int2){(int32_t)i, 0});
        const int32_t h1 = tide_hash_i2((tide_int2){(int32_t)i, 1});
        const int32_t h2 = tide_hash_i2((tide_int2){(int32_t)i, 2});
        b->x[i] = (float)(h0 % 2001 - 1000) * 0.01f;
        b->y[i] = (float)(h1 % 2001 - 1000) * 0.01f;
        b->z[i] = (float)(h2 % 2001 - 1000) * 0.01f;
        b->mass[i] = 1.0f + (float)(h0 % 100) * 0.01f;
    }
}

typedef struct pull_job {
    const nbody *b;
    uint32_t pieces;
} pull_job;

// Body i's pull from every body, itself too (from where it is, that's none).
static void pull_one(const nbody *b, const uint32_t i)
{
    const float *restrict x = b->x, *restrict y = b->y, *restrict z = b->z, *restrict mass = b->mass;
    const float xi = x[i], yi = y[i], zi = z[i];
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    for (uint32_t j = 0; j < b->count; j++) {
        const float dx = x[j] - xi, dy = y[j] - yi, dz = z[j] - zi;
        const float r2 = dx * dx + dy * dy + dz * dz + SOFTENING;
        const float s = mass[j] / (r2 * __builtin_sqrtf(r2));
        ax += dx * s;
        ay += dy * s;
        az += dz * s;
    }
    const float dt = VERSUS_DT;
    b->vx[i] += ax * dt;
    b->vy[i] += ay * dt;
    b->vz[i] += az * dt;
}

// Bodies i to i + 3's.
static void pull_four(const nbody *b, const uint32_t i)
{
    const float *restrict x = b->x, *restrict y = b->y, *restrict z = b->z, *restrict mass = b->mass;
    const f4 xi = *(const f4u *)&x[i], yi = *(const f4u *)&y[i], zi = *(const f4u *)&z[i];
    f4 ax = 0.0f, ay = 0.0f, az = 0.0f;
    for (uint32_t j = 0; j < b->count; j++) {
        const f4 dx = x[j] - xi, dy = y[j] - yi, dz = z[j] - zi;
        const f4 r2 = dx * dx + dy * dy + dz * dz + SOFTENING;
        const f4 s = mass[j] / (r2 * SQRT4(r2));
        ax += dx * s;
        ay += dy * s;
        az += dz * s;
    }
    const float dt = VERSUS_DT;
    *(f4u *)&b->vx[i] += ax * dt;
    *(f4u *)&b->vy[i] += ay * dt;
    *(f4u *)&b->vz[i] += az * dt;
}

static void pull(void *context, const uint32_t piece)
{
    const pull_job *j = context;
    const nbody *b = j->b;
    if (b->simd) { // Pieces of whole fours, and the last one has what's left
        const uint32_t fours = b->count / 4u;
        const uint32_t from = (uint32_t)((uint64_t)fours * piece / j->pieces) * 4u;
        const uint32_t to = piece + 1u == j->pieces ? b->count : (uint32_t)((uint64_t)fours * (piece + 1u) / j->pieces) * 4u;
        uint32_t i = from;
        for (; i + 4u <= to; i += 4u) pull_four(b, i);
        for (; i < to; i++) pull_one(b, i);
    } else {
        const uint32_t from = (uint32_t)((uint64_t)b->count * piece / j->pieces);
        const uint32_t to = (uint32_t)((uint64_t)b->count * (piece + 1u) / j->pieces);
        for (uint32_t i = from; i < to; i++) pull_one(b, i);
    }
}

static void tick(void *state, const tide_jobs *jobs)
{
    nbody *b = state;
    if (!b->block) setup(b); // Tide's first tick makes them, then moves them
    // Threads pay from about 256 bodies, or 512 with SIMD (on a 12-core Ryzen
    // 9 7900X)
    pull_job j = {b, versus_pieces(jobs, b->count, b->simd ? 512u : 256u)};
    versus_for(jobs, j.pieces, pull, &j);
    const float dt = VERSUS_DT;
    for (uint32_t i = 0; i < b->count; i++) {
        b->x[i] += b->vx[i] * dt;
        b->y[i] += b->vy[i] * dt;
        b->z[i] += b->vz[i] * dt;
    }
}

static uint64_t digest(const void *state)
{
    const nbody *b = state;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        const float item[7] = {b->x[i], b->y[i], b->z[i], b->vx[i], b->vy[i], b->vz[i], b->mass[i]};
        sum += versus_item(item, sizeof item);
    }
    return sum;
}

static void destroy(void *state)
{
    nbody *b = state;
    free(b->block);
    free(b);
}

const versus_way nbody_c = {"C", make_plain, tick, digest, destroy};
const versus_way nbody_c_simd = {"C, SIMD", make_simd, tick, digest, destroy};
