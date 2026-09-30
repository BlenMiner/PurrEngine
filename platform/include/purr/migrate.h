#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/layout.h"

// Carrying a world over to a build of its game whose data layout changed, for
// hot reloading (see purr/host.h), by name:
//
// - Singletons and components keep their fields of the same name. A field of
//   the same type keeps its value; an int one that became a float, or an int
//   vector a float vector of the same size, is converted; an enum keeps its
//   member by name. Any other field, and every new one, has its declared
//   default; new text and lists start empty.
// - Entities keep their IDs. An entity whose component was removed goes where
//   entities with the rest of its components go, and is dropped when no such
//   storage exists in the new build, or it's full.
// - The inputs carry over like singletons, and the heap as it is.
//
// A world whose scene is gone can't be carried over.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

typedef struct purr_migration {
    uint32_t entities_dropped;
    char failed[160]; // Why the world couldn't be carried over, or empty
} purr_migration;

// Makes `to`, a zeroed world of `to_layout`'s game, from `from`, a world of
// `from_layout`'s. `from_world` and `to_world` say which world they are: the
// layouts' match or local. False, with `m->failed` saying why, if it can't.
bool purr_migrate_world(const purr_layout *from_layout, const purr_layout_world *from_world, const void *from,
                        const purr_layout *to_layout, const purr_layout_world *to_world, void *to,
                        purr_migration *m);

// How many fields both layouts have, of the same name in the same type, whose
// values can't carry over: their type changed to one they don't convert to.
uint32_t purr_layout_fields_reset(const purr_layout *from, const purr_layout *to);
