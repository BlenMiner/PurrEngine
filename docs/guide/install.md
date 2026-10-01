# Install

Tide comes as one command, `tide`, which builds and runs games. It has its C compiler built in (clang), for native and web games alike.

## Windows

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.ps1 | iex
```

Windows needs nothing else: `tide` brings the headers and libraries games build with.

## Linux and macOS

```sh
curl -fsSL https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.sh | sh
```

Native games also need the system's own headers and libraries:

- **Linux** (x64): your distribution's C development files, which come with gcc: `sudo apt install build-essential` on Debian and Ubuntu, `sudo dnf install gcc` on Fedora.
- **macOS** (Apple Silicon): Apple's command-line tools, `xcode-select --install`.

## What the installer does

It installs `tide` for your user, in `%LOCALAPPDATA%\Tide` on Windows or `~/.tide` elsewhere, and puts its `bin` folder on your `PATH`. It needs no admin rights. Open a new terminal afterwards so the `PATH` change applies, then check it worked:

```sh
tide version
```

This shows tide's version, and which compilers it found. If you run the installer again when `tide` is already there, it upgrades it instead.

The installer also adds Tide to the editors it finds (see [Editors](./editors.md)).

## Nightly versions

Stable versions come out now and then. Nightly versions follow development day by day. For a nightly version, set `TIDE_CHANNEL` before running the installer:

::: code-group

```powershell [Windows]
$env:TIDE_CHANNEL = 'nightly'
irm https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.ps1 | iex
```

```sh [Linux and macOS]
curl -fsSL https://raw.githubusercontent.com/BlenMiner/tide-engine/release/install.sh | TIDE_CHANNEL=nightly sh
```

:::

::: tip
This site follows development, so it describes the nightly version. Some of what it shows may not be in the stable version yet.
:::

## Stay up to date

```sh
tide upgrade            # the newest version of your channel
tide upgrade --nightly  # switch to nightly versions
tide upgrade --stable   # back to stable ones
```

`tide` also tells you, at most once a day, when a new version is out. Upgrades check every download against the release's checksums.

`tide upgrade` only ever moves forward. Switching channels, or `tide upgrade --version <v>`, installs what you ask for, even an older version.
