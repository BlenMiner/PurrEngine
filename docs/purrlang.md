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
- Braces go on lines of their own, as in C#, and so does an `else` after a block. A block that fits on one line can stay there (`scene Main { }`, `if (dead) { return; }`), and so do literals (`Body { position = p }`). The language server's formatter lays code out this way, indenting as the editor is set to (four spaces by default).

### Declarations

- `component` declares a component.
- `singleton` declares world-wide state (what other ECSs call a resource). There is exactly one instance **per world**, not per process.
- `struct` declares a value type for fields and locals (see Structs).
- `system` declares a system.
- `event` declares an event, and `event(...)` code that runs when one is sent (see Events).
- `enum` declares an enum (see Enums and switch).
- `scene` declares a scene (see Scenes).
- `local` in front of a declaration makes it belong to the machine instead of the match (see Local state).
- A function, `ReturnType Name(parameters) { ... }` with no keyword, declares code that other code calls (see Functions).

### Field defaults

- Fields of components, singletons, inputs and structs can declare a default value: `int value = 100;`.
- A default must be a constant expression: literals, constructors of built-in types, struct values of constants (`Range { hi = 5 }`), `Math` functions, built-in constants like `quaternion.identity`, and operators, as in `float angle = Math.Radians(45);`. It can't read fields, singletons or `Time`.
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

### Structs

- A struct is plain data, copied when it's assigned, with no references. The world stays plain data, so copying it is still a snapshot.
- Structs are the types of fields (of components, singletons, inputs and other structs) and of locals. System parameters stay components, singletons, the input and `Entity`.
- A value is written like a component's: `Stats { armor = 2 }`. Fields left out take their default, and a struct field without one takes its struct's defaults.
- A struct's fields change through whatever holds it: `unit.stats.health -= 5` needs `mut Unit unit`, so a system's signature still says what it writes. A local copy changes with `mut var`.
- A struct can't contain itself, even through other structs: it would be infinitely big.
- Namespaces apply as to every declaration: another namespace names it `Combat.Stats`.
- `[Clamp]`, `[Min]` and `[Max]` on a struct's field are enforced where untrusted data enters the simulation: in an input that holds the struct, before `Sanitize`, as on the input's own fields. Elsewhere they only describe the field.
- `==` compares structs only if the struct declares it (see Operators).
- Structs have methods and operators (see Methods and Operators).

```csharp
struct Range
{
    float lo;
    float hi = 1;
}

struct Stats
{
    float health = 100;
    Range damage;                   // lo = 0, hi = 1: Range's defaults
    Range armor = Range { hi = 5 };
}

component Unit
{
    Stats stats;
}

system Hurt(mut Unit unit)
{
    unit.stats.health -= unit.stats.damage.hi;
}
```

### Methods

- Structs and components can have methods. Their fields are in scope by name, and so are their other methods.
- A method only reads the fields, unless it's `mut`. Calling a `mut` method changes what it's called on, so it needs write access, like an assignment: `unit.stats.Hurt(5)` needs `mut Unit unit`, and the system's signature still says what it writes. A read-only method can't call a `mut` one.
- A method returns a value with `return value;`, on every path, unless it returns `void`.
- A method sees its fields, its parameters and `Math`. It can't spawn, change entities or draw: systems do.
- Methods can't share a name, even with different parameters, and a method can't share one with a field.
- Singletons and inputs have no methods (an input has `Sample` and `Sanitize`): keep the data and its methods in a struct, and the struct in them.
- Parameters work as in functions.

```csharp
struct Stats
{
    float health = 100;

    bool IsDead() { return health <= 0; }

    mut void Hurt(float amount)
    {
        health -= amount;
        if (IsDead()) health = 0;
    }
}

system Burn(mut Unit unit)
{
    unit.stats.Hurt(1);
}
```

### Functions

- A function is code that other code calls: systems, views, the input's `Sample` and `Sanitize`, methods and other functions. The engine never runs one by itself.
- A parameter is a copy, read-only. A `mut` parameter is the caller's variable itself, which the function changes: the caller passes something it can write, and its signature still shows the write, so `Heal(unit.stats, 5)` needs `mut Unit unit`. The argument's type matches exactly.
- Parameters and return values are built-in types, structs and components.
- Functions follow the rules of methods: they see their parameters and `Math`, can't spawn or change entities, and return a value on every path unless they return `void`.
- A function can draw and use the GUI. Then only views, and other functions like it, can call it, as with `Draw`.
- Namespaces apply: another namespace calls it `Combat.Heal(...)`. A function shares its name with nothing else in its namespace.
- A function's last parameter can be a `Block`: code the caller writes in braces after the call, `Section("Audio") { ... }`. A function takes at most one, always last, and it's always written after the call, never inside the parentheses.
- The function runs the block by calling it, `content();`, as many times as it chooses, including none. The block runs as if it were written at the call: it sees the caller's locals and parameters, and what it reads and writes counts toward the caller's signature.
- A function that takes a `Block` is inlined where it's called, so blocks cost nothing and need no closures. It can't call itself, directly or through other functions, and a block can only be run, not stored.

```csharp
// A container of your own: runs its content only while open.
void Foldout(string title, mut bool open, Block content)
{
    GUILayout.Toggle(title, open);
    if (open) content();
}

view Options(mut Main menu, mut Settings settings)
{
    Foldout("Audio", menu.audioOpen)
    {
        GUILayout.Slider("Volume", settings.volume, 0, 1);
    }
}
```

```csharp
float Heal(mut Stats stats, float amount)
{
    stats.health = Math.Min(stats.health + amount, 100);
    return stats.health;
}

system Regenerate(mut Unit unit)
{
    Heal(unit.stats, 0.5);
}
```

### Operators

- A struct can declare operators, in C#'s form: `Money operator +(Money a, Money b) { ... }`. They compile to plain function calls.
- The operators are `+ - * / % & | ^ << >>`, the comparisons `== != < <= > >=`, which return `bool`, and `-` (negation), `!` and `~` with one parameter.
- At least one parameter is the struct itself. Parameters are copies, and an operator has no fields in scope: it only sees its parameters.
- As in C#, `==` and `!=` come in pairs, and so do `<` and `>`, and `<=` and `>=`.
- A compound assignment uses its operator: `total += price` uses `+`.
- Several operators with the same symbol can take different types, like `Money * int` and `int * Money`. The one whose parameters match the operands exactly wins over one that needs `int` to `float`; if two match as well, it's an error.

```csharp
struct Money
{
    int cents;

    Money operator +(Money a, Money b) { return Money { cents = a.cents + b.cents }; }
    Money operator *(Money a, int times) { return Money { cents = a.cents * times }; }
    bool operator ==(Money a, Money b) { return a.cents == b.cents; }
    bool operator !=(Money a, Money b) { return !(a == b); }
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

- The scene named `Main` is where the program starts (see Scenes). There is exactly one.
- A local `Main` starts the program in it, usually a menu. A match `Main` starts a single-player match with it right away, the same as a menu starting one.
- `Main` sets up the first scene, not the game loop: the engine runs the tick.

```csharp
scene Main { }

event(Spawned) Setup(with Main)
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

- Built-in scalar types are `bool`, `int` (32-bit), `float` (32-bit), `Entity` and `LocalEntity` (see Local state). There is no `double`. Vector types are under Vector math, and `Color` under Views and drawing.
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

- Statements: blocks, `if`/`else`, `switch`, `break;`, `return;`, local declarations, assignments (`= += -= *= /= %= <<= >>= &= |= ^=`), and calls to `Spawn`, `Add`, `Remove`, `Destroy`, `Send` and the `Draw` functions. There are no loops yet.
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

scene Main { }

event(Spawned) Setup(with Main)
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

## Events

### Decided

- An event is something that happened, sent from one part of the game to the rest: a hit, a pickup, a round ending. `event Hit { ... }` declares one, with fields like a struct's, which carry its context.
- `event(Hit hit) TakeHit(...)` declares a handler: code that runs when a `Hit` is sent. The same keyword declares both, and `event(` starts a handler. The first parentheses hold the trigger, exactly one event. The second list takes the same parameters as a system.
- An event with no fields needs no name in the trigger: `event(Spawned) Arm(...)`.
- `entity.Send(Hit { ... })` sends an event to an entity. `Send(RoundOver { ... })` sends it to the whole world.
- A handler's components and `Entity` come from the entity the event was sent to, as a system's come from the entity it runs for. If that entity doesn't match the handler's parameters, the handler doesn't run for it.
- A handler that takes components or an `Entity` needs that entity, so every `Send` of its event must name one. The compiler checks every `Send`: `Send(Hit { ... })` is an error when a `Hit` handler needs an entity, and says to write `entity.Send(...)`.
- A handler that takes no components and no `Entity` runs once per event, whether it was sent to an entity or not.
- The sender isn't recorded. When handlers need it, it goes in a field: the entity that sends is often not the one that matters, like a bomb sending a `Hit` for whoever threw it.
- Events are handled at the end of the tick, with structural changes, in the order they were all recorded. A `Send` before a `Destroy` of the same entity is handled while the entity still exists. An event sent to an entity that's gone by its turn is dropped, as `Add` on a destroyed entity does nothing.
- Handlers can send events and change entities in turn. Those are handled next, until nothing is left, so everything settles within the tick. Systems later in the same tick don't see an event's effects yet, as with `Spawn`.
- The handlers of one event run in declaration order, and `[Before]` and `[After]` order them as they do systems.
- Handlers can't draw. Drawing happens every frame, in views.
- Events waiting to be handled are world state: a snapshot holds them, and re-simulating a tick sends and handles them again the same way.
- **Built-in events** are declared like any other, and cost nothing where no handler takes them:
  - `Spawned` and `Destroyed` are sent to an entity when it's spawned or destroyed. `Destroyed` handlers run while its components can still be read.
  - `PlayerJoined` and `PlayerLeft` are sent to the world when a player joins or leaves, with the player's `PlayerID` in `player`. The server picks the tick, so every machine handles them on the same one.

```csharp
event Hit
{
    Entity attacker;
    int damage;
}

system Explode(Entity self, Bomb bomb)
{
    if (bomb.timer > 0) return;
    bomb.target.Send(Hit { attacker = bomb.owner, damage = 50 });
    self.Destroy();
}

// Health and target come from the entity the Hit was sent to.
event(Hit hit) TakeHit(Entity target, mut Health health)
{
    health.value -= hit.damage;
    if (health.value <= 0) target.Destroy();
}

// Takes nothing from the entity, so it runs once per Hit.
event(Hit hit) CountHits(mut Stats stats)
{
    stats.hits += 1;
}

// Spawned has no fields, so it needs no name.
event(Spawned) Arm(Entity player, with Player)
{
    Spawn(Weapon { owner = player });
}
```

### Provisional

- `event` is only a keyword at the start of a declaration, like `input`.
- A trigger's name is optional for any event: a handler that doesn't read the event leaves it out.
- `Spawned` handlers run as the spawn is applied, and `Destroyed` handlers just before the entity goes, both in the queue's order. A spawn's `Spawned` handlers run before the next change in the queue.
- Structural changes and events share one queue per tick, `PURR_MAX_COMMANDS` long (4096 by default). What handlers record goes on its end, so an endless chain of events fills it, and the program stops with a message naming the define to raise.
- Events can be locals (`var h = hit;`) and can be sent on (`other.Send(hit)`), but they can't be fields, function parameters or system parameters.
- `[Before]` and `[After]` only order handlers of the same event. Handlers and systems are ordered separately.
- Games can't send the built-in events. The host sends `PlayerJoined` and `PlayerLeft` with `purr_world_player_joined` and `purr_world_player_left`: they're handled at the end of the next tick, before anything that tick sends. `purr/run.h` has player 0 join before the first tick.
- A handler of an event nothing sends is a warning, and so is declared access a handler doesn't use, as for systems.
- `purrc --schedule` lists each event's handlers in the order they run.

### Open

- Events for a component being added or removed.
- Local handlers, such as playing a sound when a `Hit` happens (`local event(Hit hit) PlayHitSound()`). They have to run once even when rollback re-runs the tick that sent the event, and choose between predicted and verified ticks.
- Handling a world event for every matching entity, such as resetting every player on `RoundOver`. It waits for loops over queries.

## Enums and switch

### Decided

- Enums and `switch` follow C#'s syntax.

```csharp
enum Phase
{
    Warmup,
    Playing = 5,
    Over,
}

system Advance(mut Match match)
{
    switch (match.phase)
    {
        case Phase.Warmup:
            match.phase = Phase.Playing;
            break;
        case Phase.Playing:
        case Phase.Over:
            break;
    }
}
```

### Provisional

- `enum Name { A, B = 5, C }` declares an enum. A member without a value is one more than the one before it, and the first is 0. A value is an int literal, and a comma after the last member is fine. `enum` is only a keyword at the start of a declaration.
- Members are always written with their enum: `Phase.Playing`, or `Game.Phase.Playing` from another namespace. In generated C, `Phase.Playing` is `Phase_Playing`, a constant of the type `Phase`, an `int32_t`.
- Enums are values, like structs: fields, locals, inputs, and functions' parameters and return values can hold them. A field without a default starts at 0, even if no member has that value, as in C#.
- `==` and `!=` compare two values of the same enum. `int(phase)` gives a member's value; there's no way from an int to an enum yet.
- An enum in an input that isn't one of its members, which only a bad client could send, becomes the field's default before `Sanitize`, like a NaN float.
- `switch` works on ints and enums. A case is an int literal or one of the enum's members. Labels in a row share a section, `default` handles the rest, and each value appears once.
- Every section ends with `break;` or `return;` on every path, so none runs into the next, as in C#. `break` anywhere else is an error, since there are no loops yet.
- Each section has its own scope for locals.
- A function returns on every path when a switch with a `default` returns in every section.
- `switch`, `case`, `default` and `break` are keywords.

### Open

- Ordering enums (`<`), and turning an int into an enum.
- A warning when a switch on an enum has no `default` and misses a member.

## Input

### Decided

- One `input` declaration per game describes one player's input for one tick. It's what the network sends.
- Input is one value per player per tick. The engine writes it, and the simulation can only read it.
- `Owner` is a built-in component that ties an entity to a player. It's a normal component: it can be read, written, added and removed. Writing it hands control over.
- Players are identified by a built-in `PlayerID` type, not an `int`. Like `Entity`, it's opaque, comparable with `==`, and has a null value. The simulation only sees `PlayerID`s and never connections, so a player who reconnects and gets their `PlayerID` back (PurrNet style) keeps everything they owned. `PlayerID(0)` names a player by index, for local play and tests.
- **Sampling is PurrLang code:** the input's `Sample(Devices devices)` method. The engine calls it on the client once per tick. Fields start at their defaults, and `Sample` assigns them by name, without `mut`. It runs outside the simulation: it can read `Devices` but not components or singletons.
- **Input carries whether buttons are held, not whether they just went down,** because a missing remote input is guessed by repeating the last one. On a device, `.pressed` means down at any point since the last sample, so a quick tap between ticks is never lost. In the simulation, `bool` input fields get `.down` and `.up`, computed against the previous tick, inside structs too (`input.aim.fire.down`).
- An input parameter in a system gives the input of the player who owns the entity. Entities without an `Owner`, or whose owner isn't a known player, get the **server's input**, and so do systems that run once per tick. This is how the server controls what no player owns (PurrNet style). An input parameter doesn't filter entities: add `with Owner` to only run on owned ones.
- An input can have a `Sanitize()` method. Every input passes through it before the simulation reads it, including input from other players, so systems can rely on what it guarantees without checking again. It assigns the input's fields by name, like `Sample`, and reads nothing else.
- Input fields can declare bounds: `[Clamp(lo, hi)]`, `[Min(x)]` and `[Max(x)]`, and so can the fields of structs an input holds. The engine applies them to every input before `Sanitize`, so `Sanitize` only handles what they can't express. Bounds are constants; a number bounds every component of a vector.
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
- Views never change the match. They can change local state (see Local state).

### Provisional

- `view` declares a view. It looks like a system and takes the same parameters, but it runs once per rendered frame instead of once per tick. Its `mut` parameters, `Spawn`, `Add`, `Remove`, `Destroy` and `Send` are local (see Local state); input parameters are errors in a view.
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
- Text with values in it, such as C#'s `$"score {score}"`.
- 3D drawing, sprites and textures, layers.

## GUI

### Decided

- The GUI is immediate mode, called from views, and drawn over the world. It follows Unity's IMGUI.
- `GUILayout` lays widgets out automatically: they stack top to bottom, and `GUILayout.Horizontal()` puts them side by side. `GUI` has the same widgets, each at an explicit `Rect`, which comes first, as in Unity: `GUI.Button(rect, "Quit")`.
- Containers take a block, and they're ordinary functions with a `Block` parameter (see Functions): `GUILayout.Horizontal() { ... }`, `GUILayout.Vertical() { ... }` and `GUILayout.Area(...) { ... }`. Calls always have parentheses.
- Widgets edit values through `mut` parameters, and return whether the value changed: `GUILayout.Toggle("Fullscreen", settings.fullscreen)`. A button returns whether it was pressed.
- Widgets have no IDs to write. They're told apart by where they're called from and the entity the view runs for.
- The engine builds nothing a game couldn't build itself: containers are functions with a `Block`, and widgets are made of pieces games can use too.
- Gamepad and keyboard navigation are built in: focus moves between widgets, the south button presses, the east button goes back.
- Whatever the GUI is using, such as a click on a button or typing in a field, is hidden from the input's `Sample`.
- Typing into a field uses the characters the player types, which follow their keyboard layout, not keys by position.
- `Screen.width`, `Screen.height` and `Screen.scale` describe the window.
- The widgets: `Label`, `Button`, `Toggle`, `Slider`, `IntSlider`, `TextField`, `IntField`, `FloatField`, `Float2Field`, `Float3Field`, `Float4Field`, `ColorField` and `Space`, and the containers `Horizontal`, `Vertical` and `Area`.

```csharp
local singleton Settings
{
    bool open;
    bool fullscreen;
    float volume = 1;
}

view Options(mut Settings settings)
{
    if (GUI.Button(Rect(Screen.width - 210, 10, 200, 40), "Options")) settings.open = !settings.open;
    if (!settings.open) return;

    GUILayout.Area(Anchor.MiddleCenter)
    {
        GUILayout.Toggle("Fullscreen", settings.fullscreen);
        GUILayout.Slider("Volume", settings.volume, 0, 1);
    }
}
```

### Provisional

Claude's picks, not yet approved or implemented:

- Positions and sizes are in units of a screen 1080 units tall, whose width follows the window's shape, so a GUI laid out once fits every window. `Screen.width` and `Screen.height` are in those units, and `Screen.scale` is pixels per unit.
- `Rect(x, y, width, height)` is measured from the top left corner, with `y` down, as in Unity's GUI. World drawing and the mouse have `y` up.
- `GUILayout.Area(anchor)` places an area sized to its content at one of nine anchors, named as Unity's `TextAnchor` (`UpperLeft` to `LowerRight`). `GUILayout.Area(rect)` places it at a rect.
- The pieces widgets are made of: a control's ID, which the compiler derives from the call as for the built-in widgets, and whether that control is hovered, pressed or focused.

### Open

- A field for any enum. A game couldn't write one itself until there are generics.
- Styles and themes.
- More than one block per function, like Swift's labelled trailing closures.
- `TextField` waits for strings, and labels showing values wait for text with values in it.

## Local state

### Decided

- The world holds a match: the state every machine simulates the same way. It only exists while a match is on. Single-player is a match too, on a server the machine runs itself and reaches through a loopback transport, the same path as any other connection.
- Local state belongs to one machine and lives outside every world: menus, settings, connection status, animation timers, particles. It's never sent, rolled back or hashed.
- `local` in front of a declaration makes it local: `local singleton`, `local component`, `local scene` and `local event`. Everything else belongs to the match. Structs and functions belong to neither, and both sides use them.
- A local singleton exists once per machine, for the whole program. Local components make local entities.
- Views run every frame, in a match or not. They read the match, and read and write local state: `mut` local parameters, and `Spawn`, `Add`, `Remove` and `Destroy` of local entities. A view only runs when everything it reads exists, so a view of match components or singletons doesn't run outside a match.
- Local structural changes and local events are handled at the end of the frame, as the match's are at the end of the tick.
- The GUI is immediate mode, drawn from views (see GUI).
- **The compiler enforces the boundary,** so no mistake can reach a running game:
  - Match code (systems, match event handlers, match scenes) can't read or write anything local, and can't read `Devices`. The input's `Sample` reads them, and writes only the input.
  - Local code (views, local event handlers, local scenes) can read the match but never change it: no `mut` on match components or singletons, and no spawning, changing or sending match things.
  - The only way from local code into the match is input. Starting, joining and leaving a match are session calls, which never touch a running match.
  - Errors say where to go instead: "a view can't change the match: put it in the input and handle it in a system."

```csharp
local component Spark
{
    float2 position;
    int framesLeft = 30;
}

// Reads the match and leaves a trail of local sparks behind every ball.
view Trail(Body body, with Ball)
{
    Spawn(Spark { position = body.position });
}

view DrawSparks(Entity self, mut Spark spark)
{
    Draw.Circle(spark.position, 0.1, Color.yellow);
    spark.framesLeft -= 1;
    if (spark.framesLeft <= 0) self.Destroy();
}

// Error: match code can't read local state.
system Count(Spark spark) { }
```

### Provisional

- `local` is only a keyword at the start of a declaration. It goes before components, singletons, events and event handlers; before anything else it's an error that says why (structs and functions belong to neither side, views are always local, systems run the match).
- **`LocalEntity`** is an entity of the local world. `Spawn` in local code returns one, and a view of local components takes `LocalEntity`, where a view of match components takes `Entity`. Local code can hold and read an `Entity` (the unit a player selected, say) but never change one. The match's declarations can't hold a `LocalEntity`, and neither can structs, which both sides share.
- A view runs for the entities of one world: its components are all local or all the match's.
- Local handlers handle local events, and `Spawned` and `Destroyed` of local entities. They only take local state for now.
- The host keeps the local state and calls `purr_local_init(local)` once, then `purr_frame(w, local, draw)` every frame. `purr_frame` runs the views, then applies their local changes and events. Outside a match, `w` is NULL, and views that read the match don't run.
- The local world has its own entity table and queue, the same size as the match's.

### Open

- The session API: starting, hosting, joining and leaving a match, and the connection's status.
- Local handlers of match events (see Events).
- Time for local code, such as the frame's length.
- Saving local state, such as settings, between runs.

## Scenes

### Decided

- A scene is a group of entities that load and unload together: a menu, a level, an arena. Several scenes can be loaded at once, and several copies of the same one.
- `scene Arena { ... }` declares one. A loaded scene is an entity like any other: the declaration is its component, and its fields are the scene's state. A system that takes `Arena` runs once per loaded arena.
- `local scene` declares a local one (see Local state). A scene only holds entities of its own side.
- `Scene.Load(Arena { size = 30 })` loads a scene into the current world and returns its entity. `Scene.Unload(scene)` unloads it, destroying every entity it owns. `Spawn` of a scene is an error that says to use `Scene.Load`.
- Loading and unloading happen at the end of the tick, like `Spawn` and `Destroy`. `Spawned` handlers set a scene up, and its entities get `Destroyed` when it unloads, both within that tick (see Events).
- **Ownership:** a spawn joins the scene of the entity the code runs for. In a handler, that's the entity the event was sent to. Code that isn't running for an entity, like a system that runs once per tick, spawns into no scene, and those entities live until they're destroyed.
- A scene is never owned: it lives until it's unloaded, whoever loaded it.
- Loaded scenes share their world: the same systems, singletons and `Time`. Scenes that share nothing are separate worlds, which never communicate. Only the server creates worlds, so match code can't (see Open).
- **Visibility:** scenes are public by default, seen by every player in the world. `Scene.Load(Hand { ... }, SceneVisibility.Private)` loads a private one, which only the server and the players given it see: `Scene.AddPlayer(scene, player)` and `Scene.RemovePlayer(scene, player)`. Membership is match state, so the server decides it, and a player who's added receives the scene's state.
- A private scene with no players exists only on the server, which is where secrets like RNG seeds go. Code that reads a private scene only predicts correctly on machines that see it, and the server corrects the others.
- A client never loads match scenes on its own: it has the ones the server has it in.
- `system Main()` is an error that says to write `scene Main`.

```csharp
scene Arena
{
    int size = 20;
}

scene Hand
{
    PlayerID player;
}

// Sets up each arena. The floor joins that arena.
event(Spawned) SetupArena(Arena arena)
{
    Spawn(Floor { size = arena.size });
}

// Deals each player a hand only they can see.
event(PlayerJoined joined) DealIn()
{
    var hand = Scene.Load(Hand { player = joined.player }, SceneVisibility.Private);
    Scene.AddPlayer(hand, joined.player);
}

system Collapse(Entity self, Arena arena)
{
    if (arena.size <= 0) Scene.Unload(self);
}
```

### Provisional

- `scene` and `local scene` are only keywords at the start of a declaration. A scene's component can have methods, and `Add` can give its entity other components, like any entity.
- `Spawn`, `Add` and `Remove` of a scene's component are errors that point to `Scene.Load` and `Scene.Unload`. `Destroy` on a scene's entity unloads it, and `Scene.Unload` of an entity that isn't a scene does nothing.
- `Scene.Load` makes its entity right away, like `Spawn`, so it follows the same evaluation order and can't be inside `?:` or on the right of `&&` and `||`.
- An entity spawned into a scene that's been unloaded by the spawn's turn is never made, like the loot an enemy drops while its arena unloads.
- Entities unload in archetype order, each archetype's from its last row, and the scene's own entity last. Each gets its `Destroyed` handlers.
- `SceneVisibility` is a built-in enum, `Public` and `Private`. A local scene has no visibility. Local code can't change who sees a scene.
- In generated C, a scene's component holds its visibility and players too, as `purr_visibility` and `purr_players`, one bit per player. Code can't name them.
- The engine loads `Main` itself: `purr_world_init` loads a match `Main`, and `purr_local_init` a local one, with `PURR_MAIN_IS_LOCAL` defined. `purr/run.h` starts without a match when `Main` is local.
- The `Main` scene is the world's first entity.

### Open

- The server's own code: creating worlds from scenes, and moving players between them.
- Entity references that say what they point to, so the compiler can check `Scene.Unload` on an entity read from a field. It can already check one that comes from a system taking the scene's component, as in `Collapse`.
- The details of private scenes: what players outside one see of it, and how an added player catches up.

## Open

- How entities authored as data (levels, prefabs) feed into archetype derivation.
- Archetype growth. Every `Add` and `Remove` can apply to any entity, so the compiler assumes every combination is reachable, and each archetype currently reserves a fixed 1024 slots. Narrowing this safely needs more analysis, and storage should grow on demand.
- How modules, such as the engine's built-in systems, initialize when there's a single `Main`.
- Groups of systems (phases such as input, simulation, late), which Before and After could order as a whole.
- Access control: whether a namespace can keep declarations to itself.
