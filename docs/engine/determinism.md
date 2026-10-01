# Determinism

Given the same build and the same inputs, Tide's simulation gives bit-identical results on every supported platform: Windows, Linux, macOS and the web, on Intel, AMD and ARM. That's what lets players send only their inputs, and lets desktop and web players share one match.

It uses ordinary 32-bit floats, not fixed point. Tide code gets this for free: there's nothing to avoid, and no special types to use. This page is about how the engine keeps it, for the curious, and for anyone writing C that touches the simulation.

## Floats

Only the IEEE 754 basic operations, `+ - * /` and square root, give the same result on every machine. Everything else is built from them:

- **No fused multiply-add.** The compiler never fuses `a * b + c` into one instruction, which rounds differently.
- **No fast-math.** Float operations are never reordered or reassociated.
- **The engine's own math library.** Vectors, matrices and quaternions are written in-house. `Sin`, `Exp`, `Pow` and the other transcendental functions are computed from the basic operations, never with the platform's math library, which differs between systems.
- **No approximate instructions**, like `rsqrtps`, whose results differ between Intel and AMD.
- **Denormals stay on**, on every thread and platform, since WebAssembly can't turn them off.
- **No relaxed SIMD on the web**, whose fused multiply-add differs between machines, and no x87.

The engine compiles everything, your game included, with the flags that guarantee this.

## NaN

NaN in the simulation is a bug: NaN's bit patterns aren't the same everywhere. The engine keeps it out where it can:

- Inputs come from other machines, so NaN and infinite floats in them become the field's default before the game sees them (see [Input](../language/input.md)).
- `Math.Clamp` always returns a value in range, `Math.Min` and `Math.Max` with one NaN return the other argument, and `Math.NormalizeSafe` gives zero instead of NaN.

## Order

Everything that could run in a different order does so in a fixed one:

- Expressions run left to right, as in C#, so spawns and entity IDs come out the same everywhere.
- Systems run in one order (see [Systems](../language/systems.md#order)), and structural changes and events apply in the order they were made.
- Running systems in parallel will never change results: thread timing never decides the order of anything that can be seen.

## Integers

Integer arithmetic wraps on overflow, and dividing by zero gives 0, so no input can crash the simulation or reach undefined behavior. Float to int conversion saturates and turns NaN into 0.

## How it's tested

The engine's tests hash the exact bits of math results and of whole simulations, and the hashes must match on every platform and in every build configuration: native and WebAssembly, debug and release. The demo's smoke test plays a scripted session through the real platform layer and checks its hash too.

## Writing C

Code in C that changes the simulation, like a custom host, follows the same rules: use the engine's `tide/math.h`, never `<math.h>`, and don't depend on the order C evaluates function arguments in, which differs between platforms. Draw inputs into locals first.
