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
- If you changed data (a component, a field, a singleton, an event, a scene or the input), the game starts over.
- If the new code has errors, `purr` prints them and the game keeps running its last build.

When the game gets into a state you don't want, type `r` and press Enter in `purr`'s terminal to start it over. With a match on several windows (`--host` in one, `--join` in another), each window reloads when you save. A window that joined another's match joins it again whenever that one starts over.

Hot reload is only for `purr run` on this machine: `purr build` makes a plain program, and `purr run --web` doesn't reload yet.

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
