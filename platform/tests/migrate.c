// Carrying a world over to a build whose data layout changed (tide/migrate.h):
// the rules, on an old and a new layout written by hand, then the layout tidec
// describes for platform/tests/migrate.tide.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide/migrate.h"
#include "tide_test.h"

extern const tide_layout tide_game_layout;

#define FIELD(T, f, type, kind, dim, decl) {#f, type, kind, dim, decl, offsetof(T, f), sizeof(((T *)0)->f)}

// ---------------------------------------------------------------------------
// The old build: enum Kind { A, B, C }; Ball, Tag and the scene Arena; Rules;
// the input In. Its archetypes: Ball; Ball and Tag; Arena; Tag. Each has the
// entity column, then the scene's, then its components' lanes: 4 bytes each.

enum { OLD_A, OLD_B, OLD_C };

typedef struct old_ball {
    int32_t hits;
    tide_float2 pos;
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

// The struct its world's bytes start with
typedef struct old_world {
    old_rules rules;
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

static const tide_layout_field old_ball_fields[] = {
    FIELD(old_ball, hits, "int", TIDE_LAYOUT_INT, 1, -1),
    FIELD(old_ball, pos, "float2", TIDE_LAYOUT_FLOAT, 2, -1),
    FIELD(old_ball, kind, "Kind", TIDE_LAYOUT_ENUM, 0, 0),
    FIELD(old_ball, gone, "int", TIDE_LAYOUT_INT, 1, -1),
};
static const tide_layout_field old_tag_fields[] = {FIELD(old_tag, v, "int", TIDE_LAYOUT_INT, 1, -1)};
static const tide_layout_field old_arena_fields[] = {FIELD(old_arena, size, "int", TIDE_LAYOUT_INT, 1, -1)};
static const tide_layout_field old_rules_fields[] = {
    FIELD(old_rules, lives, "int", TIDE_LAYOUT_INT, 1, -1),
    FIELD(old_rules, speed, "int", TIDE_LAYOUT_INT, 1, -1),
};
static const tide_layout_field old_in_fields[] = {FIELD(old_in, move, "float", TIDE_LAYOUT_FLOAT, 1, -1)};

enum { OLD_BALL, OLD_TAG, OLD_ARENA, OLD_RULES, OLD_IN };
static const tide_layout_type old_types[] = {
    {"Ball", sizeof(old_ball), false, 4, old_ball_fields, old_ball_defaults},
    {"Tag", sizeof(old_tag), false, 1, old_tag_fields, old_tag_defaults},
    {"Arena", sizeof(old_arena), true, 1, old_arena_fields, old_arena_defaults},
    {"Rules", sizeof(old_rules), false, 2, old_rules_fields, old_rules_defaults},
    {"In", sizeof(old_in), false, 1, old_in_fields, old_in_defaults},
};

static const tide_layout_member old_kinds[] = {{"A", OLD_A}, {"B", OLD_B}, {"C", OLD_C}};
static const tide_layout_enum old_enums[] = {{"Kind", 3, old_kinds}};

static const tide_layout_place old_balls_columns[] = {{OLD_BALL, 2}};
static const tide_layout_place old_tagged_columns[] = {{OLD_BALL, 2}, {OLD_TAG, 7}}; // Ball's 5 lanes, then Tag's
static const tide_layout_place old_arenas_columns[] = {{OLD_ARENA, 2}};
static const tide_layout_place old_tags_columns[] = {{OLD_TAG, 2}};
static const tide_layout_archetype old_archetypes[] = {
    {true, 1, old_balls_columns},
    {true, 2, old_tagged_columns},
    {true, 1, old_arenas_columns},
    {true, 1, old_tags_columns},
};
static const tide_layout_place old_singletons[] = {{OLD_RULES, offsetof(old_world, rules)}};

static const tide_layout old_layout = {
    5, old_types, 1, old_enums,
    {sizeof(old_world), 1, old_singletons, 4, old_archetypes, OLD_IN, offsetof(old_world, inputs),
     offsetof(old_world, previous), 3, false, 0, NULL, 0, 0},
    {0},
};

// ---------------------------------------------------------------------------
// The new build: enum Kind : byte { C, A }, without B. Ball's fields moved, hits
// is a float, gone an int2, and extra is new; Tag is gone. Rules has a new field
// and speed is a float. The input has jump. Its archetypes: Arena; Ball.

enum { NEW_C, NEW_A };

typedef struct new_ball {
    tide_float2 pos;
    float hits;
    uint8_t kind; // Kind is a byte now
    uint8_t padding[3];
    tide_float3 extra;
    tide_int2 gone;
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

typedef struct new_world {
    uint32_t unused; // The singletons and inputs needn't come first
    new_rules rules;
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

static const tide_layout_field new_ball_fields[] = {
    FIELD(new_ball, pos, "float2", TIDE_LAYOUT_FLOAT, 2, -1),
    FIELD(new_ball, hits, "float", TIDE_LAYOUT_FLOAT, 1, -1),
    FIELD(new_ball, kind, "Kind", TIDE_LAYOUT_ENUM, 0, 0),
    FIELD(new_ball, extra, "float3", TIDE_LAYOUT_FLOAT, 3, -1),
    FIELD(new_ball, gone, "int2", TIDE_LAYOUT_INT, 2, -1),
};
static const tide_layout_field new_arena_fields[] = {FIELD(new_arena, size, "int", TIDE_LAYOUT_INT, 1, -1)};
static const tide_layout_field new_rules_fields[] = {
    FIELD(new_rules, added, "int", TIDE_LAYOUT_INT, 1, -1),
    FIELD(new_rules, lives, "int", TIDE_LAYOUT_INT, 1, -1),
    FIELD(new_rules, speed, "float", TIDE_LAYOUT_FLOAT, 1, -1),
};
static const tide_layout_field new_in_fields[] = {
    FIELD(new_in, move, "float", TIDE_LAYOUT_FLOAT, 1, -1),
    FIELD(new_in, jump, "float", TIDE_LAYOUT_FLOAT, 1, -1),
};

enum { NEW_ARENA, NEW_BALL, NEW_RULES, NEW_IN };
static const tide_layout_type new_types[] = {
    {"Arena", sizeof(new_arena), true, 1, new_arena_fields, new_arena_defaults},
    {"Ball", sizeof(new_ball), false, 5, new_ball_fields, new_ball_defaults},
    {"Rules", sizeof(new_rules), false, 3, new_rules_fields, new_rules_defaults},
    {"In", sizeof(new_in), false, 2, new_in_fields, new_in_defaults},
};

static const tide_layout_member new_kinds[] = {{"C", NEW_C}, {"A", NEW_A}};
static const tide_layout_enum new_enums[] = {{"Kind", 2, new_kinds}};

static const tide_layout_place new_arenas_columns[] = {{NEW_ARENA, 2}};
static const tide_layout_place new_balls_columns[] = {{NEW_BALL, 2}};
static const tide_layout_archetype new_archetypes[] = {
    {true, 1, new_arenas_columns},
    {true, 1, new_balls_columns},
};
static const tide_layout_place new_singletons[] = {{NEW_RULES, offsetof(new_world, rules)}};

#define NEW_MATCH(archetypes, count)                                                                                \
    {sizeof(new_world), 1, new_singletons, count, archetypes, NEW_IN, offsetof(new_world, inputs),                  \
     offsetof(new_world, previous), 3, false, 0, NULL, 0, 0}

static const tide_layout new_layout = {4, new_types, 1, new_enums, NEW_MATCH(new_archetypes, 2), {0}};

// The same, but the scene Arena is gone: its type and its storage.
static const tide_layout no_arena_layout = {4, new_types, 1, new_enums, NEW_MATCH(new_archetypes + 1, 1), {0}};

// ---------------------------------------------------------------------------

typedef struct old_match {
    uint8_t *bytes;
    uint32_t size;
    uint32_t slots; // Its entity table's
    tide_entity arena, ball1, ball2, tag_only;
} old_match;

// An archetype's rows: its count, then its columns, one after another: the
// entities, their scenes, then each component's lanes.
static void put_rows(tide_writer *w, const uint32_t count, const tide_entity *entities, const tide_entity *scenes,
                     const void *const *components, const uint32_t *sizes, const uint32_t component_count)
{
    tide_write_u32(w, count);
    tide_write_bytes(w, entities, count * (uint32_t)sizeof(tide_entity));
    tide_write_bytes(w, scenes, count * (uint32_t)sizeof(tide_entity));
    for (uint32_t i = 0; i < component_count; i++) {
        const uint32_t width = tide_lane_width(sizes[i]);
        for (uint32_t k = 0; k < sizes[i] / width; k++) {
            for (uint32_t row = 0; row < count; row++) {
                tide_write_bytes(w, (const uint8_t *)components[i] + row * sizes[i] + k * width, width);
            }
        }
    }
}

// An arena with two balls in it, one of them tagged, and an entity that only
// has a tag: its components can't be together in the new build. `waiting` is
// how many changes are waiting to be applied.
static old_match make_old(const uint32_t waiting)
{
    old_match m = {0};
    tide_entities entities = {0};
    const tide_entity gone = tide_entity_create(&entities); // A slot that's free again
    tide_entity_destroy(&entities, gone);
    m.arena = tide_entity_create(&entities);
    tide_entity_set_location(&entities, m.arena, (tide_location){2, 0});
    m.ball1 = tide_entity_create(&entities);
    tide_entity_set_location(&entities, m.ball1, (tide_location){0, 0});
    m.ball2 = tide_entity_create(&entities);
    tide_entity_set_location(&entities, m.ball2, (tide_location){1, 0});
    m.tag_only = tide_entity_create(&entities);
    tide_entity_set_location(&entities, m.tag_only, (tide_location){3, 0});

    old_world header = {.rules = {.lives = 2, .speed = 3}};
    for (int i = 0; i < 3; i++) {
        header.inputs[i].move = (float)i + 0.5f;
        header.previous[i].move = (float)i;
    }
    m.bytes = calloc(1, 4096);
    tide_writer w = {m.bytes, 4096, 0, false, false};
    tide_write_bytes(&w, &header, sizeof header);
    tide_entities_pack(&entities, &w);
    const tide_entity none = {0};
    const old_ball ball1 = {3, {1.0f, 2.0f}, OLD_C, 5};
    const old_ball ball2 = {4, {3.0f, 4.0f}, OLD_B, 6};
    const old_tag tag2 = {8};
    const old_tag tag_only = {1};
    const old_arena arena = {9};
    const uint32_t ball_size[] = {sizeof(old_ball), sizeof(old_tag)};
    put_rows(&w, 1, &m.ball1, &m.arena, (const void *[]){&ball1}, ball_size, 1);
    put_rows(&w, 1, &m.ball2, &m.arena, (const void *[]){&ball2, &tag2}, ball_size, 2);
    put_rows(&w, 1, &m.arena, &m.arena, (const void *[]){&arena}, (const uint32_t[]){sizeof(old_arena)}, 1);
    put_rows(&w, 1, &m.tag_only, &none, (const void *[]){&tag_only}, (const uint32_t[]){sizeof(old_tag)}, 1);
    tide_write_u32(&w, waiting);
    m.size = w.size;
    m.slots = entities.next_unused;
    tide_entities_free(&entities);
    return m;
}

static bool same_entity(const tide_entity a, const tide_entity b)
{
    return a.index == b.index && a.generation == b.generation;
}

// A table of the new world's bytes: its count, where its entities' and
// scenes' columns start, and its components, gathered from their lanes.
typedef struct new_rows {
    uint32_t count;
    const tide_entity *entities;
    const tide_entity *scenes;
    uint8_t values[128];
} new_rows;

static new_rows read_rows(tide_reader *r, const uint32_t value_size)
{
    new_rows rows = {tide_read_u32(r), NULL, NULL, {0}};
    rows.entities = (const tide_entity *)tide_read_bytes(r, rows.count * (uint32_t)sizeof(tide_entity));
    rows.scenes = (const tide_entity *)tide_read_bytes(r, rows.count * (uint32_t)sizeof(tide_entity));
    const uint32_t width = tide_lane_width(value_size);
    const bool fits = rows.count * value_size <= sizeof rows.values;
    for (uint32_t k = 0; k < value_size / width; k++) {
        const uint8_t *lane = tide_read_bytes(r, rows.count * width);
        for (uint32_t row = 0; fits && lane && row < rows.count; row++) {
            memcpy(rows.values + row * value_size + k * width, lane + row * width, width);
        }
    }
    TIDE_CHECK(fits);
    return rows;
}

TIDE_TEST(migrate_carries_a_world_over_by_name)
{
    const old_match old = make_old(0);
    void *bytes = NULL;
    uint32_t size = 0;
    tide_migration m;
    TIDE_REQUIRE(tide_migrate_world(&old_layout, &old_layout.match, old.bytes, old.size, &new_layout, &new_layout.match,
                                    &bytes, &size, &m));
    TIDE_CHECK(m.failed[0] == '\0');
    TIDE_CHECK(m.entities_dropped == 1); // The one with only a tag

    // The new world's bytes: its struct, its entities, then Arena's rows and Ball's
    TIDE_REQUIRE(size >= sizeof(new_world));
    new_world n;
    memcpy(&n, bytes, sizeof n);
    tide_reader r = {bytes, size, sizeof n, false};
    tide_entities entities = {0};
    TIDE_REQUIRE(tide_entities_unpack(&entities, &r));
    const new_rows arenas = read_rows(&r, sizeof(new_arena));
    const new_rows balls = read_rows(&r, sizeof(new_ball));
    TIDE_CHECK(tide_read_u32(&r) == 0); // No changes waiting
    TIDE_REQUIRE(!r.failed && r.at == size);

    // Entities keep their IDs; the tagged ball moved in with the other
    TIDE_REQUIRE(arenas.count == 1);
    TIDE_CHECK(same_entity(arenas.entities[0], old.arena) && same_entity(arenas.scenes[0], old.arena));
    new_arena a;
    memcpy(&a, arenas.values, sizeof a);
    TIDE_CHECK(a.size == 9);
    TIDE_REQUIRE(balls.count == 2);
    TIDE_CHECK(same_entity(balls.entities[0], old.ball1) && same_entity(balls.entities[1], old.ball2));
    TIDE_CHECK(same_entity(balls.scenes[0], old.arena) && same_entity(balls.scenes[1], old.arena));
    const tide_location l1 = tide_entity_location(&entities, old.ball1);
    const tide_location l2 = tide_entity_location(&entities, old.ball2);
    const tide_location la = tide_entity_location(&entities, old.arena);
    TIDE_CHECK(l1.archetype == 1 && l1.row == 0 && l2.archetype == 1 && l2.row == 1);
    TIDE_CHECK(la.archetype == 0 && la.row == 0);
    TIDE_CHECK(!tide_entity_alive(&entities, old.tag_only));
    TIDE_CHECK(entities.next_unused == old.slots);

    // Fields by name: moved, converted, by the member's name, new, and reset
    new_ball b[2];
    memcpy(b, balls.values, sizeof b);
    TIDE_CHECK(b[0].pos.x == 1.0f && b[0].pos.y == 2.0f && b[0].hits == 3.0f);
    TIDE_CHECK(b[1].pos.x == 3.0f && b[1].pos.y == 4.0f && b[1].hits == 4.0f);
    TIDE_CHECK(b[0].kind == NEW_C); // C is still there, as another number
    TIDE_CHECK(b[1].kind == NEW_A); // B is gone: the default
    TIDE_CHECK(b[0].extra.x == 7.0f && b[0].extra.z == 7.0f);
    TIDE_CHECK(b[0].gone.x == 0 && b[0].gone.y == 0); // int to int2 doesn't convert

    // Singletons and inputs the same way
    TIDE_CHECK(n.rules.lives == 2 && n.rules.speed == 3.0f && n.rules.added == 3);
    for (int i = 0; i < 3; i++) {
        TIDE_CHECK(n.inputs[i].move == (float)i + 0.5f && n.inputs[i].jump == 1.0f);
        TIDE_CHECK(n.previous[i].move == (float)i && n.previous[i].jump == 1.0f);
    }
    TIDE_CHECK(tide_layout_fields_reset(&old_layout, &new_layout) == 1); // Ball.gone
    tide_entities_free(&entities);
    free(old.bytes);
    free(bytes);
}

TIDE_TEST(migrate_refuses_a_world_whose_scene_is_gone)
{
    const old_match old = make_old(0);
    void *bytes = NULL;
    uint32_t size = 0;
    tide_migration m;
    TIDE_CHECK(!tide_migrate_world(&old_layout, &old_layout.match, old.bytes, old.size, &no_arena_layout,
                                   &no_arena_layout.match, &bytes, &size, &m));
    TIDE_CHECK(strstr(m.failed, "Arena") != NULL);
    TIDE_CHECK(bytes == NULL);
    free(old.bytes);
}

TIDE_TEST(migrate_refuses_a_world_with_changes_waiting)
{
    const old_match old = make_old(1);
    void *bytes = NULL;
    uint32_t size = 0;
    tide_migration m;
    TIDE_CHECK(!tide_migrate_world(&old_layout, &old_layout.match, old.bytes, old.size, &new_layout, &new_layout.match,
                                   &bytes, &size, &m));
    TIDE_CHECK(m.failed[0] != '\0');
    free(old.bytes);
}

TIDE_TEST(migrate_refuses_bytes_that_arent_a_world)
{
    const old_match old = make_old(0);
    void *bytes = NULL;
    uint32_t size = 0;
    tide_migration m;
    TIDE_CHECK(!tide_migrate_world(&old_layout, &old_layout.match, old.bytes, old.size - 3u, &new_layout,
                                   &new_layout.match, &bytes, &size, &m));
    TIDE_CHECK(m.failed[0] != '\0');
    free(old.bytes);
}

// ---------------------------------------------------------------------------
// The layout tidec describes

static const tide_layout_type *type_named(const tide_layout *l, const char *name)
{
    for (uint32_t i = 0; i < l->type_count; i++) {
        if (strcmp(l->types[i].name, name) == 0) return &l->types[i];
    }
    return NULL;
}

// The entity whose Unit is named `name`, or the null entity.
static tide_entity unit_named(const tide_world *w, const char *name)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const tide_entity e = tide_entity_in_slot(&w->entities, i);
        if (!tide_has_Unit(w, e)) continue;
        const tide_str text = tide_text_read(&w->heap, tide_get_Unit(w, e).name);
        if (text.bytes == (int32_t)strlen(name) && memcmp(text.ptr, name, strlen(name)) == 0) return e;
    }
    return (tide_entity){0};
}

// A world's bytes, in a block of their own.
static uint8_t *world_bytes(const tide_world *w, uint32_t *size)
{
    *size = tide_world_pack(w, NULL, 0);
    uint8_t *bytes = malloc(*size);
    tide_world_pack(w, bytes, *size);
    return bytes;
}

// `w`, carried over from `from` (a layout of the same game, by another name)
// to the game's own layout, into `copy`.
static bool carry(const tide_layout *from, const tide_world *w, tide_world *copy, tide_migration *m)
{
    const tide_layout *l = &tide_game_layout;
    uint32_t size;
    uint8_t *bytes = world_bytes(w, &size);
    void *carried = NULL;
    uint32_t carried_size = 0;
    bool ok = tide_migrate_world(from, &from->match, bytes, size, l, &l->match, &carried, &carried_size, m);
    ok = ok && tide_world_unpack(copy, carried, carried_size);
    free(bytes);
    free(carried);
    return ok;
}

// Whether two worlds hold the same, as their bytes and hashes.
static bool same(const tide_world *a, const tide_world *b)
{
    uint32_t a_size;
    uint32_t b_size;
    uint8_t *x = world_bytes(a, &a_size);
    uint8_t *y = world_bytes(b, &b_size);
    const bool equal = a_size == b_size && memcmp(x, y, a_size) == 0;
    free(x);
    free(y);
    return equal && tide_world_hash(a) == tide_world_hash(b);
}

TIDE_TEST(migrate_a_game_to_its_own_layout_changes_nothing)
{
    const tide_layout *l = &tide_game_layout;
    tide_world *w = calloc(1, sizeof *w);
    tide_world *copy = calloc(1, sizeof *copy);
    tide_world_init(w, 1.0f / 60.0f);
    tide_world_set_input(w, tide_player_from_index(0), (Controls){.move = {0.5f, -1.0f}});
    tide_world_tick(w);
    tide_migration m;
    TIDE_REQUIRE(carry(l, w, copy, &m));
    TIDE_CHECK(m.entities_dropped == 0);
    TIDE_CHECK(same(w, copy));

    tide_local *local = calloc(1, sizeof *local);
    tide_local *local_copy = calloc(1, sizeof *local_copy);
    tide_local_init(local);
    const uint32_t size = tide_local_pack(local, NULL, 0);
    uint8_t *bytes = malloc(size);
    tide_local_pack(local, bytes, size);
    void *carried = NULL;
    uint32_t carried_size = 0;
    TIDE_REQUIRE(tide_migrate_world(l, &l->local, bytes, size, l, &l->local, &carried, &carried_size, &m));
    TIDE_CHECK(carried_size == size && memcmp(carried, bytes, size) == 0);
    TIDE_CHECK(tide_local_unpack(local_copy, carried, carried_size));
    TIDE_CHECK(tide_layout_fields_reset(l, l) == 0);
    free(bytes);
    free(carried);
    tide_world_free(w);
    tide_world_free(copy);
    tide_local_free(local);
    tide_local_free(local_copy);
    free(w);
    free(copy);
    free(local);
    free(local_copy);
}

// A waiting task carries over to a build where its code and its frame are the
// same, and is dropped where they aren't.
TIDE_TEST(migrate_carries_waiting_tasks_whose_code_is_the_same)
{
    const tide_layout *l = &tide_game_layout;
    TIDE_REQUIRE(l->match.task_count == 1 && l->local.task_count == 1);
    tide_world *w = calloc(1, sizeof *w);
    tide_world *copy = calloc(1, sizeof *copy);
    tide_world_init(w, 1.0f / 60.0f);
    tide_world_tick(w);
    TIDE_REQUIRE(w->tide_tasks_handler_Wander.count == 2);
    tide_migration m;
    TIDE_REQUIRE(carry(l, w, copy, &m));
    TIDE_CHECK(m.tasks_dropped == 0);
    TIDE_CHECK(same(w, copy));
    // They go on the same in both
    for (int i = 0; i < 40; i++) {
        tide_world_tick(w);
        tide_world_tick(copy);
    }
    TIDE_CHECK(same(w, copy));
    TIDE_CHECK(!tide_entity_is_null(unit_named(copy, "first!")));

    // From a build where Wander's code was different
    tide_world_init(w, 1.0f / 60.0f);
    tide_world_tick(w);
    tide_layout_tasks other = l->match.tasks[0];
    other.name = "Wander 0000000000000000";
    tide_layout changed = *l;
    changed.match.tasks = &other;
    TIDE_REQUIRE(carry(&changed, w, copy, &m));
    TIDE_CHECK(m.tasks_dropped == 2);
    TIDE_CHECK(copy->tide_tasks_handler_Wander.count == 0);
    TIDE_CHECK(copy->tide_task_order == w->tide_task_order); // The order goes on all the same

    // Local tasks, and the frames they count
    static tide_draw_list draw;
    static tide_gui gui;
    tide_local *local = calloc(1, sizeof *local);
    tide_local *local_copy = calloc(1, sizeof *local_copy);
    tide_local_init(local);
    for (int i = 0; i < 3; i++) tide_frame(NULL, NULL, 1.0f, local, &draw, &gui);
    TIDE_REQUIRE(local->tide_tasks_function_Blink.count == 1);
    const uint32_t size = tide_local_pack(local, NULL, 0);
    uint8_t *bytes = malloc(size);
    tide_local_pack(local, bytes, size);
    void *carried = NULL;
    uint32_t carried_size = 0;
    TIDE_REQUIRE(tide_migrate_world(l, &l->local, bytes, size, l, &l->local, &carried, &carried_size, &m));
    TIDE_CHECK(m.tasks_dropped == 0);
    TIDE_REQUIRE(tide_local_unpack(local_copy, carried, carried_size));
    TIDE_CHECK(local_copy->tide_frames == 3 && local_copy->tide_tasks_function_Blink.count == 1);
    for (int i = 0; i < 7; i++) tide_frame(NULL, NULL, 1.0f, local_copy, &draw, &gui);
    TIDE_CHECK(local_copy->Menu.shown == 0);
    tide_frame(NULL, NULL, 1.0f, local_copy, &draw, &gui); // Frame 10: it started in frame 0
    TIDE_CHECK(local_copy->Menu.shown == 1);
    free(bytes);
    free(carried);
    tide_local_free(local);
    tide_local_free(local_copy);
    free(local);
    free(local_copy);
    tide_world_free(w);
    tide_world_free(copy);
    free(w);
    free(copy);
}

TIDE_TEST(migrate_a_games_defaults_are_its_declared_ones)
{
    const tide_layout_type *t = type_named(&tide_game_layout, "Unit");
    TIDE_REQUIRE(t != NULL);
    TIDE_REQUIRE(t->size == sizeof(Unit));
    Unit u;
    memset(&u, 0xFF, sizeof u);
    const uint32_t mark = tide_scratch_mark();
    t->defaults(&u);
    TIDE_CHECK(tide_scratch_mark() == mark); // The borrowed default text is gone again
    TIDE_CHECK(u.position.x == 0.0f && u.position.y == 0.0f);
    TIDE_CHECK(u.mode == Mode_Run);
    TIDE_CHECK(u.stats.level == 2 && u.stats.speed == 1.5f);
    TIDE_CHECK(u.name.at == 0); // Text starts empty
    TIDE_CHECK(tide_entity_is_null(u.target));
    const tide_layout_type *score = type_named(&tide_game_layout, "Score");
    TIDE_REQUIRE(score != NULL);
    Score s;
    score->defaults(&s);
    TIDE_CHECK(s.points == 10);
}

// Where each build is a program of its own (the web), the old one packs its
// layout for the next: unpacked in another block, it carries a world over the
// same.
TIDE_TEST(migrate_a_packed_layout_works_where_it_goes)
{
    const tide_layout *l = &tide_game_layout;
    uint32_t size = 0;
    void *packed = tide_layout_pack(l, &size);
    TIDE_REQUIRE(packed != NULL);
    void *elsewhere = malloc(size);
    memcpy(elsewhere, packed, size);
    memset(packed, 0xAB, size); // Nothing may point back into the first block
    const tide_layout *unpacked = tide_layout_unpack(elsewhere, size);
    TIDE_REQUIRE(unpacked != NULL);
    TIDE_CHECK(unpacked->type_count == l->type_count && unpacked->enum_count == l->enum_count);
    TIDE_CHECK(strcmp(unpacked->types[0].name, l->types[0].name) == 0);
    TIDE_CHECK(unpacked->types[0].defaults == NULL);

    tide_world *w = calloc(1, sizeof *w);
    tide_world *copy = calloc(1, sizeof *copy);
    tide_world_init(w, 1.0f / 60.0f);
    tide_world_tick(w);
    tide_migration m;
    TIDE_REQUIRE(carry(unpacked, w, copy, &m));
    TIDE_CHECK(same(w, copy));
    TIDE_CHECK(tide_layout_fields_reset(unpacked, l) == 0);

    TIDE_CHECK(tide_layout_unpack(elsewhere, 8) == NULL); // Too small to be one
    free(packed);
    free(elsewhere);
    tide_world_free(w);
    tide_world_free(copy);
    free(w);
    free(copy);
}

// A build where Unit's `position` was called `place`: the rest of Unit still
// carries over, text included.
TIDE_TEST(migrate_a_game_keeps_what_has_the_same_name)
{
    const tide_layout *l = &tide_game_layout;
    tide_layout_type *types = malloc(l->type_count * sizeof *types);
    memcpy(types, l->types, l->type_count * sizeof *types);
    tide_layout old = *l;
    old.types = types;
    tide_layout_field *fields = NULL;
    for (uint32_t i = 0; i < old.type_count; i++) {
        if (strcmp(types[i].name, "Unit") != 0) continue;
        fields = malloc(types[i].field_count * sizeof *fields);
        memcpy(fields, types[i].fields, types[i].field_count * sizeof *fields);
        for (uint32_t k = 0; k < types[i].field_count; k++) {
            if (strcmp(fields[k].name, "position") == 0) fields[k].name = "place";
        }
        types[i].fields = fields;
    }
    TIDE_REQUIRE(fields != NULL);

    tide_world *w = calloc(1, sizeof *w);
    tide_world *copy = calloc(1, sizeof *copy);
    tide_world_init(w, 1.0f / 60.0f);
    const tide_entity first = unit_named(w, "first");
    TIDE_REQUIRE(!tide_entity_is_null(first));
    Unit before = tide_get_Unit(w, first);
    before.mode = Mode_Jump;
    tide_set_Unit(w, first, before);
    tide_migration m;
    TIDE_REQUIRE(carry(&old, w, copy, &m));
    const tide_entity carried = unit_named(copy, "first");
    TIDE_REQUIRE(!tide_entity_is_null(carried));
    const Unit after = tide_get_Unit(copy, carried);
    TIDE_CHECK(before.position.x == 1.0f && after.position.x == 0.0f); // Another name: the default
    TIDE_CHECK(after.mode == Mode_Jump);
    TIDE_CHECK(after.stats.level == 2);
    TIDE_CHECK(tide_world_entity_count(copy) == tide_world_entity_count(w));
    free(fields);
    free(types);
    tide_world_free(w);
    tide_world_free(copy);
    free(w);
    free(copy);
}
