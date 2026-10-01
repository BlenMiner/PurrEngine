#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/layout.h"

// Carrying a world over to a build of its game whose data layout changed, for
// hot reloading (see tide/host.h), by name:
//
// - Singletons and components keep their fields of the same name. A field of
//   the same type keeps its value; an int one that became a float, or an int
//   vector a float vector of the same size, is converted; an enum keeps its
//   member by name. Any other field, and every new one, has its declared
//   default; new text and lists start empty.
// - Entities keep their IDs. An entity whose component was removed goes where
//   entities with the rest of its components go, and is dropped when no such
//   storage exists in the new build.
// - The inputs carry over like singletons, and the heap as it is.
//
// A world whose scene is gone can't be carried over.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

typedef struct tide_migration {
    uint32_t entities_dropped;
    char failed[160]; // Why the world couldn't be carried over, or empty
} tide_migration;

// Makes the bytes of a world of `to_layout`'s game, which its unpack function
// (tide_world_unpack, tide_local_unpack) makes a world of, from `from`, the
// bytes of a world of `from_layout`'s (tide_world_pack, tide_local_pack).
// `from_world` and `to_world` say which world they are: the layouts' match or
// local. The new bytes go in `*to`, which the caller frees. False, with
// `m->failed` saying why, if it can't.
bool tide_migrate_world(const tide_layout *from_layout, const tide_layout_world *from_world, const void *from,
                        uint32_t from_size, const tide_layout *to_layout, const tide_layout_world *to_world, void **to,
                        uint32_t *to_size, tide_migration *m);

// How many fields both layouts have, of the same name in the same type, whose
// values can't carry over: their type changed to one they don't convert to.
uint32_t tide_layout_fields_reset(const tide_layout *from, const tide_layout *to);

// A layout in one block of memory with offsets for pointers, which can go to
// another program: where each new build is a program of its own (the web), the
// old one packs its layout for the new one to carry its worlds over from. The
// types have no `defaults` then, which only the new layout's need. NULL if
// there isn't the memory; free() it.
void *tide_layout_pack(const tide_layout *layout, uint32_t *size);

// The layout in a block tide_layout_pack made, 8-aligned, which becomes it
// where it is. NULL if the block isn't one.
const tide_layout *tide_layout_unpack(void *block, uint32_t size);
