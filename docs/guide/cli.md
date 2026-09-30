# The purr command

`purr` builds and runs games. A game is every `.purr` file in a folder and its subfolders. Commands take the game's folder, or use the current one.

```
purr <command> [folder] [options]
```

| Command | What it does |
|---|---|
| `purr run [folder]` | Builds the game and plays it |
| `purr build [folder]` | Builds the game into `<folder>/build` |
| `purr schedule [folder]` | Shows which systems can run at the same time, and why the others wait |
| `purr editors` | Adds PurrLang to VS Code, Cursor, VSCodium and Windsurf |
| `purr upgrade` | Updates purr to the newest version |
| `purr version` | Shows purr's version, and which compilers it found |
| `purr help` | Lists every option |

## run and build

| Option | What it does |
|---|---|
| `--release` | Optimized, the way players get it |
| `--web` | A web page (WebGL 2), built with clang's WebAssembly target |
| `--title <title>` | The window's title (default: the folder's name) |
| `--stats` | Shows the tick, the entity count, the ping and the frame rate |
| `-o <path>` | `build` only: where the program goes |

`purr build --release --web` makes one self-contained `.html` file with the game inside. It opens straight from disk, and can be put online as it is.

## Hot reload

While `purr run` plays a game, saving a `.purr` file rebuilds it, and the game carries on with the new code in the same window:

- If you changed only code (systems, views, event handlers, methods), the match and everything local, like menus, carry on where they are.
- If you changed data (components, fields, singletons, the input), the game is carried over to it by name, and goes on from where it was.
- If the new code has errors, `purr` prints them and the game keeps running its last build.

Carrying the game over works like this:

- Every field of the same name keeps its value. An `int` field that became a `float` is converted, and so is an int vector that became a float vector of the same size. An enum field keeps its member by name.
- A new field gets its declared default. So does a field whose type changed to one it doesn't convert to, like `float2` to `float3`. New text and list fields start empty.
- Renaming a field is removing it and adding a new one: its value is lost.
- An entity keeps its ID. When you remove a component, entities that had it keep their other components. An entity whose remaining components can't exist together in the new code is dropped.
- A changed default doesn't change existing entities, only new ones.
- If the scene the game is in is gone, the game starts over.

`purr` says what happened after each reload, like `reloaded, and carried the game over to its new data layout; 1 field reset; 2 entities dropped`.

When the game gets into a state you don't want, type `r` and press Enter in `purr`'s terminal to start it over. With a match on several windows (`--host` in one, `--join` in another), each window reloads when you save, and players stay in the match. A window that joined another's match joins it again whenever that one starts over.

`purr run --web` reloads too. `purr` serves the game's page at an address on your machine, like `http://127.0.0.1:52407/`, opens it in your browser, and keeps running until you press Ctrl+C. What each reload did shows in the browser's console.

Hot reload is only for `purr run`: `purr build` makes a plain program, or a page with nothing of it.

## Multiplayer

`purr run` can start a match others join. The game's `Main` scene must be the match's for this (see [Scenes](../language/scenes.md)); a game that starts in a menu does the same from code (see [Multiplayer](../language/multiplayer.md)).

| Option | What it does |
|---|---|
| `--host [port]` | A match others can join, on UDP port 7777 by default |
| `--join <address>` | The match at an address, like `192.168.1.5` or `localhost:7777` |

## upgrade

| Option | What it does |
|---|---|
| `--nightly` | Follow nightly versions from now on |
| `--stable` | Follow stable versions from now on |
| `--version <v>` | Install exactly this version |

## Files

`purr` keeps its work in a hidden `.purr` folder in the game's folder. You can delete it any time, and git ignores it. `purr build` puts what it makes in `build/`.
