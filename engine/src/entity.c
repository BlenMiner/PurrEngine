#include "purr/entity.h"

#include <string.h>

#include "purr/net.h"

purr_entity purr_entity_create(purr_entities *t)
{
    uint32_t index;
    if (t->free_count > 0) {
        index = t->free_list[--t->free_count];
        t->free_list[t->free_count] = 0; // Past the count, a table is zeros
    } else if (t->next_unused < PURR_MAX_ENTITIES) {
        index = t->next_unused++;
    } else {
        return (purr_entity){0};
    }

    purr_entity_slot *slot = &t->slots[index];
    slot->generation++; // Even (free) -> odd (alive).
    slot->archetype = PURR_ARCHETYPE_NONE;
    slot->row = 0;
    slot->snaps = 0;
    return (purr_entity){index, slot->generation};
}

bool purr_entity_destroy(purr_entities *t, const purr_entity e)
{
    if (!purr_entity_alive(t, e)) return false;

    purr_entity_slot *slot = &t->slots[e.index];
    slot->generation++; // Odd (alive) -> even (free); stale handles stop matching.
    slot->archetype = PURR_ARCHETYPE_NONE;
    slot->row = 0;
    slot->snaps = 0;
    t->free_list[t->free_count++] = e.index;
    return true;
}

bool purr_entity_alive(const purr_entities *t, const purr_entity e)
{
    return e.index < t->next_unused
        && (e.generation & 1u)
        && t->slots[e.index].generation == e.generation;
}

void purr_entity_set_location(purr_entities *t, const purr_entity e, const purr_location loc)
{
    if (!purr_entity_alive(t, e)) return;
    t->slots[e.index].archetype = loc.archetype;
    t->slots[e.index].row = loc.row;
}

purr_location purr_entity_location(const purr_entities *t, const purr_entity e)
{
    if (!purr_entity_alive(t, e)) return (purr_location){PURR_ARCHETYPE_NONE, 0};
    const purr_entity_slot *slot = &t->slots[e.index];
    return (purr_location){slot->archetype, slot->row};
}

void purr_entities_copy(purr_entities *to, const purr_entities *from)
{
    if (to->next_unused > from->next_unused) {
        memset(&to->slots[from->next_unused], 0, (to->next_unused - from->next_unused) * sizeof to->slots[0]);
    }
    if (to->free_count > from->free_count) {
        memset(&to->free_list[from->free_count], 0, (to->free_count - from->free_count) * sizeof to->free_list[0]);
    }
    to->next_unused = from->next_unused;
    to->free_count = from->free_count;
    memcpy(to->slots, from->slots, from->next_unused * sizeof to->slots[0]);
    memcpy(to->free_list, from->free_list, from->free_count * sizeof to->free_list[0]);
}

uint64_t purr_entities_hash(uint64_t h, const purr_entities *t)
{
    h = purr_hash_more(h, &t->next_unused, sizeof t->next_unused);
    h = purr_hash_more(h, &t->free_count, sizeof t->free_count);
    h = purr_hash_more(h, t->free_list, t->free_count * sizeof t->free_list[0]);
    return purr_hash_more(h, t->slots, t->next_unused * sizeof t->slots[0]);
}

void purr_entity_snap(purr_entities *t, const purr_entity e)
{
    if (purr_entity_alive(t, e)) t->slots[e.index].snaps++;
}

uint32_t purr_entity_snaps(const purr_entities *t, const purr_entity e)
{
    return purr_entity_alive(t, e) ? t->slots[e.index].snaps : 0u;
}
