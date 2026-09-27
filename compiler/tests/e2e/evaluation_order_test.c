#include "game.h"
#include "purr_test.h"

static purr_world world;

static purr_entity slot(const uint32_t index)
{
    return (purr_entity){index, 1};
}

static int32_t thing_id(const uint32_t index)
{
    const Thing *t = purr_get_Thing(&world, slot(index));
    return t ? t->id : -1;
}

PURR_TEST(evaluation_order_siblings_left_to_right)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(thing_id(0) == 1);
    PURR_CHECK(thing_id(1) == 2);
    const Pair *p = purr_get_Pair(&world, slot(2));
    PURR_REQUIRE(p != NULL);
    PURR_CHECK(purr_entity_equal(p->a, slot(0)));
    PURR_CHECK(purr_entity_equal(p->b, slot(1)));
}

PURR_TEST(evaluation_order_follows_source_not_field_order)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(thing_id(3) == 4);
    PURR_CHECK(thing_id(4) == 3);
    const Pair *p = purr_get_Pair(&world, slot(5));
    PURR_REQUIRE(p != NULL);
    PURR_CHECK(purr_entity_equal(p->a, slot(4)));
    PURR_CHECK(purr_entity_equal(p->b, slot(3)));
}

PURR_TEST(evaluation_order_method_object_before_arguments)
{
    purr_world_init(&world, 1.0f);
    PURR_CHECK(thing_id(6) == 5);
    PURR_CHECK(thing_id(7) == 6);
    const Link *l = purr_get_Link(&world, slot(6));
    PURR_REQUIRE(l != NULL);
    PURR_CHECK(purr_entity_equal(l->target, slot(7)));
}
