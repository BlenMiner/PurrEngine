# C hosts

A game needs no C: `purr` runs it in a window. This page is for working inside PurrEngine's own repo, where CMake builds games, and for C programs that drive a game themselves: tests, tools, custom hosts. See [AGENTS.md](https://github.com/BlenMiner/PurrEngine/blob/dev/AGENTS.md) for building the repo.

## purr_add_game

```cmake
purr_add_game(<target> [SOURCES <file.purr|file.c>...] [HOST <file.c>...] [NAME <name>] [TITLE <title>] [STATS])
```

- The game is every `.purr` file in the current source folder and its subfolders. `SOURCES` lists the files instead.
- The game's C, which defines its `extern` functions (see [Calling C](../language/c-functions.md)), is every `.c` file there but the `HOST` ones, or the `.c` files listed in `SOURCES`. Unlike `purr`, CMake doesn't pick up prebuilt libraries: link them to the target yourself.
- Without `HOST`, the game is the whole program: a generated `main` runs it in a window. On the web, it's `<target>.html`.
- With `HOST`, those C files are the program. They include `<name>.h`, the generated header, where `NAME` defaults to `<target>`.
- `<target>_schedule` is a build target that prints the game's [schedule](./schedule.md).

## The standard host

`purr/run.h` is what a game without `HOST` uses:

```c
#include "game.h"
#include "purr/run.h"

int main(int argc, char **argv)
{
    purr_run(&(purr_run_desc){.title = "My game", .argc = argc, .argv = argv});
}
```

It opens a window and runs the game in a session (`purr/session.h`): it samples this machine's input once per tick, draws the views and the GUI every frame, and takes `--host [port]` and `--join address` from the command line. `purr_run_desc` also has `width` and `height` (960 by 540 by default), `tick_rate` (60 by default) and `stats`.

Hosts include `purr/platform.h`, never raylib: the generated header names types after the game's components, and raylib defines many of the same names.

## The generated header

The generated header is the API between the game and its host. Namespaced declarations have their namespace in their C name: `Combat.Health` is `Combat_Health`.

**The world**

- `purr_world` is the whole match as plain data. Copying it is a snapshot.
- `purr_world_init(w, dt)` clears it, sets `Time.dt` and the singletons' defaults, and loads `Main` if it's the match's. `purr_world_start(w, dt, start)` starts in another scene.
- `purr_world_tick(w)` runs every system once, then applies structural changes and events. If that leaves no scene loaded, it loads `Main` again when it's the match's (`purr_frame` does the same for a local `Main`).
- `purr_world_ended(w)` says whether the match is over: its last scene unloaded and `Main` is local. A server stops there and tells every player, who go offline with `PURR_DISCONNECT_ENDED`.
- `purr_get_<Component>(w, entity)` gives an entity's component, or `NULL`.
- `purr_world_player_joined(w, player)` and `purr_world_player_left(w, player)` send `PlayerJoined` and `PlayerLeft`, handled at the end of the next tick.
- `purr_world_copy(to, from)` and `purr_world_hash(w)`: snapshots and hashes, covering only what's in use.
- `purr_world_entity_count(w)` and `purr_world_print(w)`, for debugging.
- Text fields are offsets into the world's heap: read one with `purr_text_read(&w->heap, field)`.

**Local state**

- `purr_local` is this machine's local state, outside every world. `purr_local_init(local)` clears it and sets its defaults. `PURR_MAIN_IS_LOCAL` is defined when `Main` is local.
- `purr_frame(w, previous, alpha, local, draw, gui)` runs every view once, blending the match between `previous` and `w` by `alpha`, then applies the local changes they made. Pass `NULL` and 1 to draw `w` as it is, and `NULL` for `w` outside a match.

**Input**

- `PURR_HAS_INPUT` is defined when the game has an input, and `purr_input` names its type.
- `purr_input_sample(devices, local)` runs the input's `Sample` with this machine's devices.
- `purr_world_set_input(w, player, input)` sets a player's input for the next tick, and `purr_world_set_server_input(w, input)` the server's. Both repair the input first: NaN, bounds, then `Sanitize`.

**Sessions**

- `purr_game_api` is the game as a session runs it (`purr_game` in `purr/session.h`).
- `purr_local_take_request(local, &request, &start)` takes local code's session calls, like `Session.Play`. `purr_local_set_session`, `purr_local_connected` and `purr_local_disconnected` tell local code where it stands.

## A frame

```c
purr_draw_reset(&draw);
purr_gui_begin(&gui, &devices, purr_platform_screen_size(), purr_platform_measure_text);
purr_frame(w, previous, alpha, &local, &draw, &gui);
purr_gui_end(&gui, &draw);
purr_platform_draw(&draw);
```

## Limits

The generated code and the engine library read these compile definitions. To raise one, define it for the whole build, not only the game's target, since the engine's own sources depend on it too:

| Definition | Default |
|---|---|
| `PURR_MAX_ENTITIES` | 16384 |
| `PURR_ARCHETYPE_CAPACITY` | 1024 entities per archetype |
| `PURR_MAX_COMMANDS` | 4096 structural changes and events per tick |
| `PURR_HEAP_BYTES` | 256 KiB of text and lists per world |
