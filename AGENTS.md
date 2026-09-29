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
- `purr/run.h` is the standard host: `purr_run(&(purr_run_desc){.title = "..."})` opens a window, ticks the game at a fixed rate with player 0's input from the devices, and draws its views. Player 0 joins before the first tick. A game needs no other C.
- Views are written in PurrLang (`view` declarations). Their `Draw` calls record commands into a renderer-agnostic draw list (`purr/draw.h`). The platform layer renders the list (`purr_platform_draw`), so replacing raylib only means rewriting that function.

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
- Run the demo: `build/debug/bin/demo`, or open `build/web-release/bin/demo.html` in a browser after a web build.

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

`purr_add_game(<target> [SOURCES <file.purr>...] [HOST <file.c>...] [NAME <name>] [TITLE <title>] [STATS])` builds a game as the program `<target>` (see `cmake/PurrLang.cmake`):

- The game is every `.purr` file in the current source folder and its subfolders; the next build picks up new files. `SOURCES` lists the files instead, for tests and folders that hold several games.
- Without `HOST`, the game is the whole program and needs no C: a generated `main` runs it in a window through `purr/run.h`. On the web it's `<target>.html`.
- With `HOST`, those C files are the program (tests, the demo's smoke test, custom hosts). They include `<name>.h`, where `NAME` defaults to `<target>`.
- `purrc` compiles all the files together, in order of their paths. The generated files regenerate whenever a `.purr` file or `purrc` changes.
- `<target>_schedule` is a build target that prints the game's schedule: which systems can run at the same time, and why the others wait.
- Every game's files are listed in `build/tools/games.txt`, one `<game>\t<path>` per line. A path ending in `/` is a folder: every `.purr` file in it and its subfolders, so editors see new files before the next build. `purrls` reads it, and the one in `build/tools/` of any folder open in the editor, to analyze a game's files together, in `purrc`'s order. A file in no game belongs to the open folder it's in, as `purr run` would build that folder, unless a manifest lists games in that folder: then it's analyzed alone.

The generated header is the API between the game and the host. Namespaced declarations have their namespace in their C name: `Combat.Health` is `Combat_Health`, read with `purr_get_Combat_Health`.

- `purr_world`: the whole simulation state as plain data. Copying it is a snapshot.
- `purr_world_init(w, dt)`: clears the world, sets `Time.dt` and singleton defaults, and loads the `Main` scene if it's the match's.
- `PURR_MAIN_IS_LOCAL` is defined when `Main` is a local scene: the program starts outside any match.
- `purr_world_tick(w)`: runs every system once, then applies structural changes.
- `purr_local`: this machine's local state, outside every world. `purr_local_init(local)` clears it and sets its singletons' defaults.
- `purr_frame(w, local, draw)`: runs every view once, adding their Draw calls to a `purr_draw_list`, then applies the local changes they made. Call it once per frame, after `purr_draw_reset(draw)`, then render the list with `purr_platform_draw(draw)`. Outside a match `w` is NULL, and views that read the match don't run.
- `purr_get_<Component>(w, entity)`: a component of an entity, or `NULL`. A local component's takes the `purr_local`.
- `purr_world_player_joined(w, player)` and `purr_world_player_left(w, player)`: send `PlayerJoined` and `PlayerLeft`, handled at the end of the next tick. Every machine calls them before the same tick.
- `purr_world_entity_count(w)` and `purr_world_print(w)`: for debugging.
- If the game declares an input, `PURR_HAS_INPUT` is defined and `purr_input` names its type. `purr_input_sample(devices)` runs the input's `Sample` on the client (call `purr_devices_consume(devices)` after it). `purr_world_set_input(w, player, input)` sets a player's input for the next tick, and `purr_world_set_server_input(w, input)` the server's, which entities without an owner read. Both repair NaN and infinite floats, apply the fields' `[Clamp]`, `[Min]` and `[Max]`, then run the input's `Sanitize`, if it has one.

## Packaging and releases

Users get PurrEngine as the `purr` command, not this repo: see README.md.

- `purr run`, `purr build [--release] [--web]`, `purr schedule`, `purr editors`, `purr upgrade` and `purr version` (`compiler/cli/`, owned by Claude). It runs purrc's front end in-process, compiles the generated C and the engine's sources with the determinism flags (as separate files, like the CMake build), and links the prebuilt platform layer.
- Packages have clang and lld built into purr (`PURR_EMBED_LLVM`, `compiler/cli/llvm/cc.cpp`), so users need no compiler: `purr cc` is clang, whose compiles run in purr's process and whose links go to lld in it. purr links the static libraries of LLVM's own release, downloaded once into `build/llvm-<version>` and pinned (`cmake/LLVM.cmake`, which also builds the zlib, zstd and libxml2 they were built with; unpacking needs the `zstd` program). macOS's release holds them as LLVM bitcode, so purr links there with that release's lld. Builds of this repo without the option use an installed clang (`--web` needs `wasm-ld` next to it).
- The package brings the C library for web games (wasi-libc) and for Windows games, which build for MinGW-w64 (see MinGW builds): Microsoft's C runtime can't ship with purr, and the UCRT is part of Windows, so players need nothing else. Linux games use the system's C development files, and macOS games the SDK of Apple's command-line tools (purr sets `SDKROOT`).
- A game is a folder of `.purr` files. purr keeps its work in `<folder>/.purr/` (hidden on Windows too, and it ignores itself in git) and puts `purr build` output in `<folder>/build/`.
- The package is everything installed as the `purr` component: `bin/` (purr, purrls), `include/`, `src/engine/`, `lib/native/` and `lib/web/` (the prebuilt platform layer and raylib), `wasi/` (wasi-libc and the compiler runtime for wasm), `mingw/` (on Windows: MinGW-w64's headers, C runtime and compiler runtime), `lib/clang/` (clang's own headers), `web/` (the page and `purr.js`), `editors/` and `VERSION`. It comes from the `package` preset (clang and lld built in, static C runtime on Windows), the `web-package` preset and, on Windows, the `mingw-release` preset, put together by `cmake -DNAME=purr-windows-x64 -P cmake/package.cmake` into `build/dist`. Anything purr needs at build time must be installed into the package; a game build can't see this repo. The same goes for `purrls`, which reads the engine headers from the package's `include/`.
- `editors/purrlang.vsix` is the VS Code extension (`tools/purrlang-vscode`), for VS Code, Cursor, VSCodium and Windsurf. Only the `package` preset builds it (`PURR_EDITOR_EXTENSIONS`), since it needs Node's npm. The installers run `purr editors`, which installs it into every one of those editors it finds, and `purr upgrade` updates it in the editors that have it.
- The JetBrains plugin (`tools/purrlang-jetbrains`, Gradle and Java; [on Marketplace](https://plugins.jetbrains.com/plugin/34610-purrlang) as `io.github.blenminer.purrlang`) isn't in the package: CI builds it and checks it with the plugin verifier on every push, attaches it to each release, and publishes stable releases to JetBrains Marketplace with the `JETBRAINS_MARKETPLACE_TOKEN` secret. Marketplace installs LSP4IJ, which it needs, along with it.
- Users install with `install.ps1` or `install.sh` into `%LOCALAPPDATA%\Purr` or `~/.purr`, with `bin` on `PATH`, and `purr upgrade` replaces the installation from GitHub Releases. It checks each download against the release's `SHA256SUMS`, and renames running programs aside instead of overwriting them. Once a day, purr says when a newer version is out. Builds made from this repo are versioned `<VERSION>-dev` and never look for updates.
- `.github/workflows/build.yml` tests and packages Windows, Linux and macOS on every push and pull request (macOS doesn't hold back releases yet), and tests the MinGW target on Windows. Pushes to `dev` publish nightly pre-releases (`0.2.0-nightly.3`); pushes to `release` publish stable releases, and semantic-release commits the new `VERSION` there. The native demo smoke test is skipped in CI, which has no GPU.
- Versions come from conventional commits (`.releaserc.json`): `fix:` is a patch, `feat:` a minor version. While the version starts with 0, breaking changes (`feat!:`) are minor versions too, and we avoid them until 1.0 anyway.

## Layout

- `engine/`: the engine library (`purr`). Public headers go in `engine/include/purr/`, sources in `engine/src/`. New `.c` files are picked up automatically.
- `compiler/`: `purrc`, the PurrLang transpiler (owned by Claude). `compiler/tests/e2e/` holds programs compiled and run as tests. `compiler/tests/errors/` holds programs that must fail with the message on their first line.
- `compiler/cli/`: `purr`, the command users run (owned by Claude; see Packaging and releases).
- `compiler/lsp/`: `purrls`, the PurrLang language server (owned by Claude). It reuses purrc's front end, with error recovery, to give editors completion, diagnostics, quick fixes, hovers, go to definition, find usages, rename, formatting, parameter hints, inlay hints, the outline, workspace symbols, folding, semantic highlighting, and moving a declaration to a file of its own. Native builds only.
- `tools/`: editor support. `purrlang-vscode` is the VS Code extension and `purrlang-jetbrains` the JetBrains plugin: each is the grammar and a client that runs `purrls`. `purrlang-syntax` is the TextMate grammar they both include, which the package also ships as a bundle for other editors.
- `docs/purrlang.md`: the language spec.
- `platform/`: the platform layer (`purr_platform`): window, frame loop and input devices, on raylib. Public header `platform/include/purr/platform.h`.
- `demo/`: a small game on the platform layer. `demo.purr` is the simulation and the views that draw it, and `main.c` is the host. It builds as `demo.html` on the web.
- `sandbox/`: the owner's experiments: a game with no C, built by `purr_add_game`.
- `tests/`: tests built on the harness in `tests/purr_test.h`. New test files are picked up automatically.
- `.github/workflows/`, `.releaserc.json`, `install.ps1`, `install.sh`: releases and installing (see Packaging and releases).
- `cmake/`: shared compiler flags (`PurrFlags.cmake`), the package (`package.cmake`), the file that locates clang (`clang-toolchain.cmake`), the web and MinGW toolchains (`wasi-toolchain.cmake`, `mingw-toolchain.cmake`), purr's built-in clang (`LLVM.cmake`), `purr_add_game` (`PurrLang.cmake`), the raylib download (`Raylib.cmake`), and `purr_add_web_test` (`WebTest.cmake`), which runs a web page in headless Chrome or Edge as a test.

### Runtime written by Claude for now

- `engine/include/purr/entity.h` and `engine/src/entity.c` (the entity table) are a temporary implementation Claude wrote so generated code could run. The owner takes them over later. Until then Claude maintains them. Generated code depends on the functions declared in `entity.h`.
- `engine/include/purr/devices.h`, `engine/src/devices.c` (input devices) and `engine/include/purr/player.h` (`PlayerID`) are Claude's too, on the same terms. purrc reads the device member lists from `devices.h`, so PurrLang and C always agree.
- `engine/include/purr/math.h` and `engine/src/math.c` (vectors, quaternions, matrices and transcendental functions) are Claude's too, on the same terms. Generated code calls them by the names `purr_<function>_<type>`. The transcendental functions are in-house, computed in double from basic operations; CORE-MATH remains an option to replace them.
- `engine/include/purr/color.h`, `engine/include/purr/draw.h` and `engine/src/draw.c` (colors and the draw list) are Claude's too, on the same terms. Generated views call the `purr_draw_*` functions.
- `platform/` (the platform layer) and `demo/` are Claude's too, on the same terms.
  - The platform layer reads keys by physical position everywhere. On the web, the page reads the DOM's `code`, never the typed character, which follows the keyboard layout. `platform/tests/web_keys.c` guards this with AZERTY-style events. Hosts and views should read input from `Devices` too, never raylib's key functions.
  - On the web, `purr_platform_run` never returns (the browser drives the frames), so hosts do all their work in the frame function.

## Language

The language is called PurrLang (working name). Its syntax and semantics are specified in `docs/purrlang.md`.

- Derive as much as possible from the language.
- It should be as explicit as our needs require, while staying friendly.
- Dependency trees are static: known at compile time, not discovered at runtime.
- Explicit is the default. Systems declare which components they read and write in their signatures.
- A system's access (which components it reads or writes) is declared separately from its filters (which components an entity must have or lack). Filters don't create data dependencies.
- The language is built around the ECS. It should use syntax sugar to hide the ECS's pain points.
- When nothing else decides a convention (names, axes, units, orderings), follow Unity: Unity.Mathematics for math, the Input System for input. PurrLang's own rules, such as PascalCase methods, still win.
- Error messages guide the user to the fix: say what's wrong and, whenever it's knowable, what to write instead (a note with the right usage, or "did you mean ..." for a misspelled name).
- No backward compatibility yet: rename and change freely. Old spellings aren't supported; at most, an error points to the new one.

## ECS and simulation state

- The transpiler generates C specific to each game: component structs, archetype storage, per-system dispatch, and the tick. There is no runtime type registry.
- Singletons hold state for the whole world, such as time, RNG and game rules (other ECSs call them resources). There is one of each per world, stored inside it, and systems declare them the same way as components.
- The world is always passed as a pointer, never stored in a global. Several worlds can exist at once, for example predicted and verified copies, several matches on one server, or snapshots.
- The world owns all simulation memory. Components hold offsets into world memory, never pointers. The allocator's state lives in the world too, so a snapshot captures everything.
- Local state (menus, settings, view state such as particles) belongs to one machine and lives outside every world. It's never sent, rolled back or hashed. The compiler keeps match code from reading it and local code from changing the match (see `docs/purrlang.md`).
- Generated types (components, singletons, the input and structs) have no padding the compiler adds: purrc writes it out as members, which every value sets to zero, and the generated header checks each type's size (`_Static_assert`). Their bytes only depend on their fields, so snapshots and state hashes can compare memory directly. Input from outside gets its padding cleared along with its other repairs.

## Performance

- Performance is a top priority. Weigh design choices by their runtime cost.
- Flexible features are fine even if they cost more, as long as the common, fixed path stays fast.
- Friction between engine code and user code should be minimal or non-existent.
- Snapshotting and restoring the full simulation state must be cheap. Rollback depends on it.

## Networking

- Rollback netcode with full server authority. The server is the source of truth. Clients predict ahead and roll back when the server disagrees.
- Single-player is a match too: the machine runs the server itself and connects to it through a loopback transport, so there's no separate offline path.
- Sync relies on determinism as much as possible. The main thing sent over the network is inputs.
- Inputs come from clients, so they're attack points. The engine is forgiving with them and makes bad values hard to turn into broken math or a broken game: NaN and infinite floats in an input become the field's default before the game's `Sanitize` runs, and math functions avoid spreading NaN where they can (`Math.Clamp` always returns a value in range; `Math.Min` and `Math.Max` with one NaN return the other argument).
- State corrections are supported. Divergence is detected through state hashing.
- Clients can be denied specific state through a visibility system, for example other players' cards in a poker game.
- Visibility is dynamic. The server decides at runtime what each player can see, through an API.
- Each client tracks a **verified tick**. It is the latest tick for which any state the client lacked but should have has arrived, and any divergence has been corrected by the server. Ticks after it are predicted and may be wrong for a while.
- Game code can tell verified state from predicted state. For example, a player death visual can wait until the death is verified.
- Secrets such as RNG seeds are ordinary state behind visibility, for example an entity the client can't see. They need no special mechanism.
- Clients don't have to simulate the whole world. LOD borders and culling are resolved through state sync.

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
- Two systems conflict when one writes a component or singleton the other reads or writes, unless they can never touch the same entity: the compiler proves that from the archetypes (`with Player` against `with Enemy`). `Spawn`, `Add`, `Remove`, `Destroy` and `Send` never conflict, because they're recorded and applied at the end of the tick in order. Event handlers run then too, as each event's turn comes, so they aren't part of the tick's schedule. Input and `Time` are only read.
- Conflicting systems keep their order in the tick, and `[Before]`/`[After]` order systems too. A system starts as soon as everything it waits for is done; there are no barriers between stages. A system's stage is only how deep it is in that chain.
- A system splitting its entities across threads is the other kind of parallelism, and it doesn't change results either.
- The tick doesn't run on threads yet, but the plan already exists (`analyze_parallelism` in `compiler/src/parallel.c`): the language server shows each system's stage and waits above it and on hover, and `purrc --schedule` (or the `<game>_schedule` build target) prints it as text for CI and agents. `compiler/tests/schedule/` pins its output.
- Declared access that isn't used is a warning, since it makes other systems wait for nothing: a `mut` parameter that's never written, or a parameter that's never used (a component only needed as a filter belongs in `with`). The language server offers quick fixes, as it does for adding a missing `mut`.
