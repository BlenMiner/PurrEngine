#include "game.h"
#include "tide_test.h"

static tide_world world;

// The index-th entity Main's Setup spawns: the Main scene itself is the first entity.
static tide_entity slot(const uint32_t index)
{
    return (tide_entity){index + 1, 1};
}

static int32_t thing_id(const uint32_t index)
{
    if (!tide_has_Thing(&world, slot(index))) return -1;
    return tide_get_Thing(&world, slot(index)).id;
}

TIDE_TEST(evaluation_order_siblings_left_to_right)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(thing_id(0) == 1);
    TIDE_CHECK(thing_id(1) == 2);
    const Pair p = tide_get_Pair(&world, slot(2));
    TIDE_REQUIRE(tide_has_Pair(&world, slot(2)));
    TIDE_CHECK(tide_entity_equal(p.a, slot(0)));
    TIDE_CHECK(tide_entity_equal(p.b, slot(1)));
}

TIDE_TEST(evaluation_order_follows_source_not_field_order)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(thing_id(3) == 4);
    TIDE_CHECK(thing_id(4) == 3);
    const Pair p = tide_get_Pair(&world, slot(5));
    TIDE_REQUIRE(tide_has_Pair(&world, slot(5)));
    TIDE_CHECK(tide_entity_equal(p.a, slot(4)));
    TIDE_CHECK(tide_entity_equal(p.b, slot(3)));
}

TIDE_TEST(evaluation_order_method_object_before_arguments)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(thing_id(6) == 5);
    TIDE_CHECK(thing_id(7) == 6);
    const Link l = tide_get_Link(&world, slot(6));
    TIDE_REQUIRE(tide_has_Link(&world, slot(6)));
    TIDE_CHECK(tide_entity_equal(l.target, slot(7)));
}
