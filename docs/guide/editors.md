# Editors

PurrLang's language server, `purrls`, comes with `purr`. Open your game's folder, the one you run `purr run` in, and every `.purr` file in it is one game. Across all of them, editors get:

- Completion, and errors as you type, with quick fixes.
- Hovers, go to definition, find usages and rename.
- Formatting, parameter hints and inlay hints.
- The outline, symbols across the game, folding and semantic highlighting.
- Moving a declaration to a file of its own.
- Each system's place in [the schedule](../engine/schedule.md) above it: its stage, and why it waits.

## VS Code, Cursor, VSCodium and Windsurf

The installer adds PurrLang to the ones it finds, and `purr upgrade` keeps it up to date. Installed one later? Run:

```sh
purr editors
```

The extension runs `purrls` from your `PATH`, or from where `purr` is installed. The `purrlang.server.path` setting runs another one. After updating the server, run **PurrLang: Restart Language Server**.

### Running the game

The play button above a `.purr` file runs its game, which is the folder you opened:

- **PurrLang: Run** plays it in a window of its own, as `purr run` does.
- **PurrLang: Run on the Web** plays it in a browser tab beside your code: VS Code's integrated browser, or the Simple Browser in editors without it. A game only gets keys while its tab has focus, and it pauses while its tab is hidden, so keep it in a group of its own.

Either runs `purr` in a terminal of its own, so saving a file reloads the game, and typing `r` and Enter in that terminal starts it over. Running it again starts the game over. The `purrlang.purr.path` setting runs another `purr`.

## JetBrains IDEs

CLion, Rider, IntelliJ and the others, 2024.2 or later: install the [PurrLang plugin](https://plugins.jetbrains.com/plugin/34610-purrlang) from **Settings > Plugins > Marketplace**. It brings the LSP4IJ plugin it needs.

JetBrains' dark color schemes draw type names like plain text, so PurrLang's types stay uncolored until you pick colors. Each kind of type has its own color under **Settings > Editor > Color Scheme > Language Server**:

- **Struct type**: components
- **Classes > Class reference** and **Class declaration**: singletons
- **Interface**: inputs
- **Type**: built-in types such as `Color`, `Entity` and `Devices`

### Running the game

Right-click a `.purr` file and pick **Run**, or press Ctrl+Shift+F10 in it (Ctrl+Shift+R on macOS), to run its game, which is the project's folder. The run is named after the folder, like `mygame`:

- **mygame** plays it in a window of its own, as `purr run` does.
- **mygame (web)** plays it on the web, in the **PurrLang Game** tool window. Its **Open DevTools** button shows the page's console, where each reload says what it did. The game only gets keys while the tool window has focus.

Both are PurrLang run configurations, which you can also make and edit under **Run > Edit Configurations**. `purr`'s output is in the Run tool window: saving a file reloads the game, and typing `r` and Enter there starts it over. A run configuration can run another `purr`.

## Other editors

`purr`'s `editors` folder holds the TextMate grammar (`purrlang-syntax`), which many editors can load for highlighting. Any editor with a language server client can run `purrls`, from `purr`'s `bin` folder, for everything else. It talks over stdin and stdout.
