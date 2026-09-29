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

## JetBrains IDEs

CLion, Rider, IntelliJ and the others, 2024.2 or later: install the [PurrLang plugin](https://plugins.jetbrains.com/plugin/34610-purrlang) from **Settings > Plugins > Marketplace**. It brings the LSP4IJ plugin it needs.

JetBrains' dark color schemes draw type names like plain text, so PurrLang's types stay uncolored until you pick colors. Each kind of type has its own color under **Settings > Editor > Color Scheme > Language Server**:

- **Struct type**: components
- **Classes > Class reference** and **Class declaration**: singletons
- **Interface**: inputs
- **Type**: built-in types such as `Color`, `Entity` and `Devices`

## Other editors

`purr`'s `editors` folder holds the TextMate grammar (`purrlang-syntax`), which many editors can load for highlighting. Any editor with a language server client can run `purrls`, from `purr`'s `bin` folder, for everything else. It talks over stdin and stdout.
