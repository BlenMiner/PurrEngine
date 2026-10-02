# Packages

A package is Tide code, and C, that games share: a folder of `.tide` files, like a game's, without a `Main`. A game lists the packages it uses in a file named `tide.packages`, next to its own files, and `tide` builds them with it. Packages come from git repositories, at a commit, or from folders on your machine.

## Using one

`tide add` puts a package in the game in the current folder:

```
tide add github.com/someone/tide-physics
```

It finds the newest commit of the repository's default branch, downloads it, and writes a line for it in `tide.packages`, starting the file if there's none:

```
github.com/someone/tide-physics b01aac4d779e32c9c415283b65e40c86d691e7ab
```

The package's code is in its namespace, which is its name, so the game names what it declares with it, or imports it with `using` (see [Namespaces and files](../language/namespaces.md)):

```csharp
using Physics;

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Body { velocity = float3(0, 1, 0) });
}
```

`tide add` takes the repository however you have it: `https://github.com/someone/tide-physics.git`, `git@github.com:someone/tide-physics.git`, or as `tide.packages` writes it. Packages come from GitHub, GitLab, Codeberg and Bitbucket, and need no git on your machine: `tide` downloads each commit as its host's archive of it.

### Every build, the same code

A line always has its commit, and the game builds with that commit's code, whoever builds it and wherever: your machine, someone else's, a build server, the web build and the desktop one. They have to: a match's players simulate it together, so each of them has to run the same code. A branch or a tag can move, so a line never builds with "the newest" of anything.

`tide` keeps each commit it downloads, once for every game on your machine, in `%LOCALAPPDATA%\Tide\packages` on Windows and `~/.tide/packages` elsewhere (or where the `TIDE_PACKAGES` environment variable says). A game whose packages aren't there yet, like one you just cloned, downloads them the first time `tide run` or `tide build` builds it.

## Updating

`tide update` moves packages to the newest commit of what they follow, and says what changed:

```
tide update            # every package from git
tide update physics    # one, by its name, its repository's name, or its source
```

```
tide: Physics b01aac4 -> 7d3e9f1 (main)
```

A line follows its repository's default branch, or what comes after `@` in it:

| Line | `tide update` moves it to |
|---|---|
| `github.com/someone/tide-physics b01aac4...` | The newest commit of the default branch |
| `github.com/someone/tide-physics@dev b01aac4...` | The newest commit of the `dev` branch |
| `github.com/someone/tide-physics@v1 b01aac4...` | The newest `v1.x.y` tag |
| `github.com/someone/tide-physics@v1.2 b01aac4...` | The newest `v1.2.x` tag |

A version, like `v1` or `1.2`, follows tags, leaving out pre-releases like `v1.3.0-rc.1`; anything else is a branch. `tide add` takes the same: `tide add github.com/someone/tide-physics@v1`.

A line can leave its commit out, as when you write one by hand: `tide update` puts the newest one in. `tide update` only changes the commits in `tide.packages`, so your comments and the order of its lines stay, and what changed shows in git like any other change.

## The packages a package needs

A package can use other packages, which its own `tide.packages` lists. The game's `tide.packages` lists every package the game builds with, those its packages need included, so that one file says everything the game builds with. `tide add` and `tide update` add the lines of what's missing, and say so:

```
tide: added github.com/someone/tide-math 9f2c1e0..., which Physics needs
```

When the game and one of its packages list another package at different commits, the game's line is the one it builds with.

## Packages in folders

A line can be a folder instead, relative to the game's, for a package you're writing, or one a few games of yours share:

```
../shared
github.com/someone/tide-physics b01aac4d779e32c9c415283b65e40c86d691e7ab
```

A folder's files build as they are, with no commit, and `tide run` reloads the game when they change, as it does for the game's own. `tide add ../shared` adds one.

## Writing a package

A package is a folder whose `tide.packages` starts with its name, which is its namespace:

```
package Physics
tide 0.3
github.com/someone/tide-math 9f2c1e07d4b8a3f6e5c2d1b0a9f8e7d6c5b4a392
```

- Every file of the package puts what it declares in its namespace, `namespace Physics;`, or in one inside it, like `namespace Physics.Joints;`.
- What a game has one of is the game's: a package can't declare `Main`, the input or the settings. Its systems can take `Devices`, which the game's input sends with its own fields.
- `tide 0.3` is the oldest `tide` it builds with. With an older one, a game that uses it says so, and to run `tide upgrade`.
- Its other lines are the packages it needs, as a game lists them. One from git only needs packages from git, since a folder on your machine isn't on anyone else's.
- Its C goes in its folder, as a game's does (see [Calling C](../language/c-functions.md)), and compiles with the game.
- A package's systems run before the game's: packages' files compile first, each package after those it needs, and the game's files last (see [Systems](../language/systems.md#order)).

To try a package as you write it, make a game in a folder of its own inside it, whose `tide.packages` lists the package's folder:

```
physics/
  tide.packages      package Physics
  body.tide
  example/
    tide.packages    ..
    main.tide        scene Main { } ...
```

A folder with a `tide.packages` of its own is a game or a package, so `example/` isn't part of the package, and the package isn't part of a game that holds it in a folder: a game uses it by listing it. Put a package's repository online, and `tide add` takes it. A repository can hold several packages, each in a folder of it, which a line names after `//`:

```
github.com/someone/tide-packages//physics b01aac4d779e32c9c415283b65e40c86d691e7ab
```

### Code for other versions of tide

Tide still changes from version to version, so a package can have code for the versions it supports, which `#if` picks between (see [Namespaces and files](../language/namespaces.md#code-for-other-versions-of-tide)):

```csharp
#if TIDE_0_4_OR_NEWER
    // Code that uses what tide 0.4 added
#else
    // The same, as tide 0.3 writes it
#endif
```

## Editors

The editors' language server analyzes a game with its packages, so completion, errors and go to definition reach into them. A package's file you open is analyzed with the open game that uses it, or else on its own, with what it needs. Packages from git it doesn't have yet are left out until `tide` downloads them.
