# Tide

A networking-first game engine. The simulation is deterministic, so players on
different machines, the web included, can share one world. Games are written in
the Tide language, which compiles to C and then to native code or WebAssembly.

**Docs, and the demo running in your browser:** https://tide-engine.dev

## Install

Windows, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.ps1 | iex
```

Linux and macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.sh | sh
```

This installs `tide` for your user (in `%LOCALAPPDATA%\Tide` or `~/.tide`) and
puts it on your `PATH`; no admin rights needed. For nightly versions, which
follow development day by day, set `TIDE_CHANNEL=nightly` before running the
script.

`tide` has its C compiler built in (clang), for native and web games alike.
Native games also need the system's own headers and libraries:

- **Windows:** nothing more; `tide` brings them.
- **Linux:** your distribution's C development files, which come with gcc:
  `sudo apt install build-essential` on Debian and Ubuntu, `sudo dnf install gcc`
  on Fedora.
- **macOS** (Apple Silicon): Apple's command-line tools, `xcode-select --install`.

## Make a game

A game is a folder of `.tide` files. Save this as `game.tide` in a new folder:

```csharp
component Ball
{
    float2 position;
    float2 velocity = float2(160, 120);
}

scene Main { }

event(Spawned) Setup(with Main)
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
| `tide run` | Builds the game and plays it |
| `tide run --web` | Builds it as a web page and opens it |
| `tide build` | Builds the game into `build/` |
| `tide build --release --web` | An optimized, self-contained `.html` (WebGPU, or WebGL 2 where the browser has none) |
| `tide schedule` | Shows which systems can run at the same time, and why the others wait |
| `tide add github.com/owner/repo` | Adds a package, at its newest commit, to `tide.packages` |
| `tide update` | Moves the packages to the newest commits of what they follow |

`tide help` lists every option. `tide` keeps its work in a hidden `.tide`
folder next to your files, which you can delete any time; it's ignored by git.

The [docs](https://tide-engine.dev) go through the language a topic at a time, and
[docs/spec.md](docs/spec.md) is its full spec.

## Stay up to date

```sh
tide upgrade            # the newest version of your channel
tide upgrade --nightly  # switch to nightly versions
tide upgrade --stable   # back to stable ones
```

`tide` also tells you, at most once a day, when a new version is out.

## Editors

Open your game's folder, the one you run `tide run` in, and every `.tide` file
in it is one game: completion, errors as you type with quick fixes, go to
definition, rename, formatting, moving a declaration to a file of its own and
more work across all of them.

- **VS Code, Cursor, VSCodium and Windsurf:** the installer adds Tide to
  the ones it finds, and `tide upgrade` keeps it up to date. Installed one
  later? Run `tide editors`.
- **JetBrains IDEs** (CLion, Rider, IntelliJ and the others, 2024.2 or later):
  install the Tide plugin
  from Settings > Plugins > Marketplace. It brings the LSP4IJ plugin it needs.
  Dark color schemes show type names as plain text; see
  [tools/tide-jetbrains/README.md](tools/tide-jetbrains/README.md#colors)
  to color them.

## Working on the engine

See [AGENTS.md](AGENTS.md): how to build the engine and its tests with CMake,
and how the project is organized.
