# PurrLang language server for JetBrains IDEs

Completion, errors as you type, hover, go to definition, find usages, rename,
formatting, parameter hints, the structure view and semantic highlighting for
`.purr` files, from `purrls` (see `compiler/lsp/`).

1. Build the project once. Every build copies the server to `build/tools/purrls`.
2. Install the **LSP4IJ** plugin (Settings > Plugins > Marketplace).
3. Settings > Languages & Frameworks > Language Servers > `+`. In **Template**,
   choose **Import from custom template...** and pick this folder.
4. Open a `.purr` file.

Keep the TextMate bundle in `tools/purrlang-syntax` registered too: it colors
keywords, numbers and comments, and semantic highlighting adds what names mean.

## Colors

JetBrains' dark schemes draw type names like plain text, as in Java, so
PurrLang's types stay uncolored until you pick colors. The server tells each
kind of type apart, so each can have its own color under Settings > Editor >
Color Scheme > **Language Server**:

- **Struct type**: components
- **Classes > Class reference** and **Class declaration**: singletons
- **Interface**: inputs
- **Type**: built-in types such as `Color`, `Entity` and `Devices`

Built-in value types (`float3`, `int`, `bool`), `Sample` and `Sanitize` use the
keyword color, and attributes (`[After]`, `[Clamp]`) the **Decorator** color.
After a server update, restart it (Language Servers tool window) to see changes.
