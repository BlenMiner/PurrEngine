# Editors

Tide's language server, `tidels`, comes with `tide`. Open your game's folder, the one you run `tide run` in, and every `.tide` file in it is one game. Across all of them, editors get:

- Completion, and errors as you type, with quick fixes.
- Hovers, go to definition, find usages and rename.
- Formatting, parameter hints and inlay hints.
- The outline, symbols across the game, folding and semantic highlighting.
- Moving a declaration to a file of its own.
- Each system's place in [the schedule](../engine/schedule.md) above it: its stage, and why it waits.

## VS Code, Cursor, VSCodium and Windsurf

The installer adds Tide to the ones it finds, and `tide upgrade` keeps it up to date. Installed one later? Run:

```sh
tide editors
```

The extension runs `tidels` from your `PATH`, or from where `tide` is installed. The `tide.server.path` setting runs another one. After updating the server, run **Tide: Restart Language Server**.

### Running the game

The play button above a `.tide` file runs its game, which is the folder you opened:

- **Tide: Run** plays it in a window of its own, as `tide run` does.
- **Tide: Run on the Web** plays it in a browser tab beside your code: VS Code's integrated browser, or the Simple Browser in editors without it. A game only gets keys while its tab has focus, and the editor may pause it while its tab is hidden, so keep it in a group of its own.

Either runs `tide` in a terminal of its own, so saving a file reloads the game, and typing `r` and Enter in that terminal starts it over. Running it again starts the game over. The `tide.path` setting runs another `tide`.

## JetBrains IDEs

CLion, Rider, IntelliJ and the others, 2024.2 or later: install the Tide plugin from **Settings > Plugins > Marketplace**. It brings the LSP4IJ plugin it needs.

JetBrains' dark color schemes draw type names like plain text, so Tide's types stay uncolored until you pick colors. Each kind of type has its own color under **Settings > Editor > Color Scheme > Language Server**:

- **Struct type**: components
- **Classes > Class reference** and **Class declaration**: singletons
- **Interface**: inputs
- **Type**: built-in types such as `Color`, `Entity` and `Devices`

### Running the game

Right-click a `.tide` file and pick **Run**, or press Ctrl+Shift+F10 in it (Ctrl+Shift+R on macOS), to run its game, which is the project's folder. The run is named after the folder, like `mygame`:

- **mygame** plays it in a window of its own, as `tide run` does.
- **mygame (web)** plays it on the web, in the **Tide Game** tool window. Its **Open DevTools** button shows the page's console, where each reload says what it did. The game only gets keys while the tool window has focus.

Both are Tide run configurations, which you can also make and edit under **Run > Edit Configurations**. `tide`'s output is in the Run tool window: saving a file reloads the game, and typing `r` and Enter there starts it over. A run configuration can run another `tide`.

## Other editors

`tide`'s `editors` folder holds the TextMate grammar (`tide-syntax`), which many editors can load for highlighting. Any editor with a language server client can run `tidels`, from `tide`'s `bin` folder, for everything else. It talks over stdin and stdout.
