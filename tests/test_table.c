#include <stdlib.h>
#include <string.h>

#include "tide/table.h"
#include "tide_test.h"

// An archetype of entities with a position: the entity column, then a float
// pair, in chunks of 16 rows.
static const uint32_t sizes[] = {sizeof(tide_entity), 2 * sizeof(float)};
static const tide_columns columns = {2, 4, sizes};

// Another, with a third column, for moving rows between them.
static const uint32_t wider_sizes[] = {sizeof(tide_entity), 2 * sizeof(float), sizeof(int32_t)};
static const tide_columns wider = {3, 4, wider_sizes};

static const float *position(const tide_table *t, const uint32_t row)
{
    return tide_table_get(t, &columns, row, 1);
}

static tide_entity entity_at(const tide_table *t, const tide_columns *c, const uint32_t row)
{
    tide_entity e;
    memcpy(&e, tide_table_get(t, c, row, 0), sizeof e);
    return e;
}

// `n` more entities, each with position (row, -row).
static void fill(tide_table *t, tide_entities *entities, const uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const tide_entity e = tide_entity_create(entities);
        const uint32_t row = tide_table_add(t, &columns);
        *(tide_entity *)tide_table_cell(t, &columns, row, 0) = e;
        float *p = tide_table_cell(t, &columns, row, 1);
        p[0] = (float)row;
        p[1] = -(float)row;
        tide_entity_set_location(entities, e, (tide_location){0, row});
    }
}

TIDE_TEST(table_grows_in_chunks)
{
    tide_table t = {0};
    tide_entities entities = {0};
    fill(&t, &entities, 3);
    TIDE_CHECK(t.chunks == 1 && t.last_room == 4); // The first chunk starts small
    fill(&t, &entities, 2);
    TIDE_CHECK(t.chunks == 1 && t.last_room == 8);
    fill(&t, &entities, 40);
    TIDE_CHECK(t.count == 45 && t.chunks == 3 && t.last_room == 16);
    TIDE_CHECK(tide_table_rows(&t, columns.shift, 2) == 13);
    TIDE_CHECK(position(&t, 0)[0] == 0.0f && position(&t, 4)[0] == 4.0f && position(&t, 44)[1] == -44.0f);
    tide_table_free(&t, &columns);
    tide_entities_free(&entities);
}

TIDE_TEST(table_remove_moves_the_last_row_in)
{
    tide_table t = {0};
    tide_entities entities = {0};
    fill(&t, &entities, 17); // A second chunk with one row
    const tide_entity last = entity_at(&t, &columns, 16);
    tide_table_remove(&t, &columns, 3, &entities, 0);
    TIDE_CHECK(t.count == 16 && t.chunks == 1); // The chunk left empty goes
    TIDE_CHECK(position(&t, 3)[0] == 16.0f);
    TIDE_CHECK(tide_entity_equal(entity_at(&t, &columns, 3), last));
    TIDE_CHECK(tide_entity_location(&entities, last).row == 3);
    tide_table_remove(&t, &columns, 15, &entities, 0); // The last row: nothing moves
    TIDE_CHECK(t.count == 15 && position(&t, 14)[0] == 14.0f);
    tide_table_free(&t, &columns);
    tide_entities_free(&entities);
}

TIDE_TEST(table_move_keeps_shared_columns)
{
    tide_table from = {0};
    tide_table to = {0};
    tide_entities entities = {0};
    fill(&from, &entities, 5);
    const tide_entity moved = entity_at(&from, &columns, 1);
    const int32_t map[] = {0, 1, -1};
    const uint32_t row = tide_table_move(&from, &columns, 1, 0, &to, &wider, 1, map, &entities);
    TIDE_CHECK(row == 0 && to.count == 1 && from.count == 4);
    TIDE_CHECK(((const float *)tide_table_get(&to, &wider, 0, 1))[0] == 1.0f);
    TIDE_CHECK(*(const int32_t *)tide_table_get(&to, &wider, 0, 2) == 0);
    const tide_location at = tide_entity_location(&entities, moved);
    TIDE_CHECK(at.archetype == 1 && at.row == 0);
    TIDE_CHECK(tide_entity_location(&entities, entity_at(&from, &columns, 1)).row == 1); // The last one, moved in
    tide_table_free(&from, &columns);
    tide_table_free(&to, &wider);
    tide_entities_free(&entities);
}

// A snapshot shares the table's pages; changing a column of a chunk copies
// that page alone, and the hash only follows what's in use.
TIDE_TEST(table_snapshots_share_until_changed)
{
    tide_table t = {0};
    tide_table snapshot = {0};
    tide_entities entities = {0};
    fill(&t, &entities, 40);
    tide_table_copy(&snapshot, &t, &columns);
    const uint64_t before = tide_table_hash(1, &t, &columns);
    TIDE_CHECK(tide_table_hash(1, &snapshot, &columns) == before);

    float *p = tide_table_cell(&t, &columns, 20, 1);
    p[0] = 100.0f;
    TIDE_CHECK(t.pages[1 * 2 + 1] != snapshot.pages[1 * 2 + 1]); // Chunk 1's positions: copied
    TIDE_CHECK(t.pages[1 * 2 + 0] == snapshot.pages[1 * 2 + 0]); // Its entities: still shared
    TIDE_CHECK(t.pages[0 * 2 + 1] == snapshot.pages[0 * 2 + 1]); // Chunk 0: still shared
    TIDE_CHECK(position(&snapshot, 20)[0] == 20.0f);
    TIDE_CHECK(tide_table_hash(1, &t, &columns) != before);

    p = tide_table_cell(&t, &columns, 20, 1); // Changed through the table, which keeps the hash up to date
    p[0] = 20.0f;                            // As it was: the same hash again
    TIDE_CHECK(tide_table_hash(1, &t, &columns) == before);

    tide_table_copy(&t, &snapshot, &columns);
    TIDE_CHECK(t.pages[1 * 2 + 1] == snapshot.pages[1 * 2 + 1]);
    tide_table_free(&t, &columns);
    tide_table_free(&snapshot, &columns);
    tide_entities_free(&entities);
}

TIDE_TEST(table_packs_and_unpacks)
{
    tide_table t = {0};
    tide_entities entities = {0};
    fill(&t, &entities, 37);
    const uint32_t size = tide_table_packed_size(&t, &columns);
    uint8_t *bytes = malloc(size);
    tide_writer w = {bytes, size, 0, false};
    tide_table_pack(&t, &columns, &w);
    TIDE_CHECK(!w.overflow && w.size == size);

    tide_table back = {0};
    tide_reader r = {bytes, size, 0, false};
    TIDE_REQUIRE(tide_table_unpack(&back, &columns, &r));
    TIDE_CHECK(back.count == 37 && back.chunks == 3);
    TIDE_CHECK(tide_table_hash(1, &back, &columns) == tide_table_hash(1, &t, &columns));
    TIDE_CHECK(position(&back, 36)[1] == -36.0f);

    tide_table short_of_rows = {0};
    tide_reader cut = {bytes, size - 1u, 0, false};
    TIDE_CHECK(!tide_table_unpack(&short_of_rows, &columns, &cut));
    free(bytes);
    tide_table_free(&t, &columns);
    tide_table_free(&back, &columns);
    tide_table_free(&short_of_rows, &columns);
    tide_entities_free(&entities);
}

TIDE_TEST(table_queue_keeps_items_in_place)
{
    tide_queue q = {0};
    int32_t *first = tide_queue_push(&q, sizeof(int32_t));
    *first = 5;
    for (int32_t i = 1; i < 300; i++) *(int32_t *)tide_queue_push(&q, sizeof(int32_t)) = i;
    TIDE_CHECK(*first == 5 && first == tide_queue_at(&q, 0, sizeof(int32_t))); // It didn't move as the queue grew
    TIDE_CHECK(*(int32_t *)tide_queue_at(&q, 299, sizeof(int32_t)) == 299);
    tide_queue copy = {0};
    tide_queue_copy(&copy, &q, sizeof(int32_t));
    TIDE_CHECK(tide_queue_hash(1, &copy, sizeof(int32_t)) == tide_queue_hash(1, &q, sizeof(int32_t)));
    tide_queue_clear(&q, sizeof(int32_t));
    TIDE_CHECK(q.count == 0 && *first == 0);
    tide_queue_free(&q);
    tide_queue_free(&copy);
}
