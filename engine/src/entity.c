#include "purr/entity.h"

purr_entity purr_entity_create(purr_entities *t)
{
    uint32_t index;
    if (t->free_count > 0) {
        index = t->free_list[--t->free_count];
    } else if (t->next_unused < PURR_MAX_ENTITIES) {
        index = t->next_unused++;
    } else {
        return (purr_entity){0};
    }

    purr_entity_slot *slot = &t->slots[index];
    slot->generation++; // Even (free) -> odd (alive).
    slot->archetype = PURR_ARCHETYPE_NONE;
    slot->row = 0;
    return (purr_entity){index, slot->generation};
}

bool purr_entity_destroy(purr_entities *t, const purr_entity e)
{
    if (!purr_entity_alive(t, e)) return false;

    purr_entity_slot *slot = &t->slots[e.index];
    slot->generation++; // Odd (alive) -> even (free); stale handles stop matching.
    slot->archetype = PURR_ARCHETYPE_NONE;
    slot->row = 0;
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
