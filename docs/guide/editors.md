# Editors

Tide's language server, `tidels`, comes with `tide`. Open your game's folder, the one you run `tide run` in, and every `.tide` file in it is one game, with its [packages](packages.md). Across all of them, editors get:

- Completion, and errors as you type, with quick fixes: a name spelled wrong, a missing `mut`, an error nothing handles, a name from another namespace.
- Hovers, go to definition or to a value's type, find usages, the uses of a name highlighted, and rename.
- Go to implementation, from an event to its handlers and from the input to its `Sample` and `Sanitize`, and the call hierarchy: what calls a function, and what it calls.
- Formatting, of a file or a selection, parameter hints and inlay hints.
- The outline, symbols across the game, folding, growing a selection, and semantic highlighting.
- Moving a declaration to a file of its own.
- Each system's place in [the schedule](../engine/schedule.md) above it: its stage, and why it waits.

## VS Code, Cursor, VSCodium and Windsurf

The installer adds Tide to the ones it finds, and `tide upgrade` keeps it up to date. Installed one later? Run:

```sh
tide editors
```

The extension runs `tidels` from your `PATH`, or from where `tide` is installed; in a folder with a `build/tools/tidels`, like Tide's own repo once it's built, it runs that one. The `tide.server.path` setting runs another one: in it, `~` is your home folder, and `${workspaceFolder}` and relative paths are the folder whose settings set it. After updating the server, run **Tide: Restart Language Server**. The `tide.trace.server` setting shows what the editor and the server say to each other, in the **Tide Trace** output.

In a folder you haven't trusted (VS Code's Restricted Mode), nothing from it runs: the server is the `tidels` that comes with `tide`, and games don't run, since running one runs its code. Trusting the folder restarts the server.

### Running the game

The play button above a `.tide` file runs its game, which is the folder you opened, or the folder of the game it's in when that has a `tide.packages` of its own; a [package](packages.md)'s file runs the open game that lists it:

- **Tide: Run** plays it in a window of its own, as `tide run` does.
- **Tide: Run on the Web** plays it in a browser tab beside your code: VS Code's integrated browser, or the Simple Browser in editors without it. A game only gets keys while its tab has focus, and the editor may pause it while its tab is hidden, so keep it in a group of its own.
- **Tide: Run on Android** plays it on the Android phone or emulator that's connected, as `tide run --android` does (see [Android](cli.md#android)).

Each runs `tide` in a terminal of its own, so saving a file reloads the game (on Android, builds it again and starts it over), and typing `r` and Enter in that terminal starts it over. Running it again starts the game over. The first run on Android may ask to download Android's tools from Google: type `y` and Enter in that terminal to accept. The `tide.path` setting runs another `tide`.

On the web, each reload says what it did in the page's console: **Tide: Game Developer Tools** shows it.

## JetBrains IDEs

CLion, Rider, IntelliJ and the others, 2024.2 or later: install the Tide plugin from **Settings > Plugins > Marketplace**. It brings the LSP4IJ plugin it needs.

The plugin runs the `tidels` chosen under **Settings > Languages & Frameworks > Tide**, if any; else the project's own `build/tools/tidels`, like Tide's repo once it's built, once you trust the project; else the one on your `PATH`, or where `tide` is installed. When there's none, `.tide` files say so above the code, with links to install `tide`, choose a `tidels`, or try again. The **Language Servers** tool window restarts the server, after an update, and shows what it and the IDE say to each other.

JetBrains' dark color schemes draw type names like plain text, so Tide's types stay uncolored until you pick colors. Each kind of type has its own color under **Settings > Editor > Color Scheme > Language Server**:

- **Struct type**: components
- **Classes > Class reference** and **Class declaration**: singletons
- **Interface**: inputs
- **Type**: built-in types such as `Color`, `Entity` and `Devices`

### Running the game

Right-click a `.tide` file and pick **Run**, or press Ctrl+Shift+F10 in it (Ctrl+Shift+R on macOS), to run its game, which is the project's folder, or the folder of the game it's in when that has a `tide.packages` of its own; a [package](packages.md)'s file runs the project's game when it lists the package. The run is named after the folder, like `mygame`:

- **mygame** plays it in a window of its own, as `tide run` does.
- **mygame (web)** plays it on the web, in the **Tide Game** tool window. Its **Open DevTools** button shows the page's console, where each reload says what it did. The game only gets keys while the tool window has focus.
- **mygame (Android)** plays it on the Android phone or emulator that's connected, as `tide run --android` does (see [Android](cli.md#android)).

They're Tide run configurations, which you can also make and edit under **Run > Edit Configurations**, where **Play it** picks among the three. `tide`'s output is in the Run tool window, where an error's place links to the file: saving a file reloads the game (on Android, builds it again and starts it over), and typing `r` and Enter there starts it over. The first run on Android may ask to download Android's tools from Google: type `y` and Enter there to accept. A run configuration can run another `tide`.

## Other editors

`tide`'s `editors` folder holds the TextMate grammar (`tide-syntax`), a VS Code-style bundle, for highlighting in editors that load those. Any editor with a language server client can run `tidels`, from `tide`'s `bin` folder, for everything else. It talks over stdin and stdout.
