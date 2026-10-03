# Tide for VS Code

Completion, errors as you type with quick fixes, hover, go to definition, type
definition and implementation, find usages, highlights, rename, the call
hierarchy, formatting, parameter hints, inlay hints, the outline, symbols
across the game, folding, selection ranges, semantic highlighting, moving a
declaration to a file of its own, and each system's schedule above it, for
`.tide` files. It works in
VS Code, Cursor, VSCodium and Windsurf.

tide's installer adds this extension to the editors it finds, and `tide
editors` adds it again, for example after installing a new editor. The
language server is `tidels`, which comes with tide. The extension runs it from
`PATH`, or from where tide is installed. In a folder with a
`build/tools/tidels`, like Tide's own repo once it's built, it runs that one,
once you trust the folder.

Open the folder of your game, the one you'd run `tide run` in: every `.tide`
file in it and its subfolders is one game.

## Running the game

The play button above a `.tide` file runs its game:

- **Tide: Run** plays it in a window of its own, as `tide run` does.
- **Tide: Run on the Web** plays it in a browser tab beside the code:
  VS Code's integrated browser, or the Simple Browser in editors without it.
  The game gets keys while its tab has focus, and the editor may pause it
  while its tab is hidden, so keep it in a group of its own.
- **Tide: Run on Android** plays it on the Android phone or emulator that's
  connected, as `tide run --android` does.

Each runs `tide` in a terminal of its own, so saving a file reloads the
game (on Android, builds it again and starts it over), and typing `r` and
Enter there starts it over. Running it again starts the game over. The
first run on Android may ask to download Android's tools from Google: type
`y` and Enter there to accept.

On the web, each reload says what it did in the page's console. **Tide: Game
Developer Tools** shows it: the integrated browser's developer tools for the
game's tab, or in the Simple Browser (where it's also a button above the
page), the window's.

## Settings

- `tide.server.path`: another language server to run.
- `tide.path`: another `tide` to run the game with.
- `tide.trace.server`: `messages` or `verbose` shows what VS Code and the
  language server say to each other, in the **Tide Trace** output.

In the paths, `~` is your home folder. `${workspaceFolder}` and relative paths
are the folder whose settings set them; set in your user or workspace
settings, they're the first open folder (for `tide.path`, the game's).
`${workspaceFolder:name}` is the open folder of that name.

After a server update, run **Tide: Restart Language Server**.

## Restricted Mode

In a workspace you haven't trusted, nothing from it runs: the language server
is the `tidels` that comes with tide (or one your user settings name outside
the workspace), never the workspace's `build/tools/tidels` or one its settings
name, and games don't run, since running one runs its code. Trusting the
workspace restarts the server.
