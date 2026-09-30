// Carrying a world over to a build whose data layout changed (purr/migrate.h):
// the rules, on an old and a new layout written by hand, then the layout purrc
// describes for platform/tests/migrate.purr.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "purr/migrate.h"
#include "purr_test.h"

extern const purr_layout purr_game_layout;

#define FIELD(T, f, type, kind, dim, decl) {#f, type, kind, dim, decl, offsetof(T, f), sizeof(((T *)0)->f)}
#define ROWS 4u

// ---------------------------------------------------------------------------
// The old build: enum Kind { A, B, C }; Ball, Tag and the scene Arena; Rules;
// the input In

enum { OLD_A, OLD_B, OLD_C };

typedef struct old_ball {
    int32_t hits;
    purr_float2 pos;
    int32_t kind;
    int32_t gone;
} old_ball;

typedef struct old_tag {
    int32_t v;
} old_tag;

typedef struct old_arena {
    int32_t size;
} old_arena;

typedef struct old_rules {
    int32_t lives;
    int32_t speed;
} old_rules;

typedef struct old_in {
    float move;
} old_in;

typedef struct old_balls {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    old_ball ball[ROWS];
} old_balls;

typedef struct old_tagged {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    old_ball ball[ROWS];
    old_tag tag[ROWS];
} old_tagged;

typedef struct old_arenas {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    old_arena arena[ROWS];
} old_arenas;

typedef struct old_tags {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    old_tag tag[ROWS];
} old_tags;

typedef struct old_world {
    old_rules rules;
    purr_entities entities;
    old_balls balls;
    old_tagged tagged;
    old_arenas arenas;
    old_tags tags;
    uint32_t command_count;
    old_in inputs[3];
    old_in previous[3];
} old_world;

static void old_ball_defaults(void *v)
{
    *(old_ball *)v = (old_ball){0};
}

static void old_tag_defaults(void *v)
{
    *(old_tag *)v = (old_tag){0};
}

static void old_arena_defaults(void *v)
{
    *(old_arena *)v = (old_arena){0};
}

static void old_rules_defaults(void *v)
{
    *(old_rules *)v = (old_rules){0};
}

static void old_in_defaults(void *v)
{
    *(old_in *)v = (old_in){0};
}

static const purr_layout_field old_ball_fields[] = {
    FIELD(old_ball, hits, "int", PURR_LAYOUT_INT, 1, -1),
    FIELD(old_ball, pos, "float2", PURR_LAYOUT_FLOAT, 2, -1),
    FIELD(old_ball, kind, "Kind", PURR_LAYOUT_ENUM, 0, 0),
    FIELD(old_ball, gone, "int", PURR_LAYOUT_INT, 1, -1),
};
static const purr_layout_field old_tag_fields[] = {FIELD(old_tag, v, "int", PURR_LAYOUT_INT, 1, -1)};
static const purr_layout_field old_arena_fields[] = {FIELD(old_arena, size, "int", PURR_LAYOUT_INT, 1, -1)};
static const purr_layout_field old_rules_fields[] = {
    FIELD(old_rules, lives, "int", PURR_LAYOUT_INT, 1, -1),
    FIELD(old_rules, speed, "int", PURR_LAYOUT_INT, 1, -1),
};
static const purr_layout_field old_in_fields[] = {FIELD(old_in, move, "float", PURR_LAYOUT_FLOAT, 1, -1)};

enum { OLD_BALL, OLD_TAG, OLD_ARENA, OLD_RULES, OLD_IN };
static const purr_layout_type old_types[] = {
    {"Ball", sizeof(old_ball), false, 4, old_ball_fields, old_ball_defaults},
    {"Tag", sizeof(old_tag), false, 1, old_tag_fields, old_tag_defaults},
    {"Arena", sizeof(old_arena), true, 1, old_arena_fields, old_arena_defaults},
    {"Rules", sizeof(old_rules), false, 2, old_rules_fields, old_rules_defaults},
    {"In", sizeof(old_in), false, 1, old_in_fields, old_in_defaults},
};

static const purr_layout_member old_kinds[] = {{"A", OLD_A}, {"B", OLD_B}, {"C", OLD_C}};
static const purr_layout_enum old_enums[] = {{"Kind", 3, old_kinds}};

static const purr_layout_place old_balls_columns[] = {{OLD_BALL, offsetof(old_balls, ball)}};
static const purr_layout_place old_tagged_columns[] = {{OLD_BALL, offsetof(old_tagged, ball)},
                                                       {OLD_TAG, offsetof(old_tagged, tag)}};
static const purr_layout_place old_arenas_columns[] = {{OLD_ARENA, offsetof(old_arenas, arena)}};
static const purr_layout_place old_tags_columns[] = {{OLD_TAG, offsetof(old_tags, tag)}};
static const purr_layout_archetype old_archetypes[] = {
    {offsetof(old_world, balls), offsetof(old_balls, count), offsetof(old_balls, entity), offsetof(old_balls, scene), 1,
     old_balls_columns},
    {offsetof(old_world, tagged), offsetof(old_tagged, count), offsetof(old_tagged, entity),
     offsetof(old_tagged, scene), 2, old_tagged_columns},
    {offsetof(old_world, arenas), offsetof(old_arenas, count), offsetof(old_arenas, entity),
     offsetof(old_arenas, scene), 1, old_arenas_columns},
    {offsetof(old_world, tags), offsetof(old_tags, count), offsetof(old_tags, entity), offsetof(old_tags, scene), 1,
     old_tags_columns},
};
static const purr_layout_place old_singletons[] = {{OLD_RULES, offsetof(old_world, rules)}};

static const purr_layout old_layout = {
    5, old_types, 1, old_enums,
    {sizeof(old_world), 1, old_singletons, offsetof(old_world, entities), 4, old_archetypes, ROWS,
     offsetof(old_world, command_count), OLD_IN, offsetof(old_world, inputs), offsetof(old_world, previous), 3,
     UINT32_MAX},
    {0},
};

// ---------------------------------------------------------------------------
// The new build: enum Kind { C, A }, without B. Ball's fields moved, hits is a
// float, gone an int2, and extra is new; Tag is gone. Rules has a new field and
// speed is a float. The input has jump.

enum { NEW_C, NEW_A };

typedef struct new_ball {
    purr_float2 pos;
    float hits;
    int32_t kind;
    purr_float3 extra;
    purr_int2 gone;
} new_ball;

typedef struct new_arena {
    int32_t size;
} new_arena;

typedef struct new_rules {
    int32_t added;
    int32_t lives;
    float speed;
} new_rules;

typedef struct new_in {
    float move;
    float jump;
} new_in;

typedef struct new_arenas {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    new_arena arena[ROWS];
} new_arenas;

typedef struct new_balls {
    uint32_t count;
    purr_entity entity[ROWS];
    purr_entity scene[ROWS];
    new_ball ball[ROWS];
} new_balls;

typedef struct new_world {
    purr_entities entities;
    new_arenas arenas;
    new_balls balls;
    new_rules rules;
    uint32_t command_count;
    new_in inputs[3];
    new_in previous[3];
} new_world;

static void new_ball_defaults(void *v)
{
    *(new_ball *)v = (new_ball){.kind = NEW_A, .extra = {7.0f, 7.0f, 7.0f}};
}

static void new_arena_defaults(void *v)
{
    *(new_arena *)v = (new_arena){0};
}

static void new_rules_defaults(void *v)
{
    *(new_rules *)v = (new_rules){.added = 3, .lives = 1};
}

static void new_in_defaults(void *v)
{
    *(new_in *)v = (new_in){.jump = 1.0f};
}

static const purr_layout_field new_ball_fields[] = {
    FIELD(new_ball, pos, "float2", PURR_LAYOUT_FLOAT, 2, -1),
    FIELD(new_ball, hits, "float", PURR_LAYOUT_FLOAT, 1, -1),
    FIELD(new_ball, kind, "Kind", PURR_LAYOUT_ENUM, 0, 0),
    FIELD(new_ball, extra, "float3", PURR_LAYOUT_FLOAT, 3, -1),
    FIELD(new_ball, gone, "int2", PURR_LAYOUT_INT, 2, -1),
};
static const purr_layout_field new_arena_fields[] = {FIELD(new_arena, size, "int", PURR_LAYOUT_INT, 1, -1)};
static const purr_layout_field new_rules_fields[] = {
    FIELD(new_rules, added, "int", PURR_LAYOUT_INT, 1, -1),
    FIELD(new_rules, lives, "int", PURR_LAYOUT_INT, 1, -1),
    FIELD(new_rules, speed, "float", PURR_LAYOUT_FLOAT, 1, -1),
};
static const purr_layout_field new_in_fields[] = {
    FIELD(new_in, move, "float", PURR_LAYOUT_FLOAT, 1, -1),
    FIELD(new_in, jump, "float", PURR_LAYOUT_FLOAT, 1, -1),
};

enum { NEW_ARENA, NEW_BALL, NEW_RULES, NEW_IN };
static const purr_layout_type new_types[] = {
    {"Arena", sizeof(new_arena), true, 1, new_arena_fields, new_arena_defaults},
    {"Ball", sizeof(new_ball), false, 5, new_ball_fields, new_ball_defaults},
    {"Rules", sizeof(new_rules), false, 3, new_rules_fields, new_rules_defaults},
    {"In", sizeof(new_in), false, 2, new_in_fields, new_in_defaults},
};

static const purr_layout_member new_kinds[] = {{"C", NEW_C}, {"A", NEW_A}};
static const purr_layout_enum new_enums[] = {{"Kind", 2, new_kinds}};

static const purr_layout_place new_arenas_columns[] = {{NEW_ARENA, offsetof(new_arenas, arena)}};
static const purr_layout_place new_balls_columns[] = {{NEW_BALL, offsetof(new_balls, ball)}};
static const purr_layout_archetype new_archetypes[] = {
    {offsetof(new_world, arenas), offsetof(new_arenas, count), offsetof(new_arenas, entity),
     offsetof(new_arenas, scene), 1, new_arenas_columns},
    {offsetof(new_world, balls), offsetof(new_balls, count), offsetof(new_balls, entity), offsetof(new_balls, scene), 1,
     new_balls_columns},
};
static const purr_layout_place new_singletons[] = {{NEW_RULES, offsetof(new_world, rules)}};

#define NEW_MATCH(archetypes, count)                                                                                \
    {sizeof(new_world), 1, new_singletons, offsetof(new_world, entities), count, archetypes, ROWS,                  \
     offsetof(new_world, command_count), NEW_IN, offsetof(new_world, inputs), offsetof(new_world, previous), 3,     \
     UINT32_MAX}

static const purr_layout new_layout = {4, new_types, 1, new_enums, NEW_MATCH(new_archetypes, 2), {0}};

// The same, but the scene Arena is gone: its type and its storage.
static const purr_layout no_arena_layout = {4, new_types, 1, new_enums, NEW_MATCH(new_archetypes + 1, 1), {0}};

// ---------------------------------------------------------------------------

typedef struct old_match {
    old_world *w;
    purr_entity arena, ball1, ball2, tag_only;
} old_match;

static purr_entity add_entity(old_world *w, const uint32_t archetype, const uint32_t row)
{
    const purr_entity e = purr_entity_create(&w->entities);
    purr_entity_set_location(&w->entities, e, (purr_location){archetype, row});
    return e;
}

// An arena with two balls in it, one of them tagged, and an entity that only
// has a tag: its components can't be together in the new build.
static old_match make_old(void)
{
    old_match m = {.w = calloc(1, sizeof(old_world))};
    old_world *w = m.w;
    const purr_entity gone = purr_entity_create(&w->entities); // A slot that's free again
    purr_entity_destroy(&w->entities, gone);
    m.arena = add_entity(w, 2, 0);
    w->arenas = (old_arenas){.count = 1, .entity = {m.arena}, .scene = {m.arena}, .arena = {{9}}};
    m.ball1 = add_entity(w, 0, 0);
    w->balls = (old_balls){.count = 1, .entity = {m.ball1}, .scene = {m.arena}, .ball = {{3, {1.0f, 2.0f}, OLD_C, 5}}};
    m.ball2 = add_entity(w, 1, 0);
    w->tagged = (old_tagged){.count = 1, .entity = {m.ball2}, .scene = {m.arena},
                             .ball = {{4, {3.0f, 4.0f}, OLD_B, 6}}, .tag = {{8}}};
    m.tag_only = add_entity(w, 3, 0);
    w->tags = (old_tags){.count = 1, .entity = {m.tag_only}, .tag = {{1}}};
    w->rules = (old_rules){.lives = 2, .speed = 3};
    for (int i = 0; i < 3; i++) {
        w->inputs[i].move = (float)i + 0.5f;
        w->previous[i].move = (float)i;
    }
    return m;
}

static bool same_entity(const purr_entity a, const purr_entity b)
{
    return a.index == b.index && a.generation == b.generation;
}

PURR_TEST(migrate_carries_a_world_over_by_name)
{
    const old_match old = make_old();
    new_world *n = calloc(1, sizeof *n);
    purr_migration m;
    PURR_REQUIRE(purr_migrate_world(&old_layout, &old_layout.match, old.w, &new_layout, &new_layout.match, n, &m));
    PURR_CHECK(m.failed[0] == '\0');
    PURR_CHECK(m.entities_dropped == 1); // The one with only a tag

    // Entities keep their IDs; the tagged ball moved in with the other
    PURR_REQUIRE(n->arenas.count == 1);
    PURR_CHECK(same_entity(n->arenas.entity[0], old.arena) && same_entity(n->arenas.scene[0], old.arena));
    PURR_CHECK(n->arenas.arena[0].size == 9);
    PURR_REQUIRE(n->balls.count == 2);
    PURR_CHECK(same_entity(n->balls.entity[0], old.ball1) && same_entity(n->balls.entity[1], old.ball2));
    PURR_CHECK(same_entity(n->balls.scene[0], old.arena) && same_entity(n->balls.scene[1], old.arena));
    const purr_location l1 = purr_entity_location(&n->entities, old.ball1);
    const purr_location l2 = purr_entity_location(&n->entities, old.ball2);
    const purr_location la = purr_entity_location(&n->entities, old.arena);
    PURR_CHECK(l1.archetype == 1 && l1.row == 0 && l2.archetype == 1 && l2.row == 1);
    PURR_CHECK(la.archetype == 0 && la.row == 0);
    PURR_CHECK(!purr_entity_alive(&n->entities, old.tag_only));
    PURR_CHECK(n->entities.next_unused == old.w->entities.next_unused);

    // Fields by name: moved, converted, by the member's name, new, and reset
    const new_ball *b1 = &n->balls.ball[0];
    const new_ball *b2 = &n->balls.ball[1];
    PURR_CHECK(b1->pos.x == 1.0f && b1->pos.y == 2.0f && b1->hits == 3.0f);
    PURR_CHECK(b2->pos.x == 3.0f && b2->pos.y == 4.0f && b2->hits == 4.0f);
    PURR_CHECK(b1->kind == NEW_C); // C is still there, as another number
    PURR_CHECK(b2->kind == NEW_A); // B is gone: the default
    PURR_CHECK(b1->extra.x == 7.0f && b1->extra.z == 7.0f);
    PURR_CHECK(b1->gone.x == 0 && b1->gone.y == 0); // int to int2 doesn't convert

    // Singletons and inputs the same way
    PURR_CHECK(n->rules.lives == 2 && n->rules.speed == 3.0f && n->rules.added == 3);
    for (int i = 0; i < 3; i++) {
        PURR_CHECK(n->inputs[i].move == (float)i + 0.5f && n->inputs[i].jump == 1.0f);
        PURR_CHECK(n->previous[i].move == (float)i && n->previous[i].jump == 1.0f);
    }
    PURR_CHECK(purr_layout_fields_reset(&old_layout, &new_layout) == 1); // Ball.gone
    free(old.w);
    free(n);
}

PURR_TEST(migrate_refuses_a_world_whose_scene_is_gone)
{
    const old_match old = make_old();
    new_world *n = calloc(1, sizeof *n);
    purr_migration m;
    PURR_CHECK(!purr_migrate_world(&old_layout, &old_layout.match, old.w, &no_arena_layout, &no_arena_layout.match, n,
                                   &m));
    PURR_CHECK(strstr(m.failed, "Arena") != NULL);
    free(old.w);
    free(n);
}

PURR_TEST(migrate_refuses_a_world_with_changes_waiting)
{
    const old_match old = make_old();
    old.w->command_count = 1;
    new_world *n = calloc(1, sizeof *n);
    purr_migration m;
    PURR_CHECK(!purr_migrate_world(&old_layout, &old_layout.match, old.w, &new_layout, &new_layout.match, n, &m));
    PURR_CHECK(m.failed[0] != '\0');
    free(old.w);
    free(n);
}

// ---------------------------------------------------------------------------
// The layout purrc describes

static const purr_layout_type *type_named(const purr_layout *l, const char *name)
{
    for (uint32_t i = 0; i < l->type_count; i++) {
        if (strcmp(l->types[i].name, name) == 0) return &l->types[i];
    }
    return NULL;
}

// The Unit named `name`, or NULL.
static Unit *unit_named(purr_world *w, const char *name)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const purr_entity e = {i, w->entities.slots[i].generation};
        Unit *u = (e.generation & 1u) ? purr_get_Unit(w, e) : NULL;
        const purr_str text = u ? purr_text_read(&w->heap, u->name) : PURR_STR_EMPTY;
        if (u && text.bytes == (int32_t)strlen(name) && memcmp(text.ptr, name, strlen(name)) == 0) return u;
    }
    return NULL;
}

PURR_TEST(migrate_a_game_to_its_own_layout_changes_nothing)
{
    const purr_layout *l = &purr_game_layout;
    purr_world *w = calloc(1, sizeof *w);
    purr_world *copy = calloc(1, sizeof *copy);
    purr_world_init(w, 1.0f / 60.0f);
    purr_world_set_input(w, purr_player_from_index(0), (Controls){.move = {0.5f, -1.0f}});
    purr_world_tick(w);
    purr_migration m;
    PURR_REQUIRE(purr_migrate_world(l, &l->match, w, l, &l->match, copy, &m));
    PURR_CHECK(m.entities_dropped == 0);
    PURR_CHECK(memcmp(w, copy, sizeof *w) == 0);

    purr_local *local = calloc(1, sizeof *local);
    purr_local *local_copy = calloc(1, sizeof *local_copy);
    purr_local_init(local);
    PURR_REQUIRE(purr_migrate_world(l, &l->local, local, l, &l->local, local_copy, &m));
    PURR_CHECK(memcmp(local, local_copy, sizeof *local) == 0);
    PURR_CHECK(purr_layout_fields_reset(l, l) == 0);
    free(w);
    free(copy);
    free(local);
    free(local_copy);
}

PURR_TEST(migrate_a_games_defaults_are_its_declared_ones)
{
    const purr_layout_type *t = type_named(&purr_game_layout, "Unit");
    PURR_REQUIRE(t != NULL);
    PURR_REQUIRE(t->size == sizeof(Unit));
    Unit u;
    memset(&u, 0xFF, sizeof u);
    const uint32_t mark = purr_scratch_mark();
    t->defaults(&u);
    PURR_CHECK(purr_scratch_mark() == mark); // The borrowed default text is gone again
    PURR_CHECK(u.position.x == 0.0f && u.position.y == 0.0f);
    PURR_CHECK(u.mode == Mode_Run);
    PURR_CHECK(u.stats.level == 2 && u.stats.speed == 1.5f);
    PURR_CHECK(u.name.at == 0); // Text starts empty
    PURR_CHECK(purr_entity_is_null(u.target));
    const purr_layout_type *score = type_named(&purr_game_layout, "Score");
    PURR_REQUIRE(score != NULL);
    Score s;
    score->defaults(&s);
    PURR_CHECK(s.points == 10);
}

// A build where Unit's `position` was called `place`: the rest of Unit still
// carries over, text included.
PURR_TEST(migrate_a_game_keeps_what_has_the_same_name)
{
    const purr_layout *l = &purr_game_layout;
    purr_layout_type *types = malloc(l->type_count * sizeof *types);
    memcpy(types, l->types, l->type_count * sizeof *types);
    purr_layout old = *l;
    old.types = types;
    purr_layout_field *fields = NULL;
    for (uint32_t i = 0; i < old.type_count; i++) {
        if (strcmp(types[i].name, "Unit") != 0) continue;
        fields = malloc(types[i].field_count * sizeof *fields);
        memcpy(fields, types[i].fields, types[i].field_count * sizeof *fields);
        for (uint32_t k = 0; k < types[i].field_count; k++) {
            if (strcmp(fields[k].name, "position") == 0) fields[k].name = "place";
        }
        types[i].fields = fields;
    }
    PURR_REQUIRE(fields != NULL);

    purr_world *w = calloc(1, sizeof *w);
    purr_world *copy = calloc(1, sizeof *copy);
    purr_world_init(w, 1.0f / 60.0f);
    Unit *before = unit_named(w, "first");
    PURR_REQUIRE(before != NULL);
    before->mode = Mode_Jump;
    purr_migration m;
    PURR_REQUIRE(purr_migrate_world(&old, &old.match, w, l, &l->match, copy, &m));
    const Unit *after = unit_named(copy, "first");
    PURR_REQUIRE(after != NULL);
    PURR_CHECK(before->position.x == 1.0f && after->position.x == 0.0f); // Another name: the default
    PURR_CHECK(after->mode == Mode_Jump);
    PURR_CHECK(after->stats.level == 2);
    PURR_CHECK(purr_world_entity_count(copy) == purr_world_entity_count(w));
    free(fields);
    free(types);
    free(w);
    free(copy);
}
