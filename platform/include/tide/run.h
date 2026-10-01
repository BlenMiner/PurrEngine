#pragma once

// Runs a Tide game in a window. Include it after the game's generated
// header, in the file with main:
//
//     #include "game.h"
//     #include "tide/run.h"
//
//     int main(int argc, char **argv)
//     {
//         tide_run(&(tide_run_desc){.title = "Sandbox", .argc = argc, .argv = argv});
//     }
//
// The game runs in a session (tide/session.h). If its Main scene is the
// match's, a match starts right away: on this machine alone, or with others:
//
//     game --host [port]      a match others can join (port 7777 by default)
//     game --join code        the match in the room with this code, like K7QF2M
//     game --connect address  the match at "192.168.1.5", "localhost:7777" and the like
//
// A match others can join is in a room, whose code players join it with,
// and on the desktop on a UDP port too. If Main is local, the program starts in it, with no match, and
// local code starts one with Session.Play, Host, Join or Connect (--join and
// --connect work too). This machine's
// player joins before the match's first tick. If the game has an input, the
// devices are sampled once per tick this machine runs, minus what the GUI is
// using. The views draw every frame, and their GUI over the world. Close the
// window to quit.
//
// The loop itself is in tide/host.h, which runs the game through
// tide_host_game_api, made here from the game's header.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include "tide/host.h"

static inline void tide_run_local_init(void *local)
{
    tide_local_init(local);
}

#ifdef TIDE_HAS_INPUT
static inline void tide_run_sample_input(const tide_devices *devices, const void *local, void *input)
{
    *(tide_input *)input = tide_input_sample(devices, local);
}
#endif

static inline void tide_run_views(const void *world, const void *previous, const float alpha, void *local,
                                  tide_draw_list *draw, tide_gui *gui)
{
    tide_frame(world, previous, alpha, local, draw, gui);
}

static inline bool tide_run_take_request(void *local, tide_session_request *request, void *start)
{
    return tide_local_take_request(local, request, start);
}

static inline void tide_run_set_session(void *local, const uint32_t state, const tide_player_id player,
                                        const uint32_t ping, const bool server, const char *room)
{
    tide_local_set_session(local, state, player, ping, server, room);
}

static inline void tide_run_connected(void *local)
{
    tide_local_connected(local);
}

static inline void tide_run_disconnected(void *local, const uint32_t reason)
{
    tide_local_disconnected(local, reason);
}

static inline int32_t tide_run_tick(const void *world)
{
    return ((const tide_world *)world)->Time.tick;
}

static inline uint32_t tide_run_entity_count(const void *world)
{
    return tide_world_entity_count(world);
}

static inline void tide_run_unload(void)
{
    tide_scratch_free();
}

// The game as hosts run it.
static const tide_host_game tide_host_game_api = {
    .game = &tide_game_api,
    .local_size = sizeof(tide_local),
#ifdef TIDE_MAIN_IS_LOCAL
    .main_is_local = true,
#endif
    .local_init = tide_run_local_init,
#ifdef TIDE_HAS_INPUT
    .sample_input = tide_run_sample_input,
#endif
    .frame = tide_run_views,
    .take_request = tide_run_take_request,
    .set_session = tide_run_set_session,
    .connected = tide_run_connected,
    .disconnected = tide_run_disconnected,
    .tick = tide_run_tick,
    .entity_count = tide_run_entity_count,
    .unload = tide_run_unload,
};

_Noreturn static inline void tide_run(const tide_run_desc *desc)
{
    tide_host_run(desc, &tide_host_game_api);
}
