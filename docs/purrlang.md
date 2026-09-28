# PurrLang

The language PurrEngine games and built-in systems are written in. It transpiles to C (see AGENTS.md). This file is the source of truth for the language: only decisions the owner has approved go in **Decided**. Proposals and open questions go in **Open**.

PurrLang is a working name and may change.

## Decided

### Files and tools

- Source files use the `.purr` extension.
- The compiler (transpiler) is `purrc`.
- A game is one or more `.purr` files: by default, every `.purr` file in the game's folder and its subfolders. Every declaration is visible from every file of the game; there are no imports between files.
- A game needs no C: the engine runs it. A custom C host is optional, for tests or special hosts.

### Namespaces

- A file can put its declarations in a namespace, and namespaces are how large games keep names apart.
- Code outside the namespace names its declarations with it (`Combat.Health`), or imports it with `using Combat;`.

### Order of systems

- Systems run in one deterministic order, the same on every platform. By default it follows the files, sorted by path, then the order of declarations in each file.
- Attributes change the order of a system relative to others.

### Style

- The syntax is C#-like.
- Types, systems and methods use PascalCase: `Transform`, `MovePlayer`, `Spawn(...)`.
- Fields, parameters and locals use camelCase (PurrNet style): `trs.position`, not `trs.Position`.
- Public properties use camelCase too, even static ones: `Color.red`, `quaternion.identity`. True constants use FULL_CASE: `Math.PI`, `Math.TAU`.
- Attributes (`[...]`) are only for metadata, such as when a system runs or the bounds of an input field, not for what code does. The engine may enforce what an attribute declares, as it clamps an input field to its `[Clamp]`.

### Declarations

- `component` declares a component.
- `singleton` declares world-wide state (what other ECSs call a resource). There is exactly one instance **per world**, not per process.
- `system` declares a system.

### Field defaults

- Component and singleton fields can declare a default value: `int value = 100;`.
- A default must be a constant expression: literals, constructors of built-in types, `Math` functions, built-in constants like `quaternion.identity`, and operators, as in `float angle = Math.Radians(45);`. It can't read fields, singletons or `Time`.
- Singletons start with their defaults when the world is created, before `Main` runs.
- Components get their defaults whenever a value is created without setting that field: `Spawn(Health)`, `e.Add(Health)`, and fields left out of `Health { max = 200 }`.
- Fields without a default start at zero. `Entity` fields always start as the null entity and can't have a default.

```csharp
singleton Physics
{
    int collisionCount = 69;
    float3 gravity = float3(0, -9.81, 0);
}

component Health
{
    int value = 100;
    int max = 2 * 2 * 25;
}
```

### Mutability

- Everything is read-only by default. Mutability is opt-in with `mut`.
- This includes local variables. `var` infers the type, like C++ `auto`, and is read-only.

```csharp
var speed = player.speed * 2;    // read-only, type inferred
mut var total = 0.0;             // mutable, type inferred
float limit = 10;                // read-only, explicit type
mut float scale = 1;             // mutable, explicit type
```

### System parameters

A system's parameter list describes its whole query. Each parameter's modifier says how the system relates to that component or singleton:

| Modifier  | Meaning                                                                 |
|-----------|-------------------------------------------------------------------------|
| none      | Read access                                                             |
| `mut`     | Write access                                                            |
| `with`    | Entity must have this component. No data access, so no data dependency. |
| `without` | Entity must not have this component.                                    |

```csharp
system ClampPlayer(mut Transform trs, with Player)
{
    if (trs.position.y < 0)
        trs.position.y = 0;
}
```

- An `Entity` parameter gives the handle of the entity being processed: `system Die(Entity e, Health health)`.

### Entities

- `Spawn(...)`, `entity.Add(...)` and `entity.Remove(...)` take plain component lists. `with` and `without` only appear in queries.
- Component values are written without `new`. A bare type name means all fields get default values.
- An entity has at most one component of each type. `Add` on a component the entity already has replaces its value. `Remove` of a component the entity doesn't have does nothing.
- When an entity needs several of something, use a list inside one component, one entity per item pointing back at its owner, or separate component types.
- Structural changes (`Spawn`, `Add`, `Remove`) are deferred. They're applied together at the next fixed point in the tick, in a deterministic order. The entity handle `Spawn` returns is usable immediately, for example to store in a component. The entity's data becomes readable once the changes are applied.

```csharp
var e = Spawn(Transform { scale = float3(1, 1, 1) }, Player);
e.Add(Stunned { duration = 2 });
e.Remove(Stunned);
```

### Entry point

- `system Main()` is the program's entry point, like `main` in C. There is exactly one.
- It runs once when a world is created, before the first tick. It sets up the initial world, not the game loop: the engine runs the tick.
- Structural changes made in `Main` follow the same deferral rule and are applied when `Main` returns.

```csharp
system Main()
{
    Spawn(Transform { scale = float3(1, 1, 1) }, Player { speed = float3(0, 0, 5) });
}
```

### Evaluation order

- Expressions evaluate left to right, like C#: operands, arguments and field initializers run in source order. `Spawn` is the only expression with a side effect today, so this is what makes entity IDs come out the same on every platform. `Spawn(Pair { a = Spawn(Thing), b = Spawn(Thing) })` spawns `a`'s Thing, then `b`'s, then the Pair.
- `cond ? a : b` runs the condition first, then only the side it picks, as in C#.

### Archetypes

- There are no archetype declarations. The compiler derives every archetype from the code: the component set at each spawn site, plus every combination reachable through adding and removing components.

## Provisional: implemented in v0, awaiting approval

purrc v0 needed answers to these to work end to end. They're implemented, but the owner hasn't approved them yet, so any of them can change.

### Types and values

- Built-in scalar types are `bool`, `int` (32-bit), `float` (32-bit) and `Entity`. There is no `double`. Vector types are under Vector math, and `Color` under Views and drawing.
- `1.5` is a `float`; the `f` suffix is optional. An `int` converts to `float` implicitly, never the other way.
- In `cond ? a : b`, the condition is a `bool`, and the two sides have the same type or one converts to the other's, as in C#: `ready ? 1 : 0.5` is a `float`. It binds looser than every binary operator and groups to the right.
- Integer arithmetic wraps on overflow. Integer division and modulo by zero give 0, so no input can crash the simulation.
- Bitwise operators `& | ^ ~ << >>` work on `int`, with compound forms `&= |= ^= <<= >>=`. As in C#, shift counts use their low 5 bits (`1 << 33` is `2`) and `>>` keeps the sign. Operator precedence follows C#.
- Integer literals can be decimal (`255`), hex (`0xFF`) or binary (`0b1010`). Decimal goes up to 2147483647. Hex and binary can use all 32 bits and are read as the int's bit pattern: `0xFFFFFFFF` is `-1` and `0x80000000` is the lowest int.
- `_` separates digits in any number, as in C#: `1_000_000`, `0b1111_0000`, `0x_FF_FF`. It's allowed between digits and right after `0x` or `0b`, but not at the start or end or next to `.`.
- Local variables need an initial value.

### Systems

- A system with no component or `Entity` parameters runs once per tick. Otherwise it runs once per matching entity.
- A parameter that's never used, or declared `mut` and never written, is a warning: it makes other systems wait for nothing. A component that's only there to filter entities belongs in `with`.
- `return;` ends the system for the current entity.
- Locals can't reuse the name of another local or parameter in scope.
- Names starting with `purr_` are reserved for generated code.

### Entities

- `e.Destroy()` destroys an entity. It's deferred like other structural changes.
- The "fixed point" where structural changes apply is the end of each tick (and the end of `Main`). Changes apply in the order they were recorded.
- `Add`, `Remove` and `Destroy` on an entity that was already destroyed do nothing.
- `Spawn` can't appear on the right side of `&&` or `||`, or in a side of `?:`. Those parts only run sometimes, while spawns run first, in order (see Evaluation order). Spawn into a local before the condition, or use `if`/`else`.

### Built-ins

- `Time` is a built-in singleton with `float dt` (the fixed tick length) and `int tick` (starts at 0). Systems can read it but not write it.

### Syntax

- Statements: blocks, `if`/`else`, `return;`, local declarations, assignments (`= += -= *= /= %= <<= >>= &= |= ^=`), and calls to `Spawn`, `Add`, `Remove`, `Destroy` and the `Draw` functions. There are no loops yet.
- Operators and their precedence follow C#. Comments are `//` and `/* */`.

### Limits

- 64 components, 256 archetypes, 16384 entities, 1024 entities per archetype and 4096 structural changes per tick. The last three can be raised with compile definitions.

### Namespaces

- `namespace Game.Combat;` at the top of a file puts everything in the file in that namespace. A file has at most one namespace; files without one are in the global namespace. Namespaces can be dotted.
- `using Physics;` at the top of a file lets it name Physics' declarations without the prefix. `namespace` and `using` come before any declaration.
- A plain name is looked up in the file's namespace, then the namespaces around it, then the `using` namespaces, then the global namespace. If two `using` namespaces both have it, it's ambiguous: write the namespace.
- Qualified names work wherever a type is named: parameters (`mut Combat.Health health`), `with` and `without`, component values (`Combat.Health { value = 10 }`), `Spawn`, `Add` and `Remove`.
- The same name can be declared in different namespaces. Built-in names can't start a namespace (`namespace Math;` is an error).
- In generated C, namespaced declarations are prefixed with their namespace: `Combat.Health` is `Combat_Health`, read with `purr_get_Combat_Health`.
- There's exactly one `Main` in a game, in any file or namespace. `namespace` and `using` are only keywords at the top of a file.

```csharp
// combat.purr
namespace Combat;

component Health { int value = 100; }

// main.purr
using Combat;

system Main()
{
    Spawn(Health, Items.Health { value = 3 });
}
```

### Order of systems

- Files compile in order of their paths, compared byte by byte. Systems run in that order, then in the order they're declared within a file. Views, which draw once per frame, follow the same rules among themselves; later views draw on top.
- `[Before(X)]` makes a system run before `X`, and `[After(X)]` after it. They take any number of systems (`[After(Gravity, Collisions)]`), with qualified names when needed (`[After(Physics.Gravity)]`).
- Otherwise the default order holds: of the systems whose constraints are met, the earliest in the default order runs next.
- Constraints that form a loop are an error naming the loop. Systems and views are ordered separately, and `Main` isn't ordered: it runs once, before everything.
- The language server shows a system's place in the order when hovering it.
- Systems that share no data they write can run at the same time; the others wait, in this order. Above each system, the language server shows its stage and why it waits ("stage 2 · after Move: both write Body"), and `purrc --schedule` prints the whole plan.

```csharp
[After(Physics.Gravity)]
system Move(mut Body body) { ... }
```

## Vector math

Follows Unity.Mathematics, with PurrLang's naming. Everything in this section is decided.

- Types: `float2`, `float3`, `float4`, `int2`, `int3`, `int4`, `quaternion`, `float2x2`, `float3x3` and `float4x4`. They're lowercase built-in value types, like `float`.
- Math functions live on `Math` and follow the PascalCase method convention: `Math.Dot(a, b)`, `Math.Normalize(v)`, `Math.Mul(q, r)`. This overrides Unity.Mathematics' lowercase `math.dot`.
- Swizzles read any combination of components: `v.xz`, `v.zyx`, `v.xxyy`.
- Angles are in radians. `Math.Radians(degrees)` and `Math.Degrees(radians)` convert.
- Math is forgiving with bad values, which can come from other players' input. `Math.Clamp` always returns a value in range: NaN gives the lower bound. `Math.Min` and `Math.Max` with one NaN argument return the other. Everywhere else, results match Unity.Mathematics.

```csharp
quaternion spin = quaternion.AxisAngle(float3(0, 1, 0), input.turn * time.dt);
trs.rotation = Math.Mul(trs.rotation, spin);
float3 forward = Math.Rotate(trs.rotation, float3(0, 0, 1));
trs.position += Math.Normalize(forward) * 5 * time.dt;
float2 flat = trs.position.xz;
```

**Constructors**

- Vectors take any mix of scalars and vectors adding up to the right size (`float4(v.xy, 0, 1)`), or one scalar for every component (`float3(1)`).
- `float3(int3)` and `int3(float3)` convert between int and float vectors. `int(x)` and `float(x)` convert scalars. Float to int truncates toward zero, saturates at the int range, and turns NaN into 0, so no input is undefined.
- `quaternion(x, y, z, w)`, `quaternion(float4)` and `quaternion(float3x3)`.
- Matrices take one column vector per column (`float3x3(c0, c1, c2)`) or all numbers row by row (`float2x2(1, 2, 3, 4)`, where `1, 2` is the first row). Also `float3x3(quaternion)` and `float4x4(float3x3 rotation, float3 translation)`.

**Members and constants**

- Vectors have `x`, `y`, `z`, `w`. Swizzles can also be assigned (`v.xz = float2(1, 2)`), as long as no component repeats.
- `quaternion` has `value`, a `float4` with (x, y, z) as the vector part. Matrices are stored column by column and have columns `c0` to `c3`.
- `Math.PI`, `Math.TAU`, `Math.E`, `quaternion.identity`, `float2x2.identity`, `float3x3.identity`, `float4x4.identity`.
- `quaternion.AxisAngle(axis, angle)`, `quaternion.Euler(radians)` (Z first, then X, then Y, Unity's default order), `quaternion.LookRotation(forward, up)`, `float4x4.TRS(translation, rotation, scale)`, `float4x4.Translate(translation)`.

**Operators**

- `+ - * /` are component-wise on vectors, and `%` too on int vectors. A scalar widens to the vector's size, and int widens to float, so `v * 2 + 1` works.
- An int vector converts to a float vector of the same size implicitly, like `int` to `float`.
- Matrices support `+` and `-` with the same type, and `*` and `/` by a number. There's no `*` between matrices, or between a matrix and a vector; that's `Math.Mul`.
- Quaternions have no operators: combine rotations with `Math.Mul` and rotate vectors with `Math.Rotate`.
- Comparisons and `==` work on scalars only.

**Functions**

- Component-wise, on numbers and vectors: `Abs`, `Sign`, `Min`, `Max`, `Clamp` (ints too); `Floor`, `Ceil`, `Round` (ties to even), `Trunc`, `Frac`, `Sqrt`, `Rsqrt`, `Saturate`, `Radians`, `Degrees`, `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Exp`, `Exp2`, `Log`, `Log2`, `Log10`, `Pow`, `Step`, `Lerp`, `Unlerp`, `SmoothStep`.
- Vectors: `Dot`, `Cross`, `Length`, `LengthSq`, `Distance`, `DistanceSq`, `Normalize`, `NormalizeSafe` (zero instead of NaN), `Reflect`, `Csum`, `Cmin`, `Cmax`.
- Quaternions: `Mul`, `Rotate`, `Inverse`, `Conjugate`, `Normalize`, `NormalizeSafe`, `Dot`, `Slerp`, `Nlerp`, `Forward`, `Up`, `Right`, `Angle`.
- Matrices: `Mul`, `Transpose`, `Inverse`, `Determinant`, and for `float4x4`, `Transform` (a point) and `Rotate` (a direction).
- Everything is deterministic (see AGENTS.md). The transcendental functions are PurrEngine's own, accurate to about 1 ulp but not correctly rounded.

## Input

### Decided

- One `input` declaration per game describes one player's input for one tick. It's what the network sends.
- Input is one value per player per tick. The engine writes it, and the simulation can only read it.
- `Owner` is a built-in component that ties an entity to a player. It's a normal component: it can be read, written, added and removed. Writing it hands control over.
- Players are identified by a built-in `PlayerID` type, not an `int`. Like `Entity`, it's opaque, comparable with `==`, and has a null value. The simulation only sees `PlayerID`s and never connections, so a player who reconnects and gets their `PlayerID` back (PurrNet style) keeps everything they owned. `PlayerID(0)` names a player by index, for local play and tests.
- **Sampling is PurrLang code:** the input's `Sample(Devices devices)` method. The engine calls it on the client once per tick. Fields start at their defaults, and `Sample` assigns them by name, without `mut`. It runs outside the simulation: it can read `Devices` but not components or singletons.
- **Input carries whether buttons are held, not whether they just went down,** because a missing remote input is guessed by repeating the last one. On a device, `.pressed` means down at any point since the last sample, so a quick tap between ticks is never lost. In the simulation, `bool` input fields get `.down` and `.up`, computed against the previous tick.
- An input parameter in a system gives the input of the player who owns the entity. Entities without an `Owner`, or whose owner isn't a known player, get the **server's input**, and so do systems that run once per tick. This is how the server controls what no player owns (PurrNet style). An input parameter doesn't filter entities: add `with Owner` to only run on owned ones.
- An input can have a `Sanitize()` method. Every input passes through it before the simulation reads it, including input from other players, so systems can rely on what it guarantees without checking again. It assigns the input's fields by name, like `Sample`, and reads nothing else.
- Input fields can declare bounds: `[Clamp(lo, hi)]`, `[Min(x)]` and `[Max(x)]`. The engine applies them to every input before `Sanitize`, so `Sanitize` only handles what they can't express. Bounds are constants; a number bounds every component of a vector.
- **Input is an attack point,** so the engine is forgiving with it. Before `Sanitize` runs, NaN and infinite floats become the field's default. Nothing a client sends can put NaN in the simulation, and `Sanitize` only deals with values that are merely out of range.
- `Devices` has a keyboard, mouse and gamepad for now; pen, touch, joysticks and sensors come later. Every button has `.pressed` (held), `.down` (went down since the last sample) and `.up` (went up), named as in Unity: the Input System's `isPressed`, and the old `GetKeyDown` and `GetKeyUp`.
  - **Keyboard:** every key by physical position, named after the US layout (`keys.w`, `keys.space`, `keys.leftShift`, `keys.digit1`, `keys.upArrow`, `keys.f1`). WASD works on AZERTY.
  - **Mouse:** `position`, `delta` and `scroll` (`float2`), and buttons `left`, `right` and `middle`.
  - **Gamepad:** `connected`; `leftStick` and `rightStick` (`float2`); `leftTrigger` and `rightTrigger` (`float`, 0 to 1); face buttons by position (`buttonSouth`, `buttonEast`, `buttonWest`, `buttonNorth`); `dpad.up` and the other directions; `leftShoulder`, `rightShoulder`, `start` and `select`.
  - **Axes follow Unity:** `y` is positive up for sticks and the mouse. Mouse `position` is in window pixels from the bottom left. `scroll.y` is positive when scrolling away from the user.

```csharp
input PlayerInput
{
    float2 move;
    bool jump;

    Sample(Devices devices)
    {
        var keys = devices.keyboard;
        if (keys.d.pressed) move.x += 1;
        if (keys.a.pressed) move.x -= 1;
        move += devices.gamepad.leftStick;
        jump = keys.space.pressed || devices.gamepad.buttonSouth.pressed;
    }
}

system Jump(PlayerInput input, mut Velocity velocity)
{
    if (input.jump.down)
        velocity.value.y = 5;
}
```

```csharp
input PlayerInput
{
    [Clamp(-1, 1)] float2 move;
    [Min(0), Max(3)] int gear = 1;
    bool boost;

    Sample(Devices devices) { move = devices.gamepad.leftStick; }

    // After the bounds: what attributes can't say.
    Sanitize()
    {
        if (gear == 0) boost = false;
    }
}
```

### Provisional

- `input` is only a keyword at the start of a declaration, so it can still name parameters and locals.
- A player, or the server, whose input isn't set for a tick keeps their last one. Until then, inputs are the defaults.
- In local play, the machine is also the server: `purr/run.h` gives the devices to player 0 and to the server.
- The input's defaults go through the same steps as any input: NaN repair, bounds, then `Sanitize`.
- Up to 16 players for now.
- A system can have one input parameter.
- Extra device members beyond the list above: mouse `back` and `forward`, gamepad `leftStickButton` and `rightStickButton`, and the full key list in `engine/include/purr/devices.h`.
- The types inside `Devices` (keyboard, button, and so on) have no names in PurrLang; use `var`.
### Open

- Players joining, leaving and reconnecting.
- Pairing devices with players, for local multiplayer.
- Compact input types (bytes, quantized floats) to save bandwidth.
- The server's input computed from the game's state (AI) rather than from devices.

## Views and drawing

### Decided

- Drawing is immediate mode: code calls `Draw` functions every frame, and nothing is kept between frames.

### Provisional

- `view` declares a view. It looks like a system and takes the same parameters, but it runs once per rendered frame instead of once per tick, and it only reads the world. `mut` parameters, input parameters, `Spawn`, `Add`, `Remove` and `Destroy` are errors in a view.
- Views run in declaration order, after all the systems of the frame's ticks. Within a view, entities run in the same order as in systems.
- `Draw` functions can only be called from views for now. Calling them from systems needs to tell predicted ticks from verified or replayed ones, which comes with multiplayer.
- `view` is only a keyword at the start of a declaration, like `input`.
- Positions and sizes are in world units with `y` up. The camera maps them to the screen.
- The Draw functions:
  - `Draw.Clear(color)`: fills the screen.
  - `Draw.Camera(center, size)`: the camera for the Draw calls after it. `center` is the world position at the middle of the screen, and `size` is half the visible height, like Unity's orthographic size. Each frame starts black, with the camera at the origin and one world unit per pixel.
  - `Draw.Circle(center, radius, color)` and `Draw.WireCircle(center, radius, color)`.
  - `Draw.Rect(center, size, color)` and `Draw.WireRect(center, size, color)`.
  - `Draw.Line(from, to, color)`.
  - `Draw.Text(text, position, size, color)`: `position` is the top left corner and `size` the height.
- Later Draw calls draw over earlier ones.
- `Color` is a built-in value type with `r`, `g`, `b` and `a`, floats from 0 to 1, as in Unity. It's built with `Color(r, g, b)` (alpha 1) or `Color(r, g, b, a)`. The constants are `Color.white`, `black`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `gray` and `clear`, with Unity's values and names. Components and singletons can hold colors. There are no operators on colors yet.
- Text is written in double quotes, with the escapes `\"`, `\\` and `\n`, in printable ASCII. For now, text can only be passed directly to `Draw.Text`.

```csharp
view DrawBalls(Body body, Ball ball)
{
    Draw.Circle(body.position, body.radius, ball.color);
}

view DrawHud(Arena arena)
{
    Draw.Camera(float2(0, 0), arena.halfSize.y);
    Draw.Text("fire: space", float2(-arena.halfSize.x, arena.halfSize.y), 18, Color.gray);
}
```

### Open

- Drawing from systems, with the prediction stage (verified, predicted, replayed) visible to the code.
- Views reading input, for example to draw where the local player aims before the tick runs.
- Smoothing between ticks: views currently see only the latest tick.
- State that belongs to views, such as animation timers and particles. It must stay outside the world so it never affects determinism.
- Text with values in it, such as C#'s `$"score {score}"`.
- 3D drawing, sprites and textures, layers.

## Open

- User-defined value types (structs), including thin wrappers around a single `int`. The owner prefers distinct types over raw primitives for clarity and refactoring.

- How entities authored as data (levels, prefabs) feed into archetype derivation.
- Archetype growth. Every `Add` and `Remove` can apply to any entity, so the compiler assumes every combination is reachable, and each archetype currently reserves a fixed 1024 slots. Narrowing this safely needs more analysis, and storage should grow on demand.
- How modules, such as the engine's built-in systems, initialize when there's a single `Main`.
- Groups of systems (phases such as input, simulation, late), which Before and After could order as a whole.
- Access control: whether a namespace can keep declarations to itself.
