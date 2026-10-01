# Tide for VS Code

Completion, errors as you type with quick fixes, hover, go to definition, find
usages, rename, formatting, parameter hints, inlay hints, the outline, symbols
across the game, folding, semantic highlighting, moving a declaration to a file
of its own, and each system's schedule above it, for `.tide` files. It works in
VS Code, Cursor, VSCodium and Windsurf.

tide's installer adds this extension to the editors it finds, and `tide
editors` adds it again, for example after installing a new editor. The
language server is `tidels`, which comes with tide. The extension runs it from
`PATH`, or from where tide is installed. In Tide's own repo, it runs the
build's `build/tools/tidels`.

Open the folder of your game, the one you'd run `tide run` in: every `.tide`
file in it and its subfolders is one game.

## Running the game

The play button above a `.tide` file runs its game:

- **Tide: Run** plays it in a window of its own, as `tide run` does.
- **Tide: Run on the Web** plays it in a browser tab beside the code:
  VS Code's integrated browser, or the Simple Browser in editors without it.
  The game gets keys while its tab has focus, and pauses while it's hidden.

Either runs `tide` in a terminal of its own, so saving a file reloads the
game, and typing `r` and Enter there starts it over. Running it again starts
the game over.

## Settings

- `tide.server.path`: another language server to run.
  `${workspaceFolder}` is the open folder.
- `tide.path`: another `tide` to run the game with.

After a server update, run **Tide: Restart Language Server**.
