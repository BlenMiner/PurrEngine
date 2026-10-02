#include <math.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static const tide_entity THING = {1, 1};

static bool near(const float a, const float b)
{
    return fabsf(a - b) <= 1e-5f * fmaxf(1.0f, fabsf(b));
}

static bool eq3(const tide_float3 v, const float x, const float y, const float z)
{
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

static Results *results(void)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    return tide_get_Results(&world, THING);
}

TIDE_TEST(math_defaults)
{
    tide_world_init(&world, 1.0f);
    const Body *b = tide_get_Body(&world, THING);
    TIDE_REQUIRE(b != NULL);
    TIDE_CHECK(eq3(b->position, 1, 2, 3));
    TIDE_CHECK(b->color.y == 0.5f && b->color.w == 1.0f);
    TIDE_CHECK(b->cell.x == 3 && b->cell.y == -4);
    TIDE_CHECK(b->rotation.value.w == 1.0f && b->rotation.value.x == 0.0f);
    TIDE_CHECK(b->basis.c0.x == 1.0f && b->basis.c1.y == 1.0f && b->basis.c0.y == 0.0f);
    TIDE_CHECK(near(b->angle, TIDE_PI_F / 2.0f));
    TIDE_CHECK(b->pi == TIDE_PI_F);
}

TIDE_TEST(math_constructors_and_swizzles)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(eq3(r->swizzled, 3, 2, 1));
    TIDE_CHECK(r->built.x == 1 && r->built.y == 2 && r->built.z == 7 && r->built.w == 8);
    TIDE_CHECK(eq3(r->splat, 2, 2, 2));
    const Body *b = tide_get_Body(&world, THING);
    TIDE_CHECK(b->flat.x == 1 && b->flat.y == 3);
}

TIDE_TEST(math_operators)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(eq3(r->scaled, 3, 5, 7));
    TIDE_CHECK(r->halved.x == 3 && r->halved.y == -3 && r->halved.z == 4);
    TIDE_CHECK(eq3(r->widened, 3.5f, -3.5f, 3.5f));
    TIDE_CHECK(eq3(r->negated, -1, -2, -3));
}

TIDE_TEST(math_conversions_saturate)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->truncated.x == 1 && r->truncated.y == -1 && r->truncated.z == INT32_MAX);
    TIDE_CHECK(r->converted == -2);
}

TIDE_TEST(math_functions)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->dotted == 6.0f);
    TIDE_CHECK(r->length == 13.0f);
    TIDE_CHECK(eq3(r->normalized, 0, 1, 0));
    TIDE_CHECK(eq3(r->crossed, 0, 0, 1));
    TIDE_CHECK(r->clamped == 1);
    TIDE_CHECK(eq3(r->lerped, 5, 10, 15));
    TIDE_CHECK(r->sine == 1.0f);
    TIDE_CHECK(near(r->angle45, 45.0f));
    TIDE_CHECK(r->power == 1024.0f);
}

TIDE_TEST(math_quaternions_and_matrices)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(eq3(r->rotated, 0, 0, -1));
    TIDE_CHECK(eq3(r->transformed, 3, 4, 5));
    TIDE_CHECK(near(r->determinant, 8.0f));
    TIDE_CHECK(r->matrixFromRows.c0.x == 1 && r->matrixFromRows.c1.x == 2);
    TIDE_CHECK(r->matrixFromRows.c0.y == 3 && r->matrixFromRows.c1.y == 4);
    TIDE_CHECK(eq3(r->columnScaled, 0, 3, 0));
    const Body *b = tide_get_Body(&world, THING);
    TIDE_CHECK(near(b->rotation.value.y, sinf(TIDE_PI_F / 4.0f)));
}

TIDE_TEST(math_swizzle_writes)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    // w = (0,0,0); w.xz = (4,5) -> (4,0,5); w.y += 1 -> (4,1,5); w.zx += (1,1) -> (5,1,6)
    TIDE_CHECK(eq3(r->written, 5, 1, 6));
}

TIDE_TEST(math_hash)
{
    const Results *r = results();
    TIDE_REQUIRE(r != NULL);
    TIDE_CHECK(r->hashed == tide_hash_i(-7));
    TIDE_CHECK(r->hashed2 == tide_hash_i2(tide_i2(3, -4)));
    TIDE_CHECK(r->hashed3 == tide_hash_i3(tide_i3(3, -4, 9)));
    TIDE_CHECK(r->hashed4 == tide_hash_i4(tide_i4(1, 2, 3, 4)));
    TIDE_CHECK(r->hashed2 != r->hashed3);
}
