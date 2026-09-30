# PurrEngine: Agent Guide

Rules and decisions for anyone, human or AI agent, working on PurrEngine. Only agreed decisions go in this file. Open questions stay out until they're decided.

## Working agreement

- The project owner is the lead programmer and writes most of the code.
- Agents help with specific, scoped tasks. Do what was asked. Don't widen scope, scaffold systems, or refactor nearby code unless asked.
- If it's unclear whether the owner wants code or discussion, ask.
- **Exception: Claude owns the transpiler.** The owner decides the language design (what it should do, its syntax and semantics). Claude implements and maintains the transpiler, including its tests and build integration, and proposes designs for the owner to approve.
- The contract between the two sides is the C the transpiler generates and the engine runtime functions that code calls. The owner writes the runtime. Any runtime function the generated code needs is agreed on before either side depends on it.

## What PurrEngine is

A networking-first game engine:

- The simulation is deterministic and uses floats, not fixed point, on supported hardware.
- User code and the engine's built-in systems are written in a custom language that compiles to native code.
- An ECS runs systems on multiple threads automatically. Data dependencies come from the ECS and the language, so users never have to think about threads.
- Graphics work is kept to a minimum. Rendering is not the focus.

## Platforms

- The main targets are desktop and consoles.
- The web (WebAssembly, rendering with WebGL) is actively supported and tested, not an afterthought. Changes must keep the web build working.
- Determinism holds on every platform, the web included, so web and desktop players can share one simulation.

## Rendering and platform

- raylib handles windows, input and rendering for now. The web build renders with WebGL 2, through our own JavaScript (see Web builds).
- Rendering stays disconnected from the simulation, so it can be replaced later (for example for consoles):
  - The simulation never includes raylib or any platform header.
  - The view reads the world and draws it. It never writes simulation state.
  - The platform layer's only link to the simulation is filling `Devices`.
  - Game code and raylib never share a source file: generated headers name types after the game's components (`Transform`), and raylib defines many of the same names. Hosts include `purr/platform.h`, which doesn't include raylib, and `purr_platform` keeps raylib private.
- `purr/run.h` is the standard host: `purr_run(&(purr_run_desc){.title = "...", .argc = argc, .argv = argv})` opens a window and runs the game in a session (`purr/session.h`), sampling this machine's input from the devices once per tick it runs, and draws its views and their GUI. When `Main` is the match's, it plays at once, or hosts or joins with `--host [port]` or `--join address`. This machine's player joins before the first tick. A game needs no other C. Its loop is in `purr/host.h`, which runs a game through a table of its functions (`purr_host_game`), so that `purr run` can swap builds (see Hot reloading).
- Views are written in PurrLang (`view` declarations). Their `Draw` calls record commands into a renderer-agnostic draw list (`purr/draw.h`). The platform layer renders the list (`purr_platform_draw`), so replacing raylib only means rewriting that function.
- The GUI (`GUI` and `GUILayout` in PurrLang, `purr/gui.h` in C) is immediate mode, like Draw, and local: widgets change local state, never the match. It records into a list of its own, which `purr_gui_end` adds after the world's, in pixels (from the top left, y down), so widgets keep their size when the window changes. It measures text with the platform's font (`purr_platform_measure_text`). Views read the devices through it (`Devices` in PurrLang): `purr_gui_begin` works out what changed since last frame and hides what the GUI uses.

## Tech stack

- The engine is written in C and compiled with clang on every platform.
- The custom language transpiles to C.
- User code and engine code are compiled and optimized together, so calls between them cost nothing.

## Building

Requires CMake 3.25+, Ninja, and clang. The build finds clang automatically, checking `$LLVM_ROOT/bin`, then a standalone LLVM install, then `PATH`, then Visual Studio's bundled clang. This works with any CMake invocation, not just the presets.

- Configure, build and test in one step: `cmake --workflow --preset debug`
- Build only: `cmake --build --preset debug`
- Run tests: `ctest --preset debug`, or run `build/debug/bin/purr_tests [name-filter]` directly
- Run the sandbox: `build/debug/bin/sandbox`
- Run the demo: `build/debug/bin/demo`, or open `build/web-release/bin/demo.html` in a browser after a web build. Two players: `demo --host` in one window and `demo --join 127.0.0.1` in another (or another machine's address).

There are two presets for everyday work. `debug` has no optimization. `release` is optimized and keeps debug info for profiling. Web builds, MinGW builds and the package have their own (see below).

Every native build also copies the language server to `build/tools/purrls`, the fixed path editors run: the VS Code extension and the JetBrains plugin use it when the open folder is this repo (see `tools/`). On Windows, a build replaces the server while an editor still runs the old one: the running file is renamed aside and deleted by a later build.

The first configure downloads raylib (see `cmake/Raylib.cmake`). Configure with `-DPURR_PLATFORM=OFF` to build without the platform layer and the demo, for example offline.

### Web builds

- The `web-debug` and `web-release` presets build everything as WebAssembly with clang's own wasm target and wasi-libc, not Emscripten: `cmake --workflow --preset web-debug` (see `cmake/wasi-toolchain.cmake`). They need clang with `wasm-ld` (LLVM's releases have it; on Linux, the distribution's `lld`) and Node. wasi-sdk's C library is downloaded once into `build/wasi-sdk-<version>`, pinned.
- Tests run under Node's WASI through CTest (`cmake/run_wasi.mjs`), so the determinism hashes cover exactly what ships. Games generate their C with a native purrc, which web builds build first in `<build>/host` (`cmake/HostPurrc.cmake`).
- In the browser, `platform/web/purr.js` runs the program: the canvas, input and frame loop (`platform/web/purr_web.h`), the OpenGL ES 3 functions rlgl calls on WebGL 2, and the few WASI functions wasi-libc needs. raylib gets our platform backend (`platform/web/raylib/rcore_web_purr.c`), patched into its `rcore.c` by `cmake/Raylib.cmake`. A web page is one self-contained `.html` with the program inside (`cmake/web_page.mjs`, or purr).
- Web builds are single-threaded. Threads need a cross-origin isolated page, and running in parallel never changes results anyway.
- Before finishing a change, the native and web test suites must both pass.

### MinGW builds

- On Windows, the `mingw-release` preset builds everything for the MinGW-w64 target (`x86_64-w64-windows-gnu`, with the UCRT), which purr builds Windows games for (see Packaging and releases): `cmake --workflow --preset mingw-release`. Everyday Windows builds keep Visual Studio's target.
- Its headers, C runtime and compiler runtime come from llvm-mingw, downloaded once into `build/llvm-mingw-<version>`, pinned (`cmake/mingw-toolchain.cmake`). clang is the usual one, linking with lld.

### Cross-platform determinism tests

- `tests/test_crossplatform.c` and `compiler/tests/e2e/crossplatform` hash the exact bits of math results and of a full simulation. The hashes must be identical on every platform and in every configuration.
- If math or generated code changes the results on purpose, update the expected hashes from one platform, then confirm every other platform and configuration agrees.
- Test code must not depend on C's unspecified argument evaluation order: clang goes right to left on Windows and left to right on WebAssembly. Draw random inputs into locals first.
- The demo's smoke test (`demo --smoke`, or `demo.html?smoke`) plays a scripted session through the real platform layer. It checks a hash of the simulation (`SMOKE_HASH` in `demo/main.c`, the same on every platform) and pixels read back from the renderer. The native presets run it in a hidden window, so they need a desktop session with OpenGL. The web presets run it in headless Chrome or Edge on the browser's software WebGL, and skip it if neither browser is found.

### PurrLang programs

`purr_add_game(<target> [SOURCES <file.purr|file.c>...] [HOST <file.c>...] [NAME <name>] [TITLE <title>] [STATS])` builds a game as the program `<target>` (see `cmake/PurrLang.cmake`):

- The game is every `.purr` file in the current source folder and its subfolders; the next build picks up new files. `SOURCES` lists the files instead, for tests and folders that hold several games.
- The game's C (its `extern` functions') is every `.c` file there but the `HOST` ones and those in hidden folders or `build/`, or the `.c` files in `SOURCES`. Unlike `purr`, it doesn't pick up prebuilt libraries.
- Without `HOST`, the game is the whole program and needs no C: a generated `main` runs it in a window through `purr/run.h`. On the web it's `<target>.html`.
- With `HOST`, those C files are the program (tests, the demo's smoke test, custom hosts). They include `<name>.h`, where `NAME` defaults to `<target>`.
- `purrc` compiles all the files together, in order of their paths. The generated files regenerate whenever a `.purr` file or `purrc` changes.
- `<target>_schedule` is a build target that prints the game's schedule: which systems can run at the same time, and why the others wait.
- Every game's files are listed in `build/tools/games.txt`, one `<game>\t<path>` per line. A path ending in `/` is a folder: every `.purr` file in it and its subfolders, so editors see new files before the next build. `purrls` reads it, and the one in `build/tools/` of any folder open in the editor, to analyze a game's files together, in `purrc`'s order. A file in no game belongs to the open folder it's in, as `purr run` would build that folder, unless a manifest lists games in that folder: then it's analyzed alone.

The generated header is the API between the game and the host. Namespaced declarations have their namespace in their C name: `Combat.Health` is `Combat_Health`, read with `purr_get_Combat_Health`.

- `purr_world`: the whole simulation state as plain data. Copying it is a snapshot. When fields hold text or lists, it has a `heap` they're kept in; hosts read a text field with `purr_text_read(&w->heap, field)`.
- `purr_world_init(w, dt)`: clears the world, sets `Time.dt` and singleton defaults, and loads the `Main` scene if it's the match's.
- `PURR_MAIN_IS_LOCAL` is defined when `Main` is a local scene: the program starts outside any match.
- `purr_world_tick(w)`: runs every system once, then applies structural changes. With no scene left, it loads `Main` again if it's the match's; `purr_frame` does the same for a local `Main`.
- `purr_world_ended(w)`: whether the match is over: its last scene unloaded and `Main` is local, so it can't come back. The server stops ticking there and tells every player, who go offline with `PURR_DISCONNECT_ENDED` (`Ended` in PurrLang).
- `purr_local`: this machine's local state, outside every world. `purr_local_init(local)` clears it and sets its singletons' defaults.
- `purr_frame(w, previous, alpha, local, draw, gui)`: runs every view once, adding their Draw calls to a `purr_draw_list` and their widgets to a `purr_gui`, then applies the local changes they made. Views see the match's floats blended from `previous`, the tick before `w`, by `alpha` (0 to 1), so they're smooth at any tick rate; `purr_session_view` gives all three. Pass NULL and 1 to draw `w` as it is. Call it once per frame, after `purr_draw_reset(draw)` and `purr_gui_begin(gui, devices, purr_platform_screen_size(), purr_platform_measure_text)`, then `purr_gui_end(gui, draw)` and render the list with `purr_platform_draw(draw)`. Outside a match `w` is NULL, and views that read the match don't run. A zeroed `purr_gui` is ready to use.
- `purr_get_<Component>(w, entity)`: a component of an entity, or `NULL`. A local component's takes the `purr_local`.
- `purr_world_player_joined(w, player)` and `purr_world_player_left(w, player)`: send `PlayerJoined` and `PlayerLeft`, handled at the end of the next tick. Every machine calls them before the same tick.
- `purr_world_entity_count(w)` and `purr_world_print(w)`: for debugging.
- `purr_world_copy(to, from)` and `purr_world_hash(w)`: snapshots and state hashes between ticks, covering only what's in use.
- `purr_world_start(w, dt, start)`: `purr_world_init`, starting the match in the scene `start` names (`purr_start`, from `Session.Play` and `Session.Host`), or `Main` for NULL.
- `purr_game_api`: the game as sessions run it (`purr_game` in `purr/session.h`): the world's size and functions, and the input packed for the network.
- Local code's requests of the session (`Session.Play` and the like) wait in the local state: `purr_local_take_request(local, &request, &start)` takes them. Hosts tell local code where it stands with `purr_local_set_session(local, state, player, ping, server)`, `purr_local_connected(local)` and `purr_local_disconnected(local, reason)`.
- If the game has an input (it declares one, or systems take `Devices`), `PURR_HAS_INPUT` is defined and `purr_input` names its type. `purr_input_sample(devices, local)` runs the input's `Sample` on the client, with this machine's local state: give it a copy of the devices that `purr_gui_hide(gui, &copy)` took what the GUI is using out of, then call `purr_devices_consume(devices)` on the real ones. The input holds what match code reads of the devices (`purr_dev`), everything else zero. `purr_world_set_input(w, player, input)` sets a player's input for the next tick, and `purr_world_set_server_input(w, input)` the server's, which entities without an owner read. Both repair NaN and infinite floats, apply the fields' `[Clamp]`, `[Min]` and `[Max]`, then run the input's `Sanitize`, if it has one.

### Docs site

- `docs/` is the docs site, built with VitePress and published to GitHub Pages (https://blenminer.github.io/PurrEngine/) from `dev` by `.github/workflows/docs.yml`, which also builds it on pull requests. It needs Node: `npm ci`, then `npm run dev` in `docs/` serves it, and `npm run build` builds it and fails on dead links.
- The workflow builds the web demo (`web-release`, target `demo`) and puts it at `demo/`, where `docs/guide/demo.md` shows it. To see it locally, copy `build/web-release/bin/demo.html` to `docs/public/demo/index.html`.
- The docs' Markdown fences PurrLang as `csharp`, which GitHub highlights; the site highlights it with the editors' grammar (`tools/purrlang-syntax`).

## Packaging and releases

Users get PurrEngine as the `purr` command, not this repo: see README.md.

- `purr run [--host [port] | --join address]`, `purr build [--release] [--web]`, `purr schedule`, `purr editors`, `purr upgrade` and `purr version` (`compiler/cli/`, owned by Claude). It runs purrc's front end in-process, compiles the generated C and the engine's sources with the determinism flags (as separate files, like the CMake build), and links the prebuilt platform layer. `purr run` reloads the game as its files change (see Hot reloading).
- Packages have clang and lld built into purr (`PURR_EMBED_LLVM`, `compiler/cli/llvm/cc.cpp`), so users need no compiler: `purr cc` is clang, whose compiles run in purr's process and whose links go to lld in it. purr links the static libraries of LLVM's own release, downloaded once into `build/llvm-<version>` and pinned (`cmake/LLVM.cmake`, which also builds the zlib, zstd and libxml2 they were built with; unpacking needs the `zstd` program). macOS's release holds them as LLVM bitcode, so purr links there with that release's lld. Builds of this repo without the option use an installed clang (`--web` needs `wasm-ld` next to it).
- The package brings the C library for web games (wasi-libc) and for Windows games, which build for MinGW-w64 (see MinGW builds): Microsoft's C runtime can't ship with purr, and the UCRT is part of Windows, so players need nothing else. Linux games use the system's C development files, and macOS games the SDK of Apple's command-line tools (purr sets `SDKROOT`).
- A game is a folder of `.purr` files. purr keeps its work in `<folder>/.purr/` (hidden on Windows too, and it ignores itself in git) and puts `purr build` output in `<folder>/build/`.
- A game's C is in its folder too, outside hidden folders and `build/`: every `.c` file compiles with the game's flags (each again only when it, a header there or the flags change), and every library (`.a`, `.lib`, `.so`, `.dll`, `.dylib`) links when its contents say it was built for the target's platform and CPU (`compiler/cli/libraries.c`), since `.a` is every platform's. `.so`, `.dll` and `.dylib` files are copied next to the program, and `.so`/`.dylib` ones found there through its rpath; a `.dll` links through its import library. Web programs link with undefined functions allowed (the page provides GL's), so a web build fails when one of the game's extern functions is among its imports.
- The package is everything installed as the `purr` component: `bin/` (purr, purrls), `include/`, `src/engine/`, `lib/native/` and `lib/web/` (the prebuilt platform layer and raylib), `wasi/` (wasi-libc and the compiler runtime for wasm), `mingw/` (on Windows: MinGW-w64's headers, C runtime and compiler runtime), `lib/clang/` (clang's own headers), `web/` (the page and `purr.js`), `editors/` and `VERSION`. It comes from the `package` preset (clang and lld built in, static C runtime on Windows), the `web-package` preset and, on Windows, the `mingw-release` preset, put together by `cmake -DNAME=purr-windows-x64 -P cmake/package.cmake` into `build/dist`. Anything purr needs at build time must be installed into the package; a game build can't see this repo. The same goes for `purrls`, which reads the engine headers from the package's `include/`.
- `editors/purrlang.vsix` is the VS Code extension (`tools/purrlang-vscode`), for VS Code, Cursor, VSCodium and Windsurf. Only the `package` preset builds it (`PURR_EDITOR_EXTENSIONS`), since it needs Node's npm. The installers run `purr editors`, which installs it into every one of those editors it finds, and `purr upgrade` updates it in the editors that have it.
- The JetBrains plugin (`tools/purrlang-jetbrains`, Gradle and Java; [on Marketplace](https://plugins.jetbrains.com/plugin/34610-purrlang) as `io.github.blenminer.purrlang`) isn't in the package: CI builds it and checks it with the plugin verifier on every push, attaches it to each release, and publishes stable releases to JetBrains Marketplace with the `JETBRAINS_MARKETPLACE_TOKEN` secret. Marketplace installs LSP4IJ, which it needs, along with it.
- Users install with `install.ps1` or `install.sh` into `%LOCALAPPDATA%\Purr` or `~/.purr`, with `bin` on `PATH`, and `purr upgrade` replaces the installation from GitHub Releases. It checks each download against the release's `SHA256SUMS`, and renames running programs aside instead of overwriting them. Once a day, purr says when a newer version is out. purr and the installers pick a channel's highest version by semantic versioning (`compiler/cli/release.c`), never the last one published, and stable also asks GitHub for its latest release, which nightly ones push out of the list. `purr upgrade` only goes forward, except to an exact `--version` or when switching channels. Builds made from this repo are versioned `<VERSION>-dev` and never look for updates.
- `.github/workflows/build.yml` tests and packages Windows, Linux and macOS on every push and pull request (macOS doesn't hold back releases yet), and tests the MinGW target on Windows. Pushes to `dev` publish nightly pre-releases (`0.2.0-nightly.3`); pushes to `release` publish stable releases, and semantic-release commits the new `VERSION` there. The native demo smoke test is skipped in CI, which has no GPU.
- Versions come from conventional commits (`.releaserc.json`): `fix:` is a patch, `feat:` a minor version. While the version starts with 0, breaking changes (`feat!:`) are minor versions too, and we avoid them until 1.0 anyway.

### Hot reloading

It's for quick iteration, and only under `purr run`, native and web: `purr build` output has none of it.

- `purr run` builds the game as a library (`game-<n>.dll`, `.so` or `.dylib`) and a small host that loads it (`purr_host_run_library`, in `platform/src/reload.c`). purr watches the game's `.purr` files, and its C files, headers and libraries, and builds again once a change has settled. The game's C is part of its library, so the state C keeps starts over at each reload. Each build is linked under another name and renamed into place when it's whole; between two frames, the host swaps in the newest.
- A build with the same data layout keeps the match and the local state where they are (`purr_session_set_game`). The layout is `purr_game`'s hash: the generated header but for its first line.
- A build with another layout gets the match and the local state carried over by name (`purr/migrate.h`, `purr_session_migrate`). Keep as much state as possible, and when a big change breaks it, starting over is fine. No attribute for renamed fields, like Unity's `[FormerlySerializedAs]`, and no save files. The rules:
  - Fields of the same name keep their values. `int` to `float` and int vectors to float vectors of the same size convert, as PurrLang converts them implicitly, and enums keep their member by name. Other fields, and new ones, get their declared default; new text and lists start empty. A changed default doesn't touch existing values.
  - Entities keep their IDs. Without a removed component, an entity goes to the storage for the rest of its components. It's dropped when there's none or it's full.
  - A world whose scene is gone starts over.
  - The host says what didn't carry over: fields reset and entities dropped.
- purrc describes a game's layout for this only when asked (`--layout`, `codegen_options.layout`, `purr_add_game(... LAYOUT)`): `purr_game_layout` in the generated `.c`, not the header, so `purr_game`'s hash doesn't change. The migration is in the platform layer's static library (`platform/src/migrate.c`), so only programs that call it link it.
- In a match, the server's machine carries its world over, and its own player takes that world. A client of another machine carries its last verified world over, and the server sends the whole world again if it comes out different. Inputs sent for ticks to come are dropped: players' last inputs stand in until new ones arrive.
- A build that fails leaves the game running its last one. `r` and Enter in purr's terminal starts the game over (through a `restart` file in the run's folder). The host says what each reload did, so under `purr run` it's a console program even with `--release`.
- A game started with `--join` joins again a second after its match ends without it leaving: when the server starts over, or still runs an older layout.
- Each run has its own folder, `.purr/<configuration>/run/<purr's process ID>`, so a server and a client run from one folder never build over each other. It keeps the last two builds, and it's deleted when the run ends, or by a later run once its purr is gone.
- A game's library has its own copy of the engine code it uses. So everything the host and the library share is plain data: worlds, local state, devices, the draw list and the GUI. The library frees its scratch area before it's unloaded (`purr_host_game.unload`).
- On Linux, a library's objects are compiled with `-fPIC`. The engine's are kept apart in `engine-pic`, so running and building don't rebuild each other's.
- On the web, each build is a WebAssembly program of its own. `purr run --web` serves the page on 127.0.0.1 (`compiler/cli/serve.c`), opens it (unless `--no-open`), and keeps running until it's stopped. The editors run it with `--no-open` and show the page themselves, at the address purr prints (`purr: the game is at ...`): VS Code's **PurrLang: Run on the Web** in its browser, and the JetBrains plugin's PurrLang run configurations in the PurrLang Game tool window. The page asks purr for the newest build 4 times a second (`/build`: the build and restart numbers; `/game-<n>.wasm`).
- The page starts each new build in the running one's place (`platform/web/purr.js`, between its `purr run` markers): it asks the running one for its state (`purr_reload_save`), deletes its WebGL objects, and starts the new one, which carries that state over (`purr_host_run_web` in `platform/src/reload.c`). The state is the old layout, packed with offsets for pointers (`purr_layout_pack`), the server's world, the local state and the GUI. The new session goes on from the world with this machine's player in it already (`purr_session_play_from`), so there's no second `PlayerJoined`. Web games only play single-player for now, so there's nobody else to carry.
- Pages made to ship (`purr build --web`, `cmake/web_page.mjs`) leave out the lines between purr.js's `purr run` markers, and web programs only link reload code when purr run's `main.c` calls it.

## Layout

- `engine/`: the engine library (`purr`). Public headers go in `engine/include/purr/`, sources in `engine/src/`. New `.c` files are picked up automatically.
- `compiler/`: `purrc`, the PurrLang transpiler (owned by Claude). `compiler/tests/e2e/` holds programs compiled and run as tests. `compiler/tests/errors/` holds programs that must fail with the message on their first line. purrc reads files with Windows line endings (CRLF) exactly as LF ones: `e2e/crlf.purr` and `errors/*_crlf.purr` test it, and `.gitattributes` keeps them CRLF on every checkout.
- `compiler/cli/`: `purr`, the command users run (owned by Claude; see Packaging and releases). `compiler/cli/tests/` tests what it can without a network or a compiler: versions and releases, and which platform a library is for.
- `compiler/lsp/`: `purrls`, the PurrLang language server (owned by Claude). It reuses purrc's front end, with error recovery, to give editors completion, diagnostics, quick fixes, hovers, go to definition, find usages, rename, formatting, parameter hints, inlay hints, the outline, workspace symbols, folding, semantic highlighting, and moving a declaration to a file of its own. Native builds only.
- `tools/`: editor support. `purrlang-vscode` is the VS Code extension and `purrlang-jetbrains` the JetBrains plugin: each is the grammar, a client that runs `purrls`, and running the game with `purr run`, natively or on the web. `purrlang-syntax` is the TextMate grammar they both include, which the package also ships as a bundle for other editors. An editor feature goes to every integration that can have it, each in its own editor's way.
- `docs/`: the docs site (see Docs site). `docs/purrlang.md` is the language spec; `guide/`, `language/` and `engine/` are the pages for users, written from it and from this file.
- `platform/`: the platform layer (`purr_platform`): window, frame loop and input devices, on raylib. Public header `platform/include/purr/platform.h`.
- `demo/`: a small game on the platform layer. `demo.purr` is the simulation and the views that draw it, and `main.c` is the host. It builds as `demo.html` on the web.
- `sandbox/`: the owner's experiments: a game with no C, built by `purr_add_game`.
- `tests/`: tests built on the harness in `tests/purr_test.h`. New test files are picked up automatically.
- `.github/workflows/`, `.releaserc.json`, `install.ps1`, `install.sh`: releases and installing (see Packaging and releases), and the docs site.
- `cmake/`: shared compiler flags (`PurrFlags.cmake`), the package (`package.cmake`), the file that locates clang (`clang-toolchain.cmake`), the web and MinGW toolchains (`wasi-toolchain.cmake`, `mingw-toolchain.cmake`), purr's built-in clang (`LLVM.cmake`), `purr_add_game` (`PurrLang.cmake`), the raylib download (`Raylib.cmake`), and `purr_add_web_test` (`WebTest.cmake`), which runs a web page in headless Chrome or Edge as a test.

### Runtime written by Claude for now

- `engine/include/purr/entity.h` and `engine/src/entity.c` (the entity table) are a temporary implementation Claude wrote so generated code could run. The owner takes them over later. Until then Claude maintains them. Generated code depends on the functions declared in `entity.h`.
- `engine/include/purr/devices.h`, `engine/src/devices.c` (input devices) and `engine/include/purr/player.h` (`PlayerID`) are Claude's too, on the same terms. purrc reads the device member lists from `devices.h`, so PurrLang and C always agree. `purr_devices` has no padding the compiler adds: the input holds one when match code reads devices.
- `engine/include/purr/math.h` and `engine/src/math.c` (vectors, quaternions, matrices and transcendental functions) are Claude's too, on the same terms. Generated code calls them by the names `purr_<function>_<type>`. The transcendental functions are in-house, computed in double from basic operations; CORE-MATH remains an option to replace them.
- `engine/include/purr/color.h`, `engine/include/purr/draw.h` and `engine/src/draw.c` (colors and the draw list) are Claude's too, on the same terms. Generated views call the `purr_draw_*` functions.
- `engine/include/purr/heap.h`, `engine/src/heap.c` (a world's heap), `engine/include/purr/text.h`, `engine/src/text.c` (text, its formatting and the scratch area) and `engine/include/purr/list.h`, `engine/src/list.c` (lists) are Claude's too, on the same terms. Generated code calls the `purr_str_*`, `purr_text_*` and `purr_list_*` functions.
- `engine/include/purr/net.h`, `engine/src/net.c` (addresses, transports, the loopback network, packing bytes and bits, snapshots and hashes), `engine/include/purr/session.h`, `engine/src/session.c` (servers, clients and sessions: the netcode) and `platform/src/udp.c` (the UDP transport) are Claude's too, on the same terms, as a first version the owner takes over. Generated code calls the `purr_bits_*` functions and fills a `purr_game`.
- `engine/include/purr/gui.h` and `engine/src/gui.c` (the GUI's widgets, layout, focus and typing) are Claude's too, on the same terms. Generated views and the functions they call call the `purr_gui_*` functions, with IDs purrc derives from where each widget is called and the entity the view runs for. PurrLang's `Anchor` has `purr_anchor`'s values.
- `engine/include/purr/layout.h` (a game's data layout, which purrc describes for hot reloading) is Claude's too, on the same terms.
- `platform/` (the platform layer) and `demo/` are Claude's too, on the same terms.
  - The platform layer reads keys by physical position everywhere. On the web, the page reads the DOM's `code`, never the typed character, which follows the keyboard layout. `platform/tests/web_keys.c` guards this with AZERTY-style events. Hosts and views should read input from `Devices` too, never raylib's key functions.
  - The characters typed, which do follow the layout, are a separate channel (`purr_devices.text`, since the last poll) that only the GUI reads.
  - On the web, a key or mouse button pressed and released between two frames reads as held for one, so taps aren't lost when frames are slow.
  - Pixels (the window's size, the mouse, what's drawn) are the display's logical pixels, as a browser's CSS pixels are, and rendering is at the display's full resolution: raylib's `FLAG_WINDOW_HIGHDPI` on desktop, and on the web a canvas `devicePixelRatio` times its CSS size.
  - On the web, `purr_platform_run` never returns (the browser drives the frames), so hosts do all their work in the frame function.

## Language

The language is called PurrLang (working name). Its syntax and semantics are specified in `docs/purrlang.md`. The docs site explains it to users: a change to the language, `purr` or the editors updates the site's pages too (`docs/guide/`, `docs/language/`, `docs/engine/`).

- Derive as much as possible from the language.
- It should be as explicit as our needs require, while staying friendly.
- Dependency trees are static: known at compile time, not discovered at runtime.
- Explicit is the default. Systems declare which components they read and write in their signatures.
- A system's access (which components it reads or writes) is declared separately from its filters (which components an entity must have or lack). Filters don't create data dependencies.
- The language is built around the ECS. It should use syntax sugar to hide the ECS's pain points.
- When nothing else decides a convention (names, axes, units, orderings), follow Unity: Unity.Mathematics for math, the Input System for input. PurrLang's own rules, such as PascalCase methods, still win.
- Games call C through `extern` functions, and C is trusted: what it does (determinism, state outside the world, thread safety) is the game's responsibility. The language adds no markers or checks for it, and the compiler takes a C call as touching nothing it tracks. purrc declares each one in the generated `.c` from its signature, never in the header (which would change `purr_game`'s hash) and never by including the library's header. Calls to C, and to functions and methods that call it (`decl.calls_c`), are side effects that codegen keeps in source order, as it does spawns.
- Error messages guide the user to the fix: say what's wrong and, whenever it's knowable, what to write instead (a note with the right usage, or "did you mean ..." for a misspelled name).
- No backward compatibility yet: rename and change freely. Old spellings aren't supported; at most, an error points to the new one.

## ECS and simulation state

- The transpiler generates C specific to each game: component structs, archetype storage, per-system dispatch, and the tick. There is no runtime type registry.
- Singletons hold state for the whole world, such as time, RNG and game rules (other ECSs call them resources). There is one of each per world, stored inside it, and systems declare them the same way as components.
- The world is always passed as a pointer, never stored in a global. Several worlds can exist at once, for example predicted and verified copies, several matches on one server, or snapshots.
- The world owns all simulation memory. Components hold offsets into world memory, never pointers. The allocator's state lives in the world too, so a snapshot captures everything.
- Text and lists in fields live in the world's heap (`purr/heap.h`): a fixed size set when the game is built (`PURR_HEAP_BYTES`), and blocks handed out and reused in a fixed order, so every machine gets the same offsets and runs out at the same point. Freed memory is zeroed.
- Text and lists that code makes along the way live in a scratch area outside the world, cleared after each system, view and handler runs, and are only ever copied into a world.
- Local state (menus, settings, view state such as particles) belongs to one machine and lives outside every world. It's never sent, rolled back or hashed. The compiler keeps match code from reading it and local code from changing the match (see `docs/purrlang.md`).
- Generated types (components, singletons, the input and structs) have no padding the compiler adds: purrc writes it out as members, which every value sets to zero, and the generated header checks each type's size (`_Static_assert`). Their bytes only depend on their fields, so snapshots and state hashes can compare memory directly. Input from outside gets its padding cleared along with its other repairs.
- Past each count (an archetype's rows, the entity table's slots and free list, the heap's used bytes, the command queue), a world is zeros: removing a row clears the one it vacates, and a snapshot copy clears what the destination used beyond the source. So a world's bytes only depend on its state, and snapshots (`purr_world_copy`) and hashes (`purr_world_hash`) cover only what's in use: their cost follows the world's contents, not its capacity.

## Performance

- Performance is a top priority. Weigh design choices by their runtime cost.
- Flexible features are fine even if they cost more, as long as the common, fixed path stays fast.
- Friction between engine code and user code should be minimal or non-existent.
- Snapshotting and restoring the full simulation state must be cheap. Rollback depends on it.

## Networking

- Rollback netcode with full server authority. The server is the source of truth. Clients predict ahead and roll back when the server disagrees.
- Single-player is a match too: the machine runs the server itself and connects to it through a loopback transport, so there's no separate offline path. The engine assumes nothing about what games do with matches (no built-in pause): it gives them the tools.
- Clients have no input delay: they run ahead of the server by about half the round trip plus a small margin, so their inputs arrive in time, and only other players' inputs are guessed. The server says how early their inputs arrive, and clients adjust their lead by it. A client that sends none (a game without an input) ignores it and keeps the lead it joined with.
- Desktop matches use our own thin layer on UDP. The web will need another transport (WebSocket, WebTransport or WebRTC); the protocol above it stays the same.
- How it works now (`purr/session.h`): the server ticks the one true world and sends each player every tick: who joined or left, the inputs that changed, and a hash of the world after it. Inputs that didn't change or arrive keep the last one, on every machine. A client keeps a snapshot of every tick from the last one the server confirmed (the verified world) to the one it predicted. A tick that arrives as the client guessed only has its hash checked against its snapshot; one that went otherwise runs on the snapshot before it, and the ticks after it run again. A client whose hash differs gets the whole world again, packed. Joining players get it too, and a cookie that gets them their `PlayerID` back when they join again. Everything is resent until acknowledged; nothing waits on a reliable stream. A machine that runs the server and stops for a while (a browser tab in the background, a breakpoint) stops its server and its player with it: its session drops the time beyond what the server can run in one update, so the two never time each other out and nothing runs ahead to catch up. A client of another machine keeps to the real time.
- Sync relies on determinism as much as possible. The main thing sent over the network is inputs.
- Inputs come from clients, so they're attack points. The engine is forgiving with them and makes bad values hard to turn into broken math or a broken game: NaN and infinite floats in an input become the field's default before the game's `Sanitize` runs, and math functions avoid spreading NaN where they can (`Math.Clamp` always returns a value in range; `Math.Min` and `Math.Max` with one NaN return the other argument).
- State corrections are supported. Divergence is detected through state hashing.
- Clients can be denied specific state through a visibility system, for example other players' cards in a poker game.
- Visibility is dynamic. The server decides at runtime what each player can see, through an API.
- Each client tracks a **verified tick**. It is the latest tick for which any state the client lacked but should have has arrived, and any divergence has been corrected by the server. Ticks after it are predicted and may be wrong for a while.
- Game code can tell verified state from predicted state. For example, a player death visual can wait until the death is verified.
- Secrets such as RNG seeds are ordinary state behind visibility, for example an entity the client can't see. They need no special mechanism.
- Clients don't have to simulate the whole world. LOD borders and culling are resolved through state sync.
- Hosting (not built yet): PurrEngine will host games for people, and hosted matches will run as WebAssembly in a sandbox (a runtime like wasmtime), with a time budget per tick. Owning the language doesn't make hosted code safe by itself: games can call C, and even pure PurrLang can loop forever. In the sandbox, C only reaches the game's own memory and what the host gives it, so `extern` stays allowed. A hosted game's match code, and the C it calls, has to build for the web; a server build can leave out views and local code.

## Determinism

Given the same build and the same inputs, simulation results must be bit-identical on every supported platform.

### Floats

- Only the IEEE 754 basic operations (`+ - * /` and `sqrt`) can be trusted to give identical results across platforms. Build everything else from them.
- No FMA contraction. The compiler must never fuse `a*b + c` into a single instruction.
- No fast-math. No reassociation or reordering of float operations.
- Simulation code must not use the platform math library (`sin`, `cos`, `atan2`, `exp`, `pow`, and so on). Use the engine's own math library, which is built only from the basic operations.
- Vector math (vectors, matrices, quaternions) is written in-house: it only needs the basic operations, and third-party vector libraries often use approximate instructions or the platform math library.
- Transcendental functions may come from a third-party library, but only as source vendored into the engine and compiled with its flags. Prefer correctly rounded implementations, such as CORE-MATH: they return the same bits on every platform by definition.
- No approximate instructions such as `rsqrtps` or `rcpps`. Their results differ between Intel and AMD.
- Denormals stay enabled (no FTZ/DAZ) on every thread and every platform. WebAssembly can't flush denormals, so no other platform may either.
- No relaxed SIMD on the web: its fused multiply-add gives different results on different machines.
- No x87 floating point.
- NaN in simulation state is a bug. NaN bit patterns are not portable.

### Parallelism

- Running in parallel must never change results. Thread timing must not decide the order of anything observable. That includes system order, command playback, reductions, and entity ID allocation.

### Systems that touch the same data

- It is not an error for two systems to write the same component. It's normal.
- Those systems run one after the other, in a deterministic order.
- Tooling tells the user why they didn't run in parallel. Example: "`MoveSystem` runs after `GravitySystem`: both write `Position`."
- Two systems conflict when one writes a component or singleton the other reads or writes, unless they can never touch the same entity: the compiler proves that from the archetypes (`with Player` against `with Enemy`). Two systems that change the match's text or lists conflict too, as they share its heap. `Spawn`, `Add`, `Remove`, `Destroy` and `Send` never conflict, because they're recorded and applied at the end of the tick in order. Event handlers run then too, as each event's turn comes, so they aren't part of the tick's schedule. Input and `Time` are only read.
- Conflicting systems keep their order in the tick, and `[Before]`/`[After]` order systems too. A system starts as soon as everything it waits for is done; there are no barriers between stages. A system's stage is only how deep it is in that chain.
- A system splitting its entities across threads is the other kind of parallelism, and it doesn't change results either.
- The tick doesn't run on threads yet, but the plan already exists (`analyze_parallelism` in `compiler/src/parallel.c`): the language server shows each system's stage and waits above it and on hover, and `purrc --schedule` (or the `<game>_schedule` build target) prints it as text for CI and agents. `compiler/tests/schedule/` pins its output.
- Declared access that isn't used is a warning, since it makes other systems wait for nothing: a `mut` parameter that's never written, or a parameter that's never used (a component only needed as a filter belongs in `with`). The language server offers quick fixes, as it does for adding a missing `mut`.
