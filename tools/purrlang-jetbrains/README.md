# PurrLang plugin for JetBrains IDEs

[On JetBrains Marketplace](https://plugins.jetbrains.com/plugin/34610-purrlang).
Highlighting from the TextMate grammar in `tools/purrlang-syntax`, and
everything else from `purrls` through the LSP4IJ plugin, which JetBrains
Marketplace installs with it. It runs in every JetBrains IDE from 2024.2 on.

The plugin runs, in this order: the project's own `build/tools/purrls` (in
PurrEngine's repo, the server your build made), `purrls` on `PATH`, or the one
where purr's installers put it.

## Running the game

PurrLang run configurations run `purr run` on a game's folder, in the Run tool
window: natively, or on the web, whose page shows in the **PurrLang Game**
tool window (JCEF; the system's browser in an IDE without it). A `.purr`
file's context menu offers both, for the game it's in: the project's folder,
or in PurrEngine's repo, the game `build/tools/games.txt` lists it in.

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

## Building

It needs a JDK 21 or later to build; Gradle downloads the JDK 21 it compiles
with, and the IntelliJ Platform it compiles against, the first time.

```sh
./gradlew buildPlugin                  # build/distributions/purrlang-<version>.zip
./gradlew runIde --args="<game folder>"  # an IDE with the plugin, for trying it
./gradlew verifyPlugin                 # against IntelliJ 2024.2 and the newest CLion
./gradlew verifyPlugin -PverifyIde="C:/Program Files/JetBrains/CLion 2026.2.3"
```

CI builds and verifies it on every push, and publishes it to Marketplace with
each stable release (see `.github/workflows/build.yml`).
