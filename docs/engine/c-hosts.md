# C hosts

A game needs no C: `tide` runs it in a window. This page is for working inside Tide's own repo, where CMake builds games, and for C programs that drive a game themselves: tests, tools, custom hosts. See [AGENTS.md](https://github.com/BlenMiner/tide-engine/blob/dev/AGENTS.md) for building the repo.

## tide_add_game

```cmake
tide_add_game(<target> [SOURCES <file.tide|file.c>...] [HOST <file.c>...] [NAME <name>] [TITLE <title>] [STATS])
```

- The game is every `.tide` file in the current source folder and its subfolders. `SOURCES` lists the files instead.
- The game's C, which defines its `extern` functions (see [Calling C](../language/c-functions.md)), is every `.c` file there but the `HOST` ones, or the `.c` files listed in `SOURCES`. Unlike `tide`, CMake doesn't pick up prebuilt libraries: link them to the target yourself.
- Without `HOST`, the game is the whole program: a generated `main` runs it in a window. On the web, it's `<target>.html`.
- With `HOST`, those C files are the program. They include `<name>.h`, the generated header, where `NAME` defaults to `<target>`.
- `<target>_schedule` is a build target that prints the game's [schedule](./schedule.md).

## The standard host

`tide/run.h` is what a game without `HOST` uses:

```c
#include "game.h"
#include "tide/run.h"

int main(int argc, char **argv)
{
    tide_run(&(tide_run_desc){.title = "My game", .argc = argc, .argv = argv});
}
```

It opens a window and runs the game in a session (`tide/session.h`): it samples this machine's input once per tick, draws the views and the GUI every frame, and takes `--host [port]`, `--join code` and `--connect address` from the command line. `--host` opens the match, in a room (see [Multiplayer](../language/multiplayer.md#rooms)), and a UDP port too, except on the web. `tide_run_desc` also has `width` and `height` (960 by 540 by default), `tick_rate` (60 by default) and `stats`.

Hosts include `tide/platform.h`, never raylib: the generated header names types after the game's components, and raylib defines many of the same names.

## The generated header

The generated header is the API between the game and its host. Namespaced declarations have their namespace in their C name: `Combat.Health` is `Combat_Health`.

**The world**

- `tide_world` is the whole match. Its data is in pages it shares with its snapshots, so a world starts zeroed (`{0}`, static or `calloc`), copying the struct isn't a snapshot (`tide_world_copy` is), and `tide_world_free(w)` lets it go.
- `tide_world_init(w, dt)` clears it (what it had goes), sets `Time.dt` and the singletons' defaults, and loads `Main` if it's the match's. `tide_world_start(w, dt, start)` starts in another scene.
- `tide_world_tick(w)` runs every system once, then applies structural changes and events. `tide_world_tick_on(w, jobs)` does the same on threads, with the same results: `tide_platform_jobs()` gives a pool with one per core (NULL on the web), and sessions take it as `jobs` in their desc, as the standard host does. If that leaves no scene loaded, it loads `Main` again when it's the match's (`tide_frame` does the same for a local `Main`).
- `tide_world_ended(w)` says whether the match is over: its last scene unloaded and `Main` is local. A server stops there and tells every player, who go offline with `TIDE_DISCONNECT_ENDED`.
- `tide_get_<Component>(w, entity)` gives an entity's component to change, or `NULL`. `tide_read_<Component>(w, entity)` gives it only to read, which leaves the pages the world shares with its snapshots shared.
- `TIDE_AT(w, arch0_Body, Body, row)` reads a row's component in an archetype's storage, and `TIDE_ENTITY_AT(w, arch0_Body, row)` its entity: for tests and tools that go through every entity.
- `tide_world_player_joined(w, player)` and `tide_world_player_left(w, player)` send `PlayerJoined` and `PlayerLeft`, handled at the end of the next tick.
- `tide_world_copy(to, from)` and `tide_world_hash(w)`: snapshots and hashes. A snapshot shares the world's pages until one of them changes a page, so it costs the memory of what's different, and each page keeps its hash until it changes, so hashing reads what changed since the last time.
- `tide_world_pack(w, out, capacity)` and `tide_world_unpack(w, data, size)`: the world as bytes, as sessions send it.
- `tide_world_entity_count(w)` and `tide_world_print(w)`, for debugging.
- Text fields are offsets into the world's heap: read one with `tide_text_read(&w->heap, field)`.

**Local state**

- `tide_local` is this machine's local state, outside every world. `tide_local_init(local)` clears it and sets its defaults, and `tide_local_free(local)` lets it go. `TIDE_MAIN_IS_LOCAL` is defined when `Main` is local.
- `tide_frame(w, previous, alpha, local, draw, gui)` runs every view once, blending the match between `previous` and `w` by `alpha`, then applies the local changes they made. Pass `NULL` and 1 to draw `w` as it is, and `NULL` for `w` outside a match.

**Input**

- `TIDE_HAS_INPUT` is defined when the game has an input, and `tide_input` names its type.
- `tide_input_sample(devices, local)` runs the input's `Sample` with this machine's devices.
- `tide_world_set_input(w, player, input)` sets a player's input for the next tick, and `tide_world_set_server_input(w, input)` the server's. Both repair the input first: NaN, bounds, then `Sanitize`.

**Sessions**

- `tide_game_api` is the game as a session runs it (`tide_game` in `tide/session.h`).
- `tide_local_take_request(local, &request, &start)` takes local code's session calls, like `Session.Start`, in order: call it until it's false. `tide_local_set_session`, `tide_local_connected` and `tide_local_disconnected` tell local code where it stands; `tide_local_set_session` takes whether the match is open too (`tide_session_status`'s `open`), and the code of the room the match is in (`tide_platform_room_code`), or `""` while it's closed; `tide_local_disconnected` takes a kick's message (`tide_session_event`'s `message`), or NULL.

## A frame

```c
tide_draw_reset(&draw);
tide_gui_begin(&gui, &devices, tide_platform_screen_size(), tide_platform_measure_text);
tide_frame(w, previous, alpha, &local, &draw, &gui);
tide_gui_end(&gui, &draw);
tide_platform_draw(&draw);
```

## Memory

Worlds grow as they need, with no limits but memory: entities, rows of each archetype, structural changes and events in a tick, and the text and lists in a world's heap. Running out of memory ends the program, saying so. So does a chain of events that never ends, once it's `TIDE_MAX_CHAIN` deep in one tick (100,000 by default): each event sent, or entity spawned, by a handler of the one before. The message names the event. An archetype keeps its rows in chunks, and the first starts small, so archetypes with a few entities take little memory.
