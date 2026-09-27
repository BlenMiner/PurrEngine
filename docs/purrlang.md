# PurrLang

The language PurrEngine games and built-in systems are written in. It transpiles to C (see AGENTS.md). This file is the source of truth for the language: only decisions the owner has approved go in **Decided**. Proposals and open questions go in **Open**.

PurrLang is a working name and may change.

## Decided

### Files and tools

- Source files use the `.purr` extension.
- The compiler (transpiler) is `purrc`.

### Style

- The syntax is C#-like.
- Types, systems and methods use PascalCase: `Transform`, `MovePlayer`, `Spawn(...)`.
- Fields, parameters and locals use camelCase (PurrNet style): `trs.position`, not `trs.Position`.
- Attributes (`[...]`) are only for metadata, not for core semantics.

### Declarations

- `component` declares a component.
- `singleton` declares world-wide state (what other ECSs call a resource). There is exactly one instance **per world**, not per process.
- `system` declares a system.

### Field defaults

- Component and singleton fields can declare a default value: `int value = 100;`.
- A default must be a constant expression: literals, `float3(...)` of constants, and operators. It can't read fields, singletons or `Time`.
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

### Archetypes

- There are no archetype declarations. The compiler derives every archetype from the code: the component set at each spawn site, plus every combination reachable through adding and removing components.

## Provisional: implemented in v0, awaiting approval

purrc v0 needed answers to these to work end to end. They're implemented, but the owner hasn't approved them yet, so any of them can change.

### Types and values

- Built-in types are `bool`, `int` (32-bit), `float` (32-bit), `float3` and `Entity`. There is no `double`.
- `1.5` is a `float`; the `f` suffix is optional. An `int` converts to `float` implicitly, never the other way.
- `float3(x, y, z)` builds a vector with members `.x`, `.y`, `.z`. `+ - * /` work component-wise between two `float3`s, `float3 * float`, `float * float3` and `float3 / float` scale, and unary `-` negates.
- Integer arithmetic wraps on overflow. Integer division and modulo by zero give 0, so no input can crash the simulation.
- Bitwise operators `& | ^ ~ << >>` work on `int`, with compound forms `&= |= ^= <<= >>=`. As in C#, shift counts use their low 5 bits (`1 << 33` is `2`) and `>>` keeps the sign. Operator precedence follows C#.
- Integer literals can be decimal (`255`), hex (`0xFF`) or binary (`0b1010`). Decimal goes up to 2147483647. Hex and binary can use all 32 bits and are read as the int's bit pattern: `0xFFFFFFFF` is `-1` and `0x80000000` is the lowest int.
- `_` separates digits in any number, as in C#: `1_000_000`, `0b1111_0000`, `0x_FF_FF`. It's allowed between digits and right after `0x` or `0b`, but not at the start or end or next to `.`.
- Local variables need an initial value.

### Systems

- A system with no component or `Entity` parameters runs once per tick. Otherwise it runs once per matching entity.
- `return;` ends the system for the current entity.
- Locals can't reuse the name of another local or parameter in scope.
- Names starting with `purr_` are reserved for generated code.

### Entities

- `e.Destroy()` destroys an entity. It's deferred like other structural changes.
- The "fixed point" where structural changes apply is the end of each tick (and the end of `Main`). Changes apply in the order they were recorded.
- `Add`, `Remove` and `Destroy` on an entity that was already destroyed do nothing.

### Built-ins

- `Time` is a built-in singleton with `float dt` (the fixed tick length) and `int tick` (starts at 0). Systems can read it but not write it.

### Syntax

- Statements: blocks, `if`/`else`, `return;`, local declarations, assignments (`= += -= *= /= %=`), and calls to `Spawn`, `Add`, `Remove` and `Destroy`. There are no loops yet.
- Operators and their precedence follow C#. Comments are `//` and `/* */`.

### Limits

- 64 components, 256 archetypes, 16384 entities, 1024 entities per archetype and 4096 structural changes per tick. The last three can be raised with compile definitions.

## Open

- How entities authored as data (levels, prefabs) feed into archetype derivation.
- Archetype growth. Every `Add` and `Remove` can apply to any entity, so the compiler assumes every combination is reachable, and each archetype currently reserves a fixed 1024 slots. Narrowing this safely needs more analysis, and storage should grow on demand.
- Which system runs first by default. For v0: declaration order, compiling a single file.
- How modules, such as the engine's built-in systems, initialize when there's a single `Main`.
