#include "tide/entity.h"

#include <stdlib.h>
#include <string.h>

// See tide/entity.h.

#define SLOTS (1u << TIDE_ENTITY_PAGE_SHIFT)
#define PAGE_BYTES (SLOTS * (uint32_t)sizeof(tide_entity_slot))

static const tide_entity_slot *slot_of(const tide_entities *t, const uint32_t index)
{
    const tide_entity_slot *slots = tide_page_data(t->page[index >> TIDE_ENTITY_PAGE_SHIFT]);
    return &slots[index & (SLOTS - 1u)];
}

// Slots in use on page `p`.
static uint32_t used_on(const tide_entities *t, const uint32_t p)
{
    const uint32_t left = t->next_unused - (p << TIDE_ENTITY_PAGE_SHIFT);
    return left < SLOTS ? left : SLOTS;
}

// A slot to change: its page made this table's own.
static tide_entity_slot *slot_to_change(tide_entities *t, const uint32_t index)
{
    const uint32_t p = index >> TIDE_ENTITY_PAGE_SHIFT;
    t->page[p] = tide_page_own(t->page[p], 1, used_on(t, p) * (uint32_t)sizeof(tide_entity_slot));
    tide_entity_slot *slots = tide_page_data(t->page[p]);
    return &slots[index & (SLOTS - 1u)];
}

static void make_room(tide_entities *t, const uint32_t pages)
{
    if (pages <= t->room) return;
    uint32_t room = t->room ? t->room : 4u;
    while (room < pages) room *= 2u;
    tide_page **grown = realloc(t->page, room * sizeof *grown);
    if (!grown) tide_out_of_memory();
    t->page = grown;
    t->room = room;
}

tide_entity tide_entity_create(tide_entities *t)
{
    uint32_t index;
    if (t->free_head) {
        index = t->free_head - 1u;
    } else {
        if (t->next_unused == UINT32_MAX) tide_out_of_memory();
        index = t->next_unused;
        if ((index & (SLOTS - 1u)) == 0) {
            make_room(t, t->pages + 1u);
            t->page[t->pages++] = tide_page_new(PAGE_BYTES);
        }
        t->next_unused++;
    }

    tide_entity_slot *slot = slot_to_change(t, index);
    if (t->free_head) t->free_head = slot->row;
    slot->generation++; // Even (free) -> odd (alive).
    slot->archetype = TIDE_ARCHETYPE_NONE;
    slot->row = 0;
    slot->snaps = 0;
    return (tide_entity){index, slot->generation};
}

bool tide_entity_destroy(tide_entities *t, const tide_entity e)
{
    if (!tide_entity_alive(t, e)) return false;

    tide_entity_slot *slot = slot_to_change(t, e.index);
    slot->generation++; // Odd (alive) -> even (free); stale handles stop matching.
    slot->archetype = TIDE_ARCHETYPE_NONE;
    slot->row = t->free_head;
    slot->snaps = 0;
    t->free_head = e.index + 1u;
    return true;
}

bool tide_entity_alive(const tide_entities *t, const tide_entity e)
{
    return e.index < t->next_unused
        && (e.generation & 1u)
        && slot_of(t, e.index)->generation == e.generation;
}

void tide_entity_set_location(tide_entities *t, const tide_entity e, const tide_location loc)
{
    if (!tide_entity_alive(t, e)) return;
    tide_entity_slot *slot = slot_to_change(t, e.index);
    slot->archetype = loc.archetype;
    slot->row = loc.row;
}

tide_location tide_entity_location(const tide_entities *t, const tide_entity e)
{
    if (!tide_entity_alive(t, e)) return (tide_location){TIDE_ARCHETYPE_NONE, 0};
    const tide_entity_slot *slot = slot_of(t, e.index);
    return (tide_location){slot->archetype, slot->row};
}

tide_entity tide_entity_in_slot(const tide_entities *t, const uint32_t index)
{
    if (index >= t->next_unused) return (tide_entity){0};
    const uint32_t generation = slot_of(t, index)->generation;
    return (generation & 1u) ? (tide_entity){index, generation} : (tide_entity){0};
}

void tide_entity_snap(tide_entities *t, const tide_entity e)
{
    if (tide_entity_alive(t, e)) slot_to_change(t, e.index)->snaps++;
}

uint32_t tide_entity_snaps(const tide_entities *t, const tide_entity e)
{
    return tide_entity_alive(t, e) ? slot_of(t, e.index)->snaps : 0u;
}

void tide_entities_copy(tide_entities *to, const tide_entities *from)
{
    if (to == from) return;
    // Its pages first, then letting go of to's: they can be the same ones.
    for (uint32_t i = 0; i < from->pages; i++) tide_page_retain(from->page[i]);
    for (uint32_t i = 0; i < to->pages; i++) tide_page_release(to->page[i], 1);
    make_room(to, from->pages);
    if (from->pages) memcpy(to->page, from->page, from->pages * sizeof *to->page);
    to->pages = from->pages;
    to->next_unused = from->next_unused;
    to->free_head = from->free_head;
}

uint64_t tide_entities_hash(uint64_t h, const tide_entities *t)
{
    h = tide_hash_more(h, &t->next_unused, sizeof t->next_unused);
    h = tide_hash_more(h, &t->free_head, sizeof t->free_head);
    for (uint32_t p = 0; p < t->pages; p++) {
        const uint64_t page = tide_page_hash(t->page[p], used_on(t, p) * (uint32_t)sizeof(tide_entity_slot));
        h = tide_hash_more(h, &page, sizeof page);
    }
    return h;
}

void tide_entities_free(tide_entities *t)
{
    for (uint32_t i = 0; i < t->pages; i++) tide_page_release(t->page[i], 1);
    free(t->page);
    memset(t, 0, sizeof *t);
}

uint32_t tide_entities_packed_size(const tide_entities *t)
{
    return 8u + t->next_unused * (uint32_t)sizeof(tide_entity_slot);
}

void tide_entities_pack(const tide_entities *t, tide_writer *w)
{
    tide_write_u32(w, t->next_unused);
    tide_write_u32(w, t->free_head);
    for (uint32_t p = 0; p < t->pages; p++) {
        tide_write_bytes(w, tide_page_data(t->page[p]), used_on(t, p) * (uint32_t)sizeof(tide_entity_slot));
    }
}

bool tide_entities_unpack(tide_entities *t, tide_reader *r)
{
    const uint32_t next_unused = tide_read_u32(r);
    const uint32_t free_head = tide_read_u32(r);
    if (r->failed || free_head > next_unused || next_unused > (r->size - r->at) / sizeof(tide_entity_slot)) return false;
    t->next_unused = next_unused;
    t->free_head = free_head;
    const uint32_t pages = (next_unused + SLOTS - 1u) >> TIDE_ENTITY_PAGE_SHIFT;
    make_room(t, pages);
    for (uint32_t p = 0; p < pages; p++) {
        t->page[p] = tide_page_new(PAGE_BYTES);
        t->pages = p + 1u;
        const uint32_t bytes = used_on(t, p) * (uint32_t)sizeof(tide_entity_slot);
        memcpy(tide_page_data(t->page[p]), tide_read_bytes(r, bytes), bytes);
    }
    // Free slots lead to free slots in the table, without going round
    uint32_t free = t->free_head;
    for (uint32_t n = 0; free && !r->failed; n++) {
        const tide_entity_slot *slot = free <= next_unused ? slot_of(t, free - 1u) : NULL;
        r->failed = !slot || (slot->generation & 1u) || slot->row > next_unused || n == next_unused;
        free = slot ? slot->row : 0u;
    }
    return !r->failed;
}
