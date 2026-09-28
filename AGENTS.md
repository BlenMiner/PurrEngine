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

- raylib handles windows, input and rendering for now. The web build renders with WebGL 2.
- Rendering stays disconnected from the simulation, so it can be replaced later (for example for consoles):
  - The simulation never includes raylib or any platform header.
  - The view reads the world and draws it. It never writes simulation state.
  - The platform layer's only link to the simulation is filling `Devices`.
  - Game code and raylib never share a source file: generated headers name types after the game's components (`Transform`), and raylib defines many of the same names. Hosts include `purr/platform.h`, which doesn't include raylib, and `purr_platform` keeps raylib private.
- `purr/run.h` is the standard host: `purr_run(&(purr_run_desc){.title = "..."})` opens a window, ticks the game at a fixed rate with player 0's input from the devices, and draws its views. A game needs no other C.
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

There are two presets. `debug` has no optimization. `release` is optimized and keeps debug info for profiling.

Every native build also copies the language server to `build/tools/purrls`, the fixed path editors run (see `tools/purrlang-lsp4ij`). On Windows, a build replaces the server while an editor still runs the old one: the running file is renamed aside and deleted by a later build.

The first configure downloads raylib (see `cmake/Raylib.cmake`). Configure with `-DPURR_PLATFORM=OFF` to build without the platform layer and the demo, for example offline.

### Web builds

- The `web-debug` and `web-release` presets build everything as WebAssembly with Emscripten: `cmake --workflow --preset web-debug`. Tests run under Node through CTest. purrc runs as WebAssembly too, under Node, during the build.
- Emscripten is found through `$EMSDK`, then common emsdk locations such as `D:/Tools/emsdk` (see `cmake/emscripten-toolchain.cmake`).
- Web builds are single-threaded. Threads need a cross-origin isolated page, and running in parallel never changes results anyway.
- Before finishing a change, the native and web test suites must both pass.

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
- Every game's files are listed in `build/tools/games.txt`, one `<game>\t<path>` per line. A path ending in `/` is a folder: every `.purr` file in it and its subfolders, so editors see new files before the next build. `purrls` reads it to analyze a game's files together, in `purrc`'s order; a file in no game is analyzed alone.

The generated header is the API between the game and the host. Namespaced declarations have their namespace in their C name: `Combat.Health` is `Combat_Health`, read with `purr_get_Combat_Health`.

- `purr_world`: the whole simulation state as plain data. Copying it is a snapshot.
- `purr_world_init(w, dt)`: clears the world, sets `Time.dt` and singleton defaults, runs `Main`.
- `purr_world_tick(w)`: runs every system once, then applies structural changes.
- `purr_world_draw(w, draw)`: runs every view once, adding their Draw calls to a `purr_draw_list`. Call it once per frame, after `purr_draw_reset(draw)`, then render the list with `purr_platform_draw(draw)`.
- `purr_get_<Component>(w, entity)`: a component of an entity, or `NULL`.
- `purr_world_entity_count(w)` and `purr_world_print(w)`: for debugging.
- If the game declares an input, `PURR_HAS_INPUT` is defined and `purr_input` names its type. `purr_input_sample(devices)` runs the input's `Sample` on the client (call `purr_devices_consume(devices)` after it). `purr_world_set_input(w, player, input)` sets a player's input for the next tick, and `purr_world_set_server_input(w, input)` the server's, which entities without an owner read. Both repair NaN and infinite floats, apply the fields' `[Clamp]`, `[Min]` and `[Max]`, then run the input's `Sanitize`, if it has one.

## Packaging and releases

Users get PurrEngine as the `purr` command, not this repo: see README.md.

- `purr run`, `purr build [--release] [--web]`, `purr schedule`, `purr upgrade` and `purr version` (`compiler/cli/`, owned by Claude). It runs purrc's front end in-process, compiles the generated C and the engine's sources with the determinism flags (as separate files, like the CMake build), and links the prebuilt platform layer. It uses an installed clang, and an installed Emscripten for `--web`. On Windows, games link the C runtime statically, so players need no redistributable.
- A game is a folder of `.purr` files. purr keeps its work in `<folder>/.purr/` (it ignores itself in git) and puts `purr build` output in `<folder>/build/`.
- The package is everything installed as the `purr` component: `bin/` (purr, purrls), `include/`, `src/engine/`, `lib/native/` and `lib/web/` (the prebuilt platform layer and raylib), `web/shell.html`, `editors/` and `VERSION`. It comes from the `package` preset (static C runtime on Windows) and the `web-package` preset, put together by `cmake -DNAME=purr-windows-x64 -P cmake/package.cmake` into `build/dist`. Anything purr needs at build time must be installed into the package; a game build can't see this repo.
- Users install with `install.ps1` or `install.sh` into `%LOCALAPPDATA%\Purr` or `~/.purr`, with `bin` on `PATH`, and `purr upgrade` replaces the installation from GitHub Releases. It checks each download against the release's `SHA256SUMS`, and renames running programs aside instead of overwriting them. Once a day, purr says when a newer version is out. Builds made from this repo are versioned `<VERSION>-dev` and never look for updates.
- `.github/workflows/build.yml` tests and packages Windows and Linux on every push and pull request. Pushes to `dev` publish nightly pre-releases (`0.2.0-nightly.3`); pushes to `release` publish stable releases, and semantic-release commits the new `VERSION` there. The native demo smoke test is skipped in CI, which has no GPU.
- Versions come from conventional commits (`.releaserc.json`): `fix:` is a patch, `feat:` a minor version. While the version starts with 0, breaking changes (`feat!:`) are minor versions too, and we avoid them until 1.0 anyway.

## Layout

- `engine/`: the engine library (`purr`). Public headers go in `engine/include/purr/`, sources in `engine/src/`. New `.c` files are picked up automatically.
- `compiler/`: `purrc`, the PurrLang transpiler (owned by Claude). `compiler/tests/e2e/` holds programs compiled and run as tests. `compiler/tests/errors/` holds programs that must fail with the message on their first line.
- `compiler/cli/`: `purr`, the command users run (owned by Claude; see Packaging and releases).
- `compiler/lsp/`: `purrls`, the PurrLang language server (owned by Claude). It reuses purrc's front end, with error recovery, to give editors completion, diagnostics, hovers, go to definition, find usages, rename, formatting, parameter hints, the outline and semantic highlighting. Native builds only.
- `tools/`: editor support. `purrlang-syntax` is a TextMate bundle for highlighting, and `purrlang-lsp4ij` is a template that connects JetBrains IDEs to `purrls` through the LSP4IJ plugin (`installed/` is the package's copy, which runs `purrls` from `PATH`).
- `docs/purrlang.md`: the language spec.
- `platform/`: the platform layer (`purr_platform`): window, frame loop and input devices, on raylib. Public header `platform/include/purr/platform.h`.
- `demo/`: a small game on the platform layer. `demo.purr` is the simulation and the views that draw it, and `main.c` is the host. It builds as `demo.html` on the web.
- `sandbox/`: the owner's experiments: a game with no C, built by `purr_add_game`.
- `tests/`: tests built on the harness in `tests/purr_test.h`. New test files are picked up automatically.
- `.github/workflows/`, `.releaserc.json`, `install.ps1`, `install.sh`: releases and installing (see Packaging and releases).
- `cmake/`: shared compiler flags (`PurrFlags.cmake`), the package (`package.cmake`), the file that locates clang (`clang-toolchain.cmake`), `purr_add_game` (`PurrLang.cmake`), the raylib download (`Raylib.cmake`), and `purr_add_web_test` (`WebTest.cmake`), which runs a web page in headless Chrome or Edge as a test.

### Runtime written by Claude for now

- `engine/include/purr/entity.h` and `engine/src/entity.c` (the entity table) are a temporary implementation Claude wrote so generated code could run. The owner takes them over later. Until then Claude maintains them. Generated code depends on the functions declared in `entity.h`.
- `engine/include/purr/devices.h`, `engine/src/devices.c` (input devices) and `engine/include/purr/player.h` (`PlayerID`) are Claude's too, on the same terms. purrc reads the device member lists from `devices.h`, so PurrLang and C always agree.
- `engine/include/purr/math.h` and `engine/src/math.c` (vectors, quaternions, matrices and transcendental functions) are Claude's too, on the same terms. Generated code calls them by the names `purr_<function>_<type>`. The transcendental functions are in-house, computed in double from basic operations; CORE-MATH remains an option to replace them.
- `engine/include/purr/color.h`, `engine/include/purr/draw.h` and `engine/src/draw.c` (colors and the draw list) are Claude's too, on the same terms. Generated views call the `purr_draw_*` functions.
- `platform/` (the platform layer) and `demo/` are Claude's too, on the same terms.
  - The platform layer reads keys by physical position everywhere. On the web it reads the DOM's `code` itself: Emscripten's GLFW, which raylib uses there, reads the legacy `keyCode`, which follows the keyboard layout. `platform/tests/web_keys.c` guards this with AZERTY-style events. Hosts and views should read input from `Devices` too, never raylib's key functions.
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

## Performance

- Performance is a top priority. Weigh design choices by their runtime cost.
- Flexible features are fine even if they cost more, as long as the common, fixed path stays fast.
- Friction between engine code and user code should be minimal or non-existent.
- Snapshotting and restoring the full simulation state must be cheap. Rollback depends on it.

## Networking

- Rollback netcode with full server authority. The server is the source of truth. Clients predict ahead and roll back when the server disagrees.
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
- Two systems conflict when one writes a component or singleton the other reads or writes, unless they can never touch the same entity: the compiler proves that from the archetypes (`with Player` against `with Enemy`). `Spawn`, `Add`, `Remove` and `Destroy` never conflict, because they're recorded and applied at the end of the tick in order. Input and `Time` are only read.
- Conflicting systems keep their order in the tick, and `[Before]`/`[After]` order systems too. A system starts as soon as everything it waits for is done; there are no barriers between stages. A system's stage is only how deep it is in that chain.
- A system splitting its entities across threads is the other kind of parallelism, and it doesn't change results either.
- The tick doesn't run on threads yet, but the plan already exists (`analyze_parallelism` in `compiler/src/parallel.c`): the language server shows each system's stage and waits above it and on hover, and `purrc --schedule` (or the `<game>_schedule` build target) prints it as text for CI and agents. `compiler/tests/schedule/` pins its output.
- Declared access that isn't used is a warning, since it makes other systems wait for nothing: a `mut` parameter that's never written, or a parameter that's never used (a component only needed as a filter belongs in `with`). The language server offers quick fixes, as it does for adding a missing `mut`.
