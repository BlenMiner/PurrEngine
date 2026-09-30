#pragma once

// Runs a PurrLang game in a window. Include it after the game's generated
// header, in the file with main:
//
//     #include "game.h"
//     #include "purr/run.h"
//
//     int main(int argc, char **argv)
//     {
//         purr_run(&(purr_run_desc){.title = "Sandbox", .argc = argc, .argv = argv});
//     }
//
// The game runs in a session (purr/session.h). If its Main scene is the
// match's, a match starts right away: on this machine alone, or with others:
//
//     game --host [port]   a match others can join (port 7777 by default)
//     game --join address  the match at "192.168.1.5", "localhost:7777" and the like
//
// If Main is local, the program starts in it, with no match, and local code
// starts one with Session.Play, Host or Join (--join works too). This machine's
// player joins before the match's first tick. If the game has an input, the
// devices are sampled once per tick this machine runs, minus what the GUI is
// using. The views draw every frame, and their GUI over the world. Close the
// window to quit.
//
// The loop itself is in purr/host.h, which runs the game through
// purr_host_game_api, made here from the game's header.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include "purr/host.h"

static inline void purr_run_local_init(void *local)
{
    purr_local_init(local);
}

#ifdef PURR_HAS_INPUT
static inline void purr_run_sample_input(const purr_devices *devices, const void *local, void *input)
{
    *(purr_input *)input = purr_input_sample(devices, local);
}
#endif

static inline void purr_run_views(const void *world, const void *previous, const float alpha, void *local,
                                  purr_draw_list *draw, purr_gui *gui)
{
    purr_frame(world, previous, alpha, local, draw, gui);
}

static inline bool purr_run_take_request(void *local, purr_session_request *request, void *start)
{
    return purr_local_take_request(local, request, start);
}

static inline void purr_run_set_session(void *local, const uint32_t state, const purr_player_id player,
                                        const uint32_t ping, const bool server)
{
    purr_local_set_session(local, state, player, ping, server);
}

static inline void purr_run_connected(void *local)
{
    purr_local_connected(local);
}

static inline void purr_run_disconnected(void *local, const uint32_t reason)
{
    purr_local_disconnected(local, reason);
}

static inline int32_t purr_run_tick(const void *world)
{
    return ((const purr_world *)world)->Time.tick;
}

static inline uint32_t purr_run_entity_count(const void *world)
{
    return purr_world_entity_count(world);
}

static inline void purr_run_unload(void)
{
    purr_scratch_free();
}

// The game as hosts run it.
static const purr_host_game purr_host_game_api = {
    .game = &purr_game_api,
    .local_size = sizeof(purr_local),
#ifdef PURR_MAIN_IS_LOCAL
    .main_is_local = true,
#endif
    .local_init = purr_run_local_init,
#ifdef PURR_HAS_INPUT
    .sample_input = purr_run_sample_input,
#endif
    .frame = purr_run_views,
    .take_request = purr_run_take_request,
    .set_session = purr_run_set_session,
    .connected = purr_run_connected,
    .disconnected = purr_run_disconnected,
    .tick = purr_run_tick,
    .entity_count = purr_run_entity_count,
    .unload = purr_run_unload,
};

_Noreturn static inline void purr_run(const purr_run_desc *desc)
{
    purr_host_run(desc, &purr_host_game_api);
}
