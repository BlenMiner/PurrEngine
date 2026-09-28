# PurrEngine

A networking-first game engine. The simulation is deterministic, so players on
different machines, the web included, can share one world. Games are written in
PurrLang, which compiles to C and then to native code or WebAssembly.

## Install

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.ps1 | iex
```

Linux and macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.sh | sh
```

This installs `purr` for your user (in `%LOCALAPPDATA%\Purr` or `~/.purr`) and
puts it on your `PATH`; no admin rights needed. For nightly versions, which
follow development day by day, set `PURR_CHANNEL=nightly` before running the
script.

`purr` builds games with clang:

- **Windows:** [LLVM](https://github.com/llvm/llvm-project/releases) (or
  `winget install LLVM.LLVM`), and Visual Studio or its Build Tools with
  *Desktop development with C++*, for the system libraries programs link with.
- **Linux:** clang and lld from your package manager.
- **macOS** (Apple Silicon): Apple's command-line tools (`xcode-select --install`)
  for native games. Apple's clang can't build WebAssembly, so web games use
  Homebrew's: `brew install llvm lld`.

On Windows and Linux, web games need nothing more: the same clang builds them for
WebAssembly.

`purr version` shows which compilers it found.

## Make a game

A game is a folder of `.purr` files. Save this as `game.purr` in a new folder:

```csharp
component Ball
{
    float2 position;
    float2 velocity = float2(160, 120);
}

system Main()
{
    Spawn(Ball);
}

system Move(mut Ball ball, Time time)
{
    ball.position += ball.velocity * time.dt;
    if (Math.Abs(ball.position.x) > 300) ball.velocity.x = -ball.velocity.x;
    if (Math.Abs(ball.position.y) > 200) ball.velocity.y = -ball.velocity.y;
}

view DrawBalls(Ball ball)
{
    Draw.Circle(ball.position, 20, Color.red);
}
```

Then, in that folder:

| Command | What it does |
|---|---|
| `purr run` | Builds the game and plays it |
| `purr run --web` | Builds it as a web page and opens it |
| `purr build` | Builds the game into `build/` |
| `purr build --release --web` | An optimized, self-contained `.html` (WebGL 2) |
| `purr schedule` | Shows which systems can run at the same time, and why the others wait |

`purr help` lists every option. `purr` keeps its work in a `.purr` folder next
to your files, which you can delete any time; it's ignored by git.

The language is described in [docs/purrlang.md](docs/purrlang.md).

## Stay up to date

```sh
purr upgrade            # the newest version of your channel
purr upgrade --nightly  # switch to nightly versions
purr upgrade --stable   # back to stable ones
```

`purr` also tells you, at most once a day, when a new version is out.

## Editors

Open your game's folder, the one you run `purr run` in, and every `.purr` file
in it is one game: completion, errors as you type, go to definition, rename,
formatting and more work across all of them.

- **VS Code, Cursor, VSCodium and Windsurf:** the installer adds PurrLang to
  the ones it finds, and `purr upgrade` keeps it up to date. Installed one
  later? Run `purr editors`.
- **JetBrains IDEs** (CLion, Rider, IntelliJ and the others, 2024.2 or later):
  install the [PurrLang plugin](https://plugins.jetbrains.com/plugin/34610-purrlang)
  from Settings > Plugins > Marketplace. It brings the LSP4IJ plugin it needs.
  Dark color schemes show type names as plain text; see
  [tools/purrlang-jetbrains/README.md](tools/purrlang-jetbrains/README.md#colors)
  to color them.

## Working on the engine

See [AGENTS.md](AGENTS.md): how to build the engine and its tests with CMake,
and how the project is organized.
