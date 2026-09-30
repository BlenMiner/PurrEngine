# Calling C

A game can call C: a library it ships with, like stb or miniaudio, a platform SDK, or C of its own. `extern` declares a function written in C, with no body:

```csharp
extern float Lerp3(float a, float b, float c, float t);
```

It's called like any function, from systems, views, event handlers, methods and other functions, and the call costs what a call between C functions does:

```csharp
system Blend(mut Mix mix)
{
    mix.value = Lerp3(mix.a, mix.b, mix.c, mix.t);
}
```

## Names

The C function has the name the extern is written with. When C's name doesn't suit PurrLang, `[NativeName]` gives it, and the PurrLang name can follow PurrLang's style:

```csharp
[NativeName("stb_perlin_noise3")]
extern float Noise(float x, float y, float z, int xWrap, int yWrap, int zWrap);
```

A namespace doesn't change the C name: in `namespace Terrain;`, that's `Terrain.Noise` in PurrLang and still `stb_perlin_noise3` in C.

## Where the C goes

In the game's folder, with nothing to set up:

- Every `.c` file in the folder and its subfolders compiles with the game, with the same determinism flags as the engine. Headers next to them are found as C finds them.
- Every prebuilt library there links with the game when it was built for the platform being built for: `.a` and `.lib` files, and `.so`, `.dll` and `.dylib` ones. purr reads each library to tell which platform and CPU it's for, so one folder holds them all, named and placed however you like.
- Code for one platform only goes in `#ifdef`, as in any C.

```
MyGame/
  game.purr
  noise.c              // #define STB_PERLIN_IMPLEMENTATION, then #include "stb_perlin.h"
  stb_perlin.h
  steam/
    steam_api64.lib    // Windows: links in
    steam_api64.dll    // ...and goes next to the game
    libsteam_api.so    // Linux
    libsteam_api.dylib // macOS
  steam_web.c          // #ifdef __wasm__: stand-ins, as the web has no Steam
```

`.so`, `.dll` and `.dylib` files are copied next to the built game, where it finds them. On Windows, a `.dll` links through its import library, the `.lib` that comes with it.

Windows games build for MinGW, so a static library built with Microsoft's compiler may need Microsoft's C runtime and fail to link: rebuild it with clang or MinGW.

On the web, only C files and WebAssembly libraries define functions. When an extern function has no definition there, the web build fails and names it; give it a stand-in inside `#ifdef __wasm__`.

`purr run` builds again when a C file, header or library changes. The C is part of the game's library, which each build replaces, so whatever C keeps in its own variables starts over at each reload.

## Values across

Extern functions take and return plain data, by value:

| PurrLang | C |
| --- | --- |
| `int`, `float`, `bool` | `int32_t` (`int`), `float`, `bool` |
| `float3`, `int2`, `quaternion`, `float4x4`, ... | `purr_float3`, `purr_int2`, ... from `purr/math.h` |
| `Color`, `Rect`, `Entity`, `PlayerID` | `purr_color`, `purr_rect`, `purr_entity`, `purr_player_id` |
| an enum | `int32_t` |
| a struct or component | a struct with the same fields, in the same order |

C often takes pointers. PurrLang has none, and no pointer arithmetic: the parameter says how a value goes to C, and the call takes its address by itself. Every pointer is only good until C returns.

| Parameter | C gets |
| --- | --- |
| `mut T x` | `T *`: the caller's variable, which C can change |
| `in T x` | `const T *`: the caller's variable, read-only, with no copy (a copy for a value that's no variable's) |
| `List<T> xs` | `const T *`: the list's elements, NULL when it's empty. Pass `xs.Count` too |
| `mut List<T> xs` | `T *`: the elements, which C can change, but not how many there are |
| `string s` | `const char *`: UTF-8, ending in a zero |

An extern function can return `string` from C's `const char *`: the text is copied, so C can reuse its buffer, and NULL is empty text.

```csharp
extern float Average(List<float> values, int count);
extern string Describe(in Stats stats);

system Report(Scores scores, mut Summary summary)
{
    summary.average = Average(scores.values, scores.values.Count);
    summary.text = Describe(summary.stats);
}
```

```c
float Average(const float *values, int count);
const char *Describe(const Stats *stats);
```

```csharp
struct Hit
{
    float3 point;
    float distance;
}

extern bool RayCast(float3 from, float3 direction, mut Hit hit);
```

```c
#include <stdbool.h>
#include "purr/math.h"

typedef struct Hit
{
    purr_float3 point;
    float distance;
} Hit;

bool RayCast(purr_float3 from, purr_float3 direction, Hit *hit)
{
    // ...
    return false;
}
```

PurrLang's types have no padding the compiler adds, so a C struct with the same fields lines up with them. Structs that hold text or lists can't go to C, and neither can lists of text.

## Order

C can keep state, so calling it is a side effect, and PurrLang keeps its order: in `Pick(Roll(), Roll())`, the first `Roll` runs first on every platform, though C itself would let each compiler pick. The same goes for functions, methods and operators that call C, and for calls inside `&&`, `||` and `?:`, whose parts still only run when they would.

## What C is trusted with

The compiler doesn't look inside C. It takes each call as touching nothing it tracks, so C never makes systems wait for each other, and anything C does is the game's to get right:

- **Determinism.** Match code runs the same on every machine only if its C does too: no platform math library (`sinf`, `powf`), and nothing that depends on the machine. The game's own C files get the engine's flags, which keep float math exact; a prebuilt library's flags are whatever it was built with.
- **State.** Variables C keeps aren't in the world, so they aren't sent, rolled back or hashed. Keep what the match depends on in components and singletons.
- **Threads.** Once systems run in parallel, two that call the same C function can run at the same time.

purrc declares each extern function from its PurrLang signature. If it doesn't match the C function, the call goes wrong the way it would in C.
