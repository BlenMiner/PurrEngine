#include <math.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;

static const purr_entity THING = {0, 1};

static bool near(const float a, const float b)
{
    return fabsf(a - b) <= 1e-5f * fmaxf(1.0f, fabsf(b));
}

static bool eq3(const purr_float3 v, const float x, const float y, const float z)
{
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

static Results *results(void)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    return purr_get_Results(&world, THING);
}

PURR_TEST(math_defaults)
{
    purr_world_init(&world, 1.0f);
    const Body *b = purr_get_Body(&world, THING);
    PURR_REQUIRE(b != NULL);
    PURR_CHECK(eq3(b->position, 1, 2, 3));
    PURR_CHECK(b->color.y == 0.5f && b->color.w == 1.0f);
    PURR_CHECK(b->cell.x == 3 && b->cell.y == -4);
    PURR_CHECK(b->rotation.value.w == 1.0f && b->rotation.value.x == 0.0f);
    PURR_CHECK(b->basis.c0.x == 1.0f && b->basis.c1.y == 1.0f && b->basis.c0.y == 0.0f);
    PURR_CHECK(near(b->angle, PURR_PI_F / 2.0f));
    PURR_CHECK(b->pi == PURR_PI_F);
}

PURR_TEST(math_constructors_and_swizzles)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(eq3(r->swizzled, 3, 2, 1));
    PURR_CHECK(r->built.x == 1 && r->built.y == 2 && r->built.z == 7 && r->built.w == 8);
    PURR_CHECK(eq3(r->splat, 2, 2, 2));
    const Body *b = purr_get_Body(&world, THING);
    PURR_CHECK(b->flat.x == 1 && b->flat.y == 3);
}

PURR_TEST(math_operators)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(eq3(r->scaled, 3, 5, 7));
    PURR_CHECK(r->halved.x == 3 && r->halved.y == -3 && r->halved.z == 4);
    PURR_CHECK(eq3(r->widened, 3.5f, -3.5f, 3.5f));
    PURR_CHECK(eq3(r->negated, -1, -2, -3));
}

PURR_TEST(math_conversions_saturate)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->truncated.x == 1 && r->truncated.y == -1 && r->truncated.z == INT32_MAX);
    PURR_CHECK(r->converted == -2);
}

PURR_TEST(math_functions)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(r->dotted == 6.0f);
    PURR_CHECK(r->length == 13.0f);
    PURR_CHECK(eq3(r->normalized, 0, 1, 0));
    PURR_CHECK(eq3(r->crossed, 0, 0, 1));
    PURR_CHECK(r->clamped == 1);
    PURR_CHECK(eq3(r->lerped, 5, 10, 15));
    PURR_CHECK(r->sine == 1.0f);
    PURR_CHECK(near(r->angle45, 45.0f));
    PURR_CHECK(r->power == 1024.0f);
}

PURR_TEST(math_quaternions_and_matrices)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    PURR_CHECK(eq3(r->rotated, 0, 0, -1));
    PURR_CHECK(eq3(r->transformed, 3, 4, 5));
    PURR_CHECK(near(r->determinant, 8.0f));
    PURR_CHECK(r->matrixFromRows.c0.x == 1 && r->matrixFromRows.c1.x == 2);
    PURR_CHECK(r->matrixFromRows.c0.y == 3 && r->matrixFromRows.c1.y == 4);
    PURR_CHECK(eq3(r->columnScaled, 0, 3, 0));
    const Body *b = purr_get_Body(&world, THING);
    PURR_CHECK(near(b->rotation.value.y, sinf(PURR_PI_F / 4.0f)));
}

PURR_TEST(math_swizzle_writes)
{
    const Results *r = results();
    PURR_REQUIRE(r != NULL);
    // w = (0,0,0); w.xz = (4,5) -> (4,0,5); w.y += 1 -> (4,1,5); w.zx += (1,1) -> (5,1,6)
    PURR_CHECK(eq3(r->written, 5, 1, 6));
}
