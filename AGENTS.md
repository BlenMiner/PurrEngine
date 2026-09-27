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

There are two presets. `debug` has no optimization. `release` is optimized and keeps debug info for profiling.

### PurrLang programs

`purr_add_game(<target> SOURCE <file.purr> [NAME <name>])` runs `purrc` on the file and compiles the generated C into the target, which then includes `<name>.h`. The files regenerate whenever the `.purr` file or `purrc` changes.

The generated header is the API between the game and the host:

- `purr_world`: the whole simulation state as plain data. Copying it is a snapshot.
- `purr_world_init(w, dt)`: clears the world, sets `Time.dt` and singleton defaults, runs `Main`.
- `purr_world_tick(w)`: runs every system once, then applies structural changes.
- `purr_get_<Component>(w, entity)`: a component of an entity, or `NULL`.
- `purr_world_entity_count(w)` and `purr_world_print(w)`: for debugging.

## Layout

- `engine/`: the engine library (`purr`). Public headers go in `engine/include/purr/`, sources in `engine/src/`. New `.c` files are picked up automatically.
- `compiler/`: `purrc`, the PurrLang transpiler (owned by Claude). `compiler/tests/e2e/` holds programs compiled and run as tests. `compiler/tests/errors/` holds programs that must fail with the message on their first line.
- `docs/purrlang.md`: the language spec.
- `sandbox/`: an executable for experiments.
- `tests/`: tests built on the harness in `tests/purr_test.h`. New test files are picked up automatically.
- `cmake/`: shared compiler flags (`PurrFlags.cmake`), the file that locates clang (`clang-toolchain.cmake`), and `purr_add_game` (`PurrLang.cmake`).

### Runtime written by Claude for now

- `engine/include/purr/entity.h` and `engine/src/entity.c` (the entity table) are a temporary implementation Claude wrote so generated code could run. The owner takes them over later. Until then Claude maintains them. Generated code depends on the functions declared in `entity.h`.

## Language

The language is called PurrLang (working name). Its syntax and semantics are specified in `docs/purrlang.md`.

- Derive as much as possible from the language.
- It should be as explicit as our needs require, while staying friendly.
- Dependency trees are static: known at compile time, not discovered at runtime.
- Explicit is the default. Systems declare which components they read and write in their signatures.
- A system's access (which components it reads or writes) is declared separately from its filters (which components an entity must have or lack). Filters don't create data dependencies.
- The language is built around the ECS. It should use syntax sugar to hide the ECS's pain points.

## ECS and simulation state

- The transpiler generates C specific to each game: component structs, archetype storage, per-system dispatch, and the tick. There is no runtime type registry.
- Resources hold state for the whole world, such as time, RNG and game rules. They live inside the world, and systems declare them the same way as components.
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
- Denormal handling (FTZ/DAZ) is set per thread. Every thread that runs simulation code must set it the same way.
- No x87 floating point.
- NaN in simulation state is a bug. NaN bit patterns are not portable.

### Parallelism

- Running in parallel must never change results. Thread timing must not decide the order of anything observable. That includes system order, command playback, reductions, and entity ID allocation.

### Systems that touch the same data

- It is not an error for two systems to write the same component. It's normal.
- Those systems run one after the other, in a deterministic order.
- Tooling tells the user why they didn't run in parallel. Example: "`MoveSystem` runs after `GravitySystem`: both write `Position`."
