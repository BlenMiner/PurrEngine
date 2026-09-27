#pragma once

#include <stdbool.h>
#include <stdint.h>

// Player identity (PurrLang's PlayerID).
//
// Temporary implementation written by Claude; the project owner takes it over
// later. The simulation only ever sees player IDs, never connections: the
// networking layer maps connections to IDs, so a player who reconnects and gets
// their ID back keeps everything they owned.
//
// A zeroed ID is "no player". PlayerID(n) in PurrLang is stored as n + 1.

#ifndef PURR_MAX_PLAYERS
#define PURR_MAX_PLAYERS 16u
#endif

typedef struct purr_player_id {
    uint32_t id; // 0 = no player, otherwise player index + 1
} purr_player_id;

static inline purr_player_id purr_player_from_index(const int32_t index)
{
    return (purr_player_id){index >= 0 ? (uint32_t)index + 1u : 0u};
}

static inline bool purr_player_is_null(const purr_player_id p)
{
    return p.id == 0;
}

static inline bool purr_player_equal(const purr_player_id a, const purr_player_id b)
{
    return a.id == b.id;
}

// Index into per-player arrays, or -1 for no player or an ID out of range.
static inline int32_t purr_player_index(const purr_player_id p)
{
    return p.id >= 1 && p.id <= PURR_MAX_PLAYERS ? (int32_t)(p.id - 1u) : -1;
}
