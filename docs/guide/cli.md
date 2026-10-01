# The tide command

`tide` builds and runs games. A game is every `.tide` file in a folder and its subfolders, with the `.c` files and libraries there when it calls C (see [Calling C](../language/c-functions.md)). Commands take the game's folder, or use the current one.

```
tide <command> [folder] [options]
```

| Command | What it does |
|---|---|
| `tide run [folder]` | Builds the game and plays it |
| `tide build [folder]` | Builds the game into `<folder>/build` |
| `tide schedule [folder]` | Shows which systems can run at the same time, and why the others wait |
| `tide editors` | Adds Tide to VS Code, Cursor, VSCodium and Windsurf |
| `tide upgrade` | Updates tide to the newest version |
| `tide version` | Shows tide's version, and which compilers it found |
| `tide help` | Lists every option |

## run and build

| Option | What it does |
|---|---|
| `--release` | Optimized, the way players get it |
| `--web` | A web page (WebGL 2), built with clang's WebAssembly target |
| `--title <title>` | The window's title, over the game's `title` setting (default: that, or the folder's name) |
| `--stats` | Shows the tick, the entity count, the ping and the frame rate |
| `-o <path>` | `build` only: where the program goes |
| `--no-open` | `run --web` only: serves the page without opening a browser |

`tide build --release --web` makes one self-contained `.html` file with the game inside. It opens straight from disk, and can be put online as it is.

### Threads on the web

A game's ticks run on every core when the page is cross-origin isolated: served with these two headers, which let it share its memory with the workers its threads run in.

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`tide run --web` serves the page with them. Where a page is put online, the host has to send them: on itch.io, it's the **SharedArrayBuffer support** option in the game's embed settings. A page without them, or opened from disk, runs the same game on one thread, with the same results, so web players on either kind of page play together. Workers only start once a tick has enough work for them.

## Hot reload

While `tide run` plays a game, saving a `.tide` file rebuilds it, and so does saving one of its C files or libraries. The game carries on with the new code in the same window:

- If you changed only code (systems, views, event handlers, methods), the match and everything local, like menus, carry on where they are.
- If you changed data (components, fields, singletons, the input), the game is carried over to it by name, and goes on from where it was.
- If the new code has errors, `tide` prints them and the game keeps running its last build.

Carrying the game over works like this:

- Every field of the same name keeps its value. An `int` field that became a `float` is converted, and so is an int vector that became a float vector of the same size. An enum field keeps its member by name.
- A new field gets its declared default. So does a field whose type changed to one it doesn't convert to, like `float2` to `float3`. New text and list fields start empty.
- Renaming a field is removing it and adding a new one: its value is lost.
- An entity keeps its ID. When you remove a component, entities that had it keep their other components. An entity whose remaining components can't exist together in the new code is dropped.
- A changed default doesn't change existing entities, only new ones.
- A waiting [task](../language/tasks.md) carries on when its async function's code is the same, and is dropped when it changed: where it was waiting is gone.
- If the scene the game is in is gone, the game starts over.

`tide` says what happened after each reload, like `reloaded, and carried the game over to its new data layout; 1 field reset; 2 entities dropped`.

When the game gets into a state you don't want, type `r` and press Enter in `tide`'s terminal to start it over. With a match on several windows (`--host` in one, `--connect localhost` in another), each window reloads when you save, and players stay in the match. A window that joined another's match joins it again whenever that one starts over. On the web, a reload goes on with the match closed for now: the room closes, and the other players drop out.

`tide run --web` reloads too. `tide` serves the game's page at an address on your machine, like `http://127.0.0.1:52407/`, opens it in your browser, and keeps running until you press Ctrl+C. What each reload did shows in the browser's console. VS Code and JetBrains IDEs can play it beside your code instead (see [Editors](editors.md)).

Hot reload is only for `tide run`: `tide build` makes a plain program, or a page with nothing of it.

## Multiplayer

`tide run` can start a match others join. The game's `Main` scene must be the match's for this (see [Scenes](../language/scenes.md)); a game that starts in a menu does the same from code (see [Multiplayer](../language/multiplayer.md)).

| Option | What it does |
|---|---|
| `--host [port]` | Opens the match for others to join: in a room, whose code the game can show, and on UDP port 7777 by default (not on the web) |
| `--join <code>` | The match in the room with this code, like `K7QF2M` |
| `--connect <address>` | The match at an address, like `192.168.1.5` or `localhost:7777` |

## upgrade

| Option | What it does |
|---|---|
| `--nightly` | Follow nightly versions from now on |
| `--stable` | Follow stable versions from now on |
| `--version <v>` | Install exactly this version |

## Files

`tide` keeps its work in a hidden `.tide` folder in the game's folder. You can delete it any time, and git ignores it. `tide build` puts what it makes in `build/`, with the game's `.dll`, `.so` or `.dylib` libraries next to the program. C files and libraries in hidden folders or in `build/` aren't part of the game.
