#include <inttypes.h>
#include <stdio.h>

#include "game.h"
#include "tide_test.h"

// The expected hash was produced natively; WebAssembly (and every other
// platform) must match it bit for bit. See tests/test_crossplatform.c.

static tide_world world;

static uint64_t hash_bits(uint64_t h, const uint32_t bits)
{
    for (int i = 0; i < 4; i++) {
        h ^= (bits >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t hash_float(const uint64_t h, const float x)
{
    return hash_bits(h, x != x ? 0x7FC00000u : tide_f_bits(x));
}

static uint64_t hash_world(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    h = hash_bits(h, (uint32_t)world.Spawner.spawned);
    h = hash_bits(h, (uint32_t)world.Spawner.seed);
    h = hash_bits(h, world.arch0_Body.count);
    for (uint32_t i = 0; i < world.arch0_Body.count; i++) {
        const Body *b = TIDE_AT(&world, arch0_Body, Body, i);
        const tide_entity e = TIDE_ENTITY_AT(&world, arch0_Body, i);
        h = hash_bits(h, e.index);
        h = hash_bits(h, e.generation);
        h = hash_float(hash_float(hash_float(h, b->position.x), b->position.y), b->position.z);
        h = hash_float(hash_float(hash_float(h, b->velocity.x), b->velocity.y), b->velocity.z);
        h = hash_float(hash_float(hash_float(hash_float(h, b->rotation.value.x), b->rotation.value.y),
                                  b->rotation.value.z), b->rotation.value.w);
        h = hash_float(h, b->phase);
        h = hash_bits(h, (uint32_t)b->counter);
    }
    return h;
}

TIDE_TEST(crossplatform_simulation)
{
    tide_world_init(&world, 1.0f / 60.0f);
    for (int i = 0; i < 1000; i++) tide_world_tick(&world);
    TIDE_CHECK(world.Spawner.spawned == 200);
    TIDE_CHECK(world.arch0_Body.count > 0 && world.arch0_Body.count < 200); // Some were retired

    const uint64_t h = hash_world();
    const uint64_t expected = 0x8D7E5A010DE69811ull;
    if (h != expected) printf("    simulation: hash is 0x%016" PRIX64 ", expected 0x%016" PRIX64 "\n", h, expected);
    TIDE_CHECK(h == expected);
}
