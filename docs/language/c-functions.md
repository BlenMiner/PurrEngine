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

The C function has the name the extern is written with. When C's name doesn't suit Tide, `[NativeName]` gives it, and the Tide name can follow Tide's style:

```csharp
[NativeName("stb_perlin_noise3")]
extern float Noise(float x, float y, float z, int xWrap, int yWrap, int zWrap);
```

A namespace doesn't change the C name: in `namespace Terrain;`, that's `Terrain.Noise` in Tide and still `stb_perlin_noise3` in C.

## Where the C goes

In the game's folder, with nothing to set up:

- Every `.c` file in the folder and its subfolders compiles with the game, with the same determinism flags as the engine. Headers next to them are found as C finds them.
- Every `.cpp` file does too, as C++ with no runtime behind it: see [C++](#c).
- Every prebuilt library there links with the game when it was built for the platform being built for: `.a` and `.lib` files, and `.so`, `.dll` and `.dylib` ones. tide reads each library to tell which platform and CPU it's for, so one folder holds them all, named and placed however you like (for phones, see [Android](#android)).
- Code for one platform only goes in `#ifdef`, as in any C.

```
MyGame/
  game.tide
  noise.c              // #define STB_PERLIN_IMPLEMENTATION, then #include "stb_perlin.h"
  stb_perlin.h
  steam/
    steam_api64.lib    // Windows: links in
    steam_api64.dll    // ...and goes next to the game
    libsteam_api.so    // Linux
    libsteam_api.dylib // macOS
  steam_web.c          // #ifdef __wasm__: stand-ins, as the web has no Steam
  physics/
    linux/libjoltc.so  // Linux
    arm64/libjoltc.so  // Android, on phones: goes in the app
    x86_64/libjoltc.so // Android, on its emulator
```

`.so`, `.dll` and `.dylib` files are copied next to the built game, where it finds them. On Windows, a `.dll` links through its import library, the `.lib` that comes with it.

Windows games build for MinGW, so a static library built with Microsoft's compiler may need Microsoft's C runtime and fail to link: rebuild it with clang or MinGW.

On the web, only C files and WebAssembly libraries define functions. When a function has no definition there, an extern function or one the game's C calls, the web build fails and names it; give it a stand-in inside `#ifdef __wasm__`. Web games build for `wasm32-wasip1-threads`, which shares memory between threads, so a WebAssembly library needs building for that target too (clang's `--target=wasm32-wasip1-threads`), or at least with `-matomics -mbulk-memory`. tide's own clang can build it, as `tide cc`, with the C library tide brings: `tide cc --target=wasm32-wasip1-threads --sysroot=<tide>/wasi/sysroot`, where `<tide>` is the folder `tide version` says it's installed in.

`tide run` builds again when a C or C++ file, header or library changes. The C is part of the game's library, which each build replaces, so whatever C keeps in its own variables starts over at each reload.

## C++

`.cpp` files in the game's folder compile with it as `.c` files do (`.cc` and `.cxx` too), so a library written in C++ can go in as source. Tide still calls C: what the game calls is `extern "C"`.

```cpp
// counter.cpp
struct Counter
{
    int count = 0;
    int Next() { return ++count; }
};

static Counter counter;

extern "C" int NextId(void)
{
    return counter.Next();
}
```

```csharp
extern int NextId();
```

Tide brings no C++ runtime, so this is C++ the language, with nothing behind it:

- **No standard library.** `<vector>`, `<string>` and the rest aren't there, on any platform. C's headers are, under their C names: `<stdint.h>`, not `<cstdint>`.
- **No exceptions and no RTTI**: no `throw` and `try`, no `typeid` and `dynamic_cast`.
- **`new` and `delete` are the code's to define.** A runtime is what defines them, so code that uses them does it itself, over `malloc` and `free`. A class with a virtual destructor uses `delete`, and one with a pure virtual function needs `__cxa_pure_virtual`:

  ```cpp
  #include <stddef.h>
  #include <stdlib.h>

  void *operator new(size_t size) { return malloc(size); }
  void *operator new[](size_t size) { return malloc(size); }
  void operator delete(void *p) noexcept { free(p); }
  void operator delete[](void *p) noexcept { free(p); }
  void operator delete(void *p, size_t) noexcept { free(p); }
  void operator delete[](void *p, size_t) noexcept { free(p); }
  extern "C" void __cxa_pure_virtual() { abort(); }
  ```

  Placement new, which `<new>` would declare, is one line in a header of the code's own: `inline void *operator new(size_t, void *place) noexcept { return place; }`.

The rest of the language is there, as C++20: classes, templates, lambdas, virtual functions. A global's constructor runs before the game starts, and again at each reload under `tide run`, like everything C keeps. A function's `static` is made the first time through, without the lock a runtime would take: two systems on different threads getting there at once is the game's to get right, like the rest of what C does on threads.

When a build fails for want of the runtime, tide says so after the compiler's error, and what to write instead.

Libraries written to need no runtime, as Dear ImGui is, go in as source. One that needs it comes prebuilt instead: a `.dll`, `.so` or `.dylib` behind a C API has its runtime inside. A static library (`.a`, `.lib`) has to be built to need none, as the game's own C++ is (`-fno-exceptions -fno-rtti -fno-threadsafe-statics`, and nothing of the standard library), and the web only has static ones, so a library that needs the runtime can't be used there.

### Android

On Android, the game's `.so` libraries go in the app with it:

- An app is built for two CPUs, arm64 (phones and tablets) and x86-64 (Android's emulator), so a library comes as a file for each. Where one only exists for phones, give its functions stand-ins for the emulator, inside `#if defined(__ANDROID__) && defined(__x86_64__)`.
- Android's libraries and Linux's are the same kind of file. tide tells them apart by the note Android's NDK puts in every shared library it links, which says the Android it was built for, so both can be in the game's folder.
- In the app, a library goes by the name it was linked with (its soname), or by its file's name when it was linked with none. Android wants that name to be `lib<name>.so`.
- A static library (`.a`) can't say it's Android's: the same C compiles to the same thing for Linux and Android. tide takes it for Linux's, so for Android, build the library as a shared one.
- A library written in C++ needs C++'s own library with it: link it in when building yours (`-static-libstdc++`), or put the NDK's `libc++_shared.so` in the game's folder too.
- Newer phones have memory pages of 16 KiB, and a library linked for 4 KiB ones may not load there. `tide` warns about such a library; Google Play requires apps to run on those phones. The NDK links for 16 KiB from r28 on, and older ones with `-Wl,-z,max-page-size=16384`.

## Values across

Extern functions take and return plain data, by value:

| Tide | C |
| --- | --- |
| `int`, `float`, `bool` | `int32_t` (`int`), `float`, `bool` |
| `float3`, `int2`, `quaternion`, `float4x4`, ... | `tide_float3`, `tide_int2`, ... from `tide/math.h` |
| `Color`, `Rect`, `Entity`, `PlayerID` | `tide_color`, `tide_rect`, `tide_entity`, `tide_player_id` |
| an enum | `int32_t`, or `uint8_t` and `uint16_t` for `: byte` and `: ushort` |
| a struct or component | a struct with the same fields, in the same order |

A system that splits its entities across threads gives the entities it spawns temporary handles until it's done (see [The schedule](../engine/schedule.md#splitting-across-threads)), so an `Entity` C gets from one may be temporary: `tide_entity_is_temporary(e)`, from `tide/entity.h`, tells. Tide gives the real handle to what the system kept, but not to C, so C that keeps entities should keep real ones.

C often takes pointers. Tide has none, and no pointer arithmetic: the parameter says how a value goes to C, and the call takes its address by itself. Every pointer is only good until C returns.

| Parameter | C gets |
| --- | --- |
| `mut T x` | `T *`: the caller's variable, which C can change |
| `in T x` | `const T *`: the caller's variable, read-only, with no copy (a copy for a value that's no variable's) |
| `List<T> xs` | `const T *`: the list's elements, NULL when it's empty. Pass `xs.count` too |
| `mut List<T> xs` | `T *`: the elements, which C can change, but not how many there are |
| `string s` | `const char *`: UTF-8, ending in a zero |

A world keeps a list whose elements take more than 16 KB in pieces, so that changing one element doesn't copy the rest. C gets those elements side by side in a copy, made for the call, and with `mut`, written back into the list once C returns. Smaller lists, and lists that aren't a world's, go to C as they are.

An extern function can return `string` from C's `const char *`: the text is copied, so C can reuse its buffer, and NULL is empty text.

```csharp
extern float Average(List<float> values, int count);
extern string Describe(in Stats stats);

system Report(Scores scores, mut Summary summary)
{
    summary.average = Average(scores.values, scores.values.count);
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
#include "tide/math.h"

typedef struct Hit
{
    tide_float3 point;
    float distance;
} Hit;

bool RayCast(tide_float3 from, tide_float3 direction, Hit *hit)
{
    // ...
    return false;
}
```

Tide's types have no padding the compiler adds, so a C struct with the same fields lines up with them. Structs that hold text or lists can't go to C, and neither can lists of text or `T?` values.

C functions can't `fail` (see [Errors](./errors.md)): they return what C returns. To turn a C function's error code into an error, check it in a Tide function that fails, and call that.

## Order

C can keep state, so calling it is a side effect, and Tide keeps its order: in `Pick(Roll(), Roll())`, the first `Roll` runs first on every platform, though C itself would let each compiler pick. The same goes for functions, methods and operators that call C, and for calls inside `&&`, `||` and `?:`, whose parts still only run when they would.

## What C is trusted with

The compiler doesn't look inside C. It takes each call as touching nothing it tracks, so C never makes systems wait for each other, and anything C does is the game's to get right:

- **Determinism.** Match code runs the same on every machine only if its C does too: no platform math library (`sinf`, `powf`), and nothing that depends on the machine. The game's own C and C++ files get the engine's flags, which keep float math exact; a prebuilt library's flags are whatever it was built with.
- **State.** Variables C keeps aren't in the world, so they aren't sent, rolled back or hashed. Keep what the match depends on in components and singletons.
- **Threads.** Once systems run in parallel, two that call the same C function can run at the same time.

C also changes how players join. When starting a match calls no C (no match event handler does, through anything it calls), a player joining a big world starts the match on its own machine too, and only gets what changed since (see [Sending worlds](../engine/networking.md#sending-worlds)). C called while a match starts could do something outside the world on every joining machine, so a game whose match start calls C sends joining players the whole world instead.

tidec declares each extern function from its Tide signature. If it doesn't match the C function, the call goes wrong the way it would in C.
