# Install

PurrEngine comes as one command, `purr`, which builds and runs games. It has its C compiler built in (clang), for native and web games alike.

## Windows

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.ps1 | iex
```

Windows needs nothing else: `purr` brings the headers and libraries games build with.

## Linux and macOS

```sh
curl -fsSL https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.sh | sh
```

Native games also need the system's own headers and libraries:

- **Linux** (x64): your distribution's C development files, which come with gcc: `sudo apt install build-essential` on Debian and Ubuntu, `sudo dnf install gcc` on Fedora.
- **macOS** (Apple Silicon): Apple's command-line tools, `xcode-select --install`.

## What the installer does

It installs `purr` for your user, in `%LOCALAPPDATA%\Purr` on Windows or `~/.purr` elsewhere, and puts its `bin` folder on your `PATH`. It needs no admin rights. Open a new terminal afterwards so the `PATH` change applies, then check it worked:

```sh
purr version
```

This shows purr's version, and which compilers it found. If you run the installer again when `purr` is already there, it upgrades it instead.

The installer also adds PurrLang to the editors it finds (see [Editors](./editors.md)).

## Nightly versions

Stable versions come out now and then. Nightly versions follow development day by day. For a nightly version, set `PURR_CHANNEL` before running the installer:

::: code-group

```powershell [Windows]
$env:PURR_CHANNEL = 'nightly'
irm https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.ps1 | iex
```

```sh [Linux and macOS]
curl -fsSL https://raw.githubusercontent.com/BlenMiner/PurrEngine/release/install.sh | PURR_CHANNEL=nightly sh
```

:::

::: tip
This site follows development, so it describes the nightly version. Some of what it shows may not be in the stable version yet.
:::

## Stay up to date

```sh
purr upgrade            # the newest version of your channel
purr upgrade --nightly  # switch to nightly versions
purr upgrade --stable   # back to stable ones
```

`purr` also tells you, at most once a day, when a new version is out. Upgrades check every download against the release's checksums.

`purr upgrade` only ever moves forward. Switching channels, or `purr upgrade --version <v>`, installs what you ask for, even an older version.
