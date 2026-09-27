# PurrEngine: Agent Guide

Rules and decisions for anyone, human or AI agent, working on PurrEngine. Only agreed decisions go in this file. Open questions stay out until they're decided.

## Working agreement

- The project owner is the lead programmer and writes most of the code.
- Agents help with specific, scoped tasks. Do what was asked. Don't widen scope, scaffold systems, or refactor nearby code unless asked.
- If it's unclear whether the owner wants code or discussion, ask.

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

Requires CMake 3.25+, Ninja, and clang. The build finds clang automatically, checking `$LLVM_ROOT/bin`, then `PATH`, then a standalone LLVM install, then Visual Studio's bundled clang.

- Configure, build and test in one step: `cmake --workflow --preset debug`
- Build only: `cmake --build --preset debug`
- Run tests: `ctest --preset debug`, or run `build/debug/bin/purr_tests [name-filter]` directly
- Run the sandbox: `build/debug/bin/sandbox`

There are two presets. `debug` has no optimization. `release` is optimized and keeps debug info for profiling.

## Layout

- `engine/`: the engine library (`purr`). Public headers go in `engine/include/purr/`, sources in `engine/src/`. New `.c` files are picked up automatically.
- `sandbox/`: an executable for experiments.
- `tests/`: tests built on the harness in `tests/purr_test.h`. New test files are picked up automatically.
- `cmake/`: shared compiler flags (`PurrFlags.cmake`) and the file that locates clang (`clang-toolchain.cmake`).

## Language

- Derive as much as possible from the language.
- It should be as explicit as our needs require, while staying friendly.
- Dependency trees are static: known at compile time, not discovered at runtime.
- Explicit is the default. Systems declare which components they read and write in their signatures.
- The language is built around the ECS. It should use syntax sugar to hide the ECS's pain points.

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
