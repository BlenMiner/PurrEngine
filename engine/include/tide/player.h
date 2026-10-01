#pragma once

#include <stdbool.h>
#include <stdint.h>

// Player identity (Tide's PlayerID).
//
// Temporary implementation written by Claude; the project owner takes it over
// later. The simulation only ever sees player IDs, never connections: the
// networking layer maps connections to IDs, so a player who reconnects and gets
// their ID back keeps everything they owned.
//
// A zeroed ID is "no player". PlayerID(n) in Tide is stored as n + 1.

#ifndef TIDE_MAX_PLAYERS
#define TIDE_MAX_PLAYERS 16u
#endif

typedef struct tide_player_id {
    uint32_t id; // 0 = no player, otherwise player index + 1
} tide_player_id;

static inline tide_player_id tide_player_from_index(const int32_t index)
{
    return (tide_player_id){index >= 0 ? (uint32_t)index + 1u : 0u};
}

static inline bool tide_player_is_null(const tide_player_id p)
{
    return p.id == 0;
}

static inline bool tide_player_equal(const tide_player_id a, const tide_player_id b)
{
    return a.id == b.id;
}

// Index into per-player arrays, or -1 for no player or an ID out of range.
static inline int32_t tide_player_index(const tide_player_id p)
{
    return p.id >= 1 && p.id <= TIDE_MAX_PLAYERS ? (int32_t)(p.id - 1u) : -1;
}
