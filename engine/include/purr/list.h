#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/heap.h"

// PurrLang's `List<T>`: a list of values, as a value. Generated code calls the
// functions below with each element's size, and handles the text in elements
// itself (see purr/text.h).
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A purr_list is where its elements are: a block in a world's heap, or in the
// scratch area. As with text, which one comes from the handle's address: a
// list that's part of a world (a component's field, a singleton's, the command
// queue's) has its own block in that world's heap, and any other list has one
// in the scratch area. A list is never shared: taking one out of a field or a
// variable copies it (purr_list_copy), so changing a list in place is safe.
//
// Nothing fails: past the end, reads find nothing and writes do nothing, and
// when there's no room left, adding does nothing.

typedef struct purr_list {
    uint32_t at; // 0: empty. Otherwise its block's offset, and where in the top two bits
} purr_list;

int32_t purr_list_count(purr_list l);

// The element at `i`, or NULL past the end. It moves when the list grows.
void *purr_list_at(purr_list l, int32_t i, uint32_t size);

// A new element at the end, zeroed, or NULL when there's no room.
void *purr_list_add(purr_list *l, uint32_t size);

// A new element at `i` (clamped to the list), zeroed, the ones after it moved
// along; or NULL when there's no room.
void *purr_list_insert(purr_list *l, int32_t i, uint32_t size);

// Removes the element at `i`, moving the ones after it back. Past the end, it
// does nothing. Release its text first.
void purr_list_remove_at(purr_list *l, int32_t i, uint32_t size);

// Removes every element. Release their text first.
void purr_list_clear(purr_list *l, uint32_t size);

// A copy in the scratch area: a list taken out of a field or variable.
purr_list purr_list_copy(purr_list l, uint32_t size);

// A list in the scratch area of `count` elements: [a, b, c].
purr_list purr_list_from(const void *items, int32_t count, uint32_t size);

// Assigning: `value`'s elements, a copy if `to` is part of a world, or `value`
// itself otherwise. Release the old elements' text first, and own the new ones'
// after.
void purr_list_set(purr_list *to, purr_list value, uint32_t size);

// A list just copied into a world, like a spawn's component into the command
// queue: its elements in a block of the world's own. Own their text after.
void purr_list_own(purr_list *l, uint32_t size);

// A world's list leaving it. Release its elements' text first.
void purr_list_release(purr_list *l);
