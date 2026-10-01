#include "tide/entity.h"

#include <string.h>

#include "tide/net.h"

tide_entity tide_entity_create(tide_entities *t)
{
    uint32_t index;
    if (t->free_count > 0) {
        index = t->free_list[--t->free_count];
        t->free_list[t->free_count] = 0; // Past the count, a table is zeros
    } else if (t->next_unused < TIDE_MAX_ENTITIES) {
        index = t->next_unused++;
    } else {
        return (tide_entity){0};
    }

    tide_entity_slot *slot = &t->slots[index];
    slot->generation++; // Even (free) -> odd (alive).
    slot->archetype = TIDE_ARCHETYPE_NONE;
    slot->row = 0;
    slot->snaps = 0;
    return (tide_entity){index, slot->generation};
}

bool tide_entity_destroy(tide_entities *t, const tide_entity e)
{
    if (!tide_entity_alive(t, e)) return false;

    tide_entity_slot *slot = &t->slots[e.index];
    slot->generation++; // Odd (alive) -> even (free); stale handles stop matching.
    slot->archetype = TIDE_ARCHETYPE_NONE;
    slot->row = 0;
    slot->snaps = 0;
    t->free_list[t->free_count++] = e.index;
    return true;
}

bool tide_entity_alive(const tide_entities *t, const tide_entity e)
{
    return e.index < t->next_unused
        && (e.generation & 1u)
        && t->slots[e.index].generation == e.generation;
}

void tide_entity_set_location(tide_entities *t, const tide_entity e, const tide_location loc)
{
    if (!tide_entity_alive(t, e)) return;
    t->slots[e.index].archetype = loc.archetype;
    t->slots[e.index].row = loc.row;
}

tide_location tide_entity_location(const tide_entities *t, const tide_entity e)
{
    if (!tide_entity_alive(t, e)) return (tide_location){TIDE_ARCHETYPE_NONE, 0};
    const tide_entity_slot *slot = &t->slots[e.index];
    return (tide_location){slot->archetype, slot->row};
}

void tide_entities_copy(tide_entities *to, const tide_entities *from)
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

uint64_t tide_entities_hash(uint64_t h, const tide_entities *t)
{
    h = tide_hash_more(h, &t->next_unused, sizeof t->next_unused);
    h = tide_hash_more(h, &t->free_count, sizeof t->free_count);
    h = tide_hash_more(h, t->free_list, t->free_count * sizeof t->free_list[0]);
    return tide_hash_more(h, t->slots, t->next_unused * sizeof t->slots[0]);
}

void tide_entity_snap(tide_entities *t, const tide_entity e)
{
    if (tide_entity_alive(t, e)) t->slots[e.index].snaps++;
}

uint32_t tide_entity_snaps(const tide_entities *t, const tide_entity e)
{
    return tide_entity_alive(t, e) ? t->slots[e.index].snaps : 0u;
}
