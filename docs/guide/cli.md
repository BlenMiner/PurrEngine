# The tide command

`tide` builds and runs games. A game is every `.tide` file in a folder and its subfolders, with the `.c` files and libraries there when it calls C (see [Calling C](../language/c-functions.md)), and the packages its `tide.packages` lists (see [Packages](packages.md)). Commands take the game's folder, or use the current one.

```
tide <command> [folder] [options]
```

| Command | What it does |
|---|---|
| `tide run [folder]` | Builds the game and plays it |
| `tide build [folder]` | Builds the game into `<folder>/build`, named after the folder |
| `tide schedule [folder]` | Shows which systems can run at the same time, and why the others wait |
| `tide add <source>` | Adds a package to the game in the current folder, from git or a folder |
| `tide update [package]` | Moves the game's packages from git to the newest commit of what they follow |
| `tide editors` | Adds Tide to VS Code, Cursor, VSCodium and Windsurf |
| `tide upgrade` | Updates tide to the newest version |
| `tide version` | Shows tide's version and channel, where it's installed, and which compilers it found |
| `tide help` | Lists every option |
| `tide cc <options>` | clang, built into tide, for C you build yourself (see [Calling C](../language/c-functions.md#where-the-c-goes)) |

## run and build

| Option | What it does |
|---|---|
| `--release` | Optimized, the way players get it |
| `--web` | A web page (WebGL 2), built with clang's WebAssembly target |
| `--android` | An Android app, for phones and the emulator (see [Android](#android)) |
| `--title <title>` | The window's title, over the game's `title` setting (default: that, or the folder's name) |
| `--stats` | Shows the frame rate, the ping, the bandwidth (what goes over the network each second, up and down), the tick, the entity count and the threads ticks run on |
| `-o <path>` | `build` only: where the program goes |
| `--no-open` | `run --web` only: serves the page without opening a browser |

`tide build --release --web` makes one self-contained `.html` file with the game inside. It opens straight from disk, and can be put online as it is.

### Threads on the web

A game's ticks run on every core when the page is cross-origin isolated: served with these two headers, which let it share its memory with the workers its threads run in.

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`tide run --web` serves the page with them. Where a page is put online, the host has to send them: on itch.io, it's the **SharedArrayBuffer support** option in the game's embed settings. A page without them, or opened from disk, runs the same game on one thread, with the same results, so web players on either kind of page play together. Workers only start once a tick has enough work for them.

## Android

`tide build --android` makes an Android app, `<folder>/build/<name>.apk`, for phones and tablets (arm64) and Android's emulator (x86-64), on Android 10 and up. With `--release`, it also makes `<name>.aab`, for [Google Play](#google-play). `tide run --android` builds it, installs it on the phone or emulator that's connected, starts it, and shows what it prints until it ends. `--host`, `--join` and `--connect` go to it as they do on a computer, and Android players play in the same rooms as everyone else.

To play on your phone, turn on USB debugging once: in **Settings > About phone**, tap **Build number** seven times, then turn on **USB debugging** in **System > Developer options**. Connect the phone, and allow your computer when it asks.

Building for Android needs two things of Google's: the NDK, Android's C library and headers, and adb, which installs apps. `tide` uses the ones Android Studio installs. Without them, the first `tide build --android` asks to download them from Google, under Google's license (the [Android SDK License Agreement](https://developer.android.com/studio/terms)), and keeps what it needs of them next to itself: about 30 MB. Set `TIDE_ACCEPT_ANDROID_LICENSE=1` to accept it without being asked, as a build server would.

- The app's ID is the game's `appId` setting (see [Settings](../language/basics.md#settings)). Without one, it's `dev.tide.<the game's name>`, which is fine for testing, but `--release` needs one.
- Its name under its icon is the game's `title` setting, or `--title`, or else the game's name.
- Its icon is `icon.png` in the game's folder: a square PNG, 512 by 512 pixels is plenty. Without one, it's Tide's.
- `tide` signs apps with a key it makes on your computer the first time, `~/.android/tide.pem`. A phone only takes an update to an app signed with the same key, so keep a copy of it somewhere safe, and don't share it: whoever has it can sign apps as you. `tide run --android` replaces an app another computer installed.
- To sign with another key (a build server's, or one you already have), set `TIDE_ANDROID_KEY` to its file: PEM, the private key (RSA, 2048 bits or more) and then its certificate. `openssl pkcs12 -in upload.p12 -nodes -out key.pem` makes one from a keystore; a `.jks` keystore becomes a `.p12` first with `keytool -importkeystore -srckeystore upload.jks -destkeystore upload.p12 -deststoretype PKCS12`.
- Each build's version code, which Android only updates an app to a higher one of, is the minutes since 2020 began, so every build is newer than the last. Where `SOURCE_DATE_EPOCH` is set, it stands in for now. The version people see is the game's `version` setting, like `1.2.0`, or else 1.0.
- The back button comes to the game as Escape, as in Unity.
- While `tide run --android` runs, saving a `.tide` or C file builds the app again, installs it and starts it over: a phone can't swap code into a running app, so the match starts over too. Type `r` and press Enter to start it over yourself; closing the app ends the run.

### Google Play

`<name>.aab` is an App Bundle, which is what Google Play takes: Play makes the APKs from it, each phone getting only the library for its CPU. To put a game on Play:

1. Make a developer account in the [Play Console](https://play.google.com/console) (Google charges a one-time fee), and create the app there.
2. Give the game its `appId`, and build it with `tide build --android --release`.
3. Upload `build/<name>.aab` to a release: internal testing is the quickest way to try it on your own phone. Play signs the APKs it makes with a key of its own, which it keeps (Play App Signing); `~/.android/tide.pem` is then your upload key, and Play only takes uploads signed with it.
4. For an update, build again and upload the new `.aab`: its version code is higher than the last one's.

Play's other requirements, like the version of Android an app is made for (Android 16) and support for phones with 16 KiB memory pages, `tide` meets itself. If you lose your upload key, Play can take a new one (the app's **App integrity** page asks for its certificate: the part of the key's file from `-----BEGIN CERTIFICATE-----` on).

## Hot reload

While `tide run` plays a game, saving a `.tide` file rebuilds it, and so does saving one of its C files, headers or libraries. The game carries on with the new code in the same window:

- If you changed only code (systems, views, event handlers, methods), the match and everything local, like menus, carry on where they are.
- If you changed data (components, fields, singletons, the input), the game is carried over to it by name, and goes on from where it was.
- If the new code has errors, `tide` prints them and the game keeps running its last build.

Carrying the game over works like this:

- Every field of the same name keeps its value. An `int` field that became a `float` is converted, and so is an int vector that became a float vector of the same size. An enum field keeps its member by name.
- A new field gets its declared default. So does a field whose type changed to one it doesn't convert to, like `float2` to `float3`. New text and list fields start empty.
- Renaming a field is removing it and adding a new one: its value is lost.
- An entity keeps its ID. When you remove a component, entities that had it keep their other components. An entity whose remaining components can't exist together in the new code is dropped.
- A changed default doesn't change existing entities, only new ones.
- A waiting [task](../language/tasks.md) carries on when its async function's code is the same, and is dropped when it changed: where it was waiting is gone.
- If the scene the game is in is gone, the game starts over.

`tide` says what happened after each reload, like `reloaded, and carried the game over to its new data layout; 1 field reset; 2 entities dropped`.

When the game gets into a state you don't want, type `r` and press Enter in `tide`'s terminal to start it over. With a match on several windows (`--host` in one, `--connect localhost` in another), each window reloads when you save, and players stay in the match. A window that joined another's match joins it again a second after that match ends without it: when the other window starts it over, or is still on a build with another data layout. On the web, a reload goes on with the match closed for now: the room closes, and the other players drop out.

`tide run --web` reloads too. `tide` serves the game's page at an address on your machine, like `http://127.0.0.1:52407/`, opens it in your browser, and keeps running until you press Ctrl+C. What each reload did shows in the browser's console. VS Code and JetBrains IDEs can play it beside your code instead (see [Editors](editors.md)).

Hot reload is only for `tide run`: `tide build` makes a plain program, or a page with nothing of it.

## Multiplayer

`tide run` can start a match others join, or join one. For `--host`, the game's `Main` scene must be the match's (see [Scenes](../language/scenes.md)); a game that starts in a menu does the same from code (see [Multiplayer](../language/multiplayer.md)).

| Option | What it does |
|---|---|
| `--host [port]` | Opens the match for others to join: in a room, whose code the game can show, and on UDP port 7777 by default (not on the web) |
| `--join <code>` | The match in the room with this code, like `K7QF2M` |
| `--connect <address>` | The match at an address, like `192.168.1.5` or `localhost:7777` (not on the web) |

## add and update

`tide add` and `tide update` change the `tide.packages` of the game in the current folder (see [Packages](packages.md)).

| Command | What it does |
|---|---|
| `tide add github.com/owner/repo` | The newest commit of the repository's default branch |
| `tide add github.com/owner/repo@dev` | The newest commit of the `dev` branch, which `tide update` follows |
| `tide add github.com/owner/repo@v1` | The newest `v1.x.y` tag, which `tide update` follows |
| `tide add github.com/owner/repo//physics` | The package in the repository's `physics` folder |
| `tide add ../shared` | A package in a folder on this machine |
| `tide update` | Every package from git, to the newest commit of what it follows |
| `tide update physics` | One, by its name, its repository's name or its source |

Both add the lines of the packages the game's packages need that it doesn't list yet.

## upgrade

| Option | What it does |
|---|---|
| `--nightly` | Follow nightly versions from now on |
| `--stable` | Follow stable versions from now on |
| `--version <v>` | Install exactly this version |

`tide` says, at most once a day, when a newer version is out. Set the `TIDE_NO_UPDATE_CHECK` environment variable to stop it looking, as on a build server.

## Files

`tide` keeps its work in a hidden `.tide` folder in the game's folder. You can delete it any time, and git ignores it. Packages from git are kept for every game, once each commit: in `%LOCALAPPDATA%\Tide\packages` on Windows and `~/.tide/packages` elsewhere, or where `TIDE_PACKAGES` says. `tide build` puts what it makes in `build/`, with the game's `.dll`, `.so` or `.dylib` libraries next to the program. C files and libraries in hidden folders or in `build/` aren't part of the game.
