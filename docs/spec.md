# The Tide language

The language Tide games and the engine's built-in systems are written in. It transpiles to C (see AGENTS.md). This file is the source of truth for the language: only decisions the owner has approved go in **Decided**. Proposals and open questions go in **Open**.

## Decided

### Files and tools

- Source files use the `.tide` extension.
- The compiler (transpiler) is `tidec`.
- A game is one or more `.tide` files: by default, every `.tide` file in the game's folder and its subfolders, and those of the packages it lists (see Packages). Every declaration is visible from every file of the game; there are no imports between files.
- A game needs no C: the engine runs it. A custom C host is optional, for tests or special hosts. A game can call C of its own, from `.c` files and libraries in its folder (see C functions).

### Namespaces

- A file can put its declarations in a namespace, and namespaces are how large games keep names apart.
- Code outside the namespace names its declarations with it (`Combat.Health`), or imports it with `using Combat;`.

### Order of systems

- Systems run in one deterministic order, the same on every platform. By default it follows the files, sorted by path, then the order of declarations in each file.
- Attributes change the order of a system relative to others.

### Style

- The syntax is C#-like.
- Types, systems and methods use PascalCase: `Transform`, `MovePlayer`, `Spawn(...)`.
- Fields, parameters and locals use camelCase: `trs.position`, not `trs.Position`.
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
- `extern` declares a function written in C (see C functions).
- `const` declares a constant (see Constants).
- `settings` sets the engine's settings for the game (see Settings).

### Field defaults

- Fields of components, singletons, inputs and structs can declare a default value: `int value = 100;`.
- A default must be a constant expression: literals, constants, constructors of built-in types, struct values of constants (`Range { hi = 5 }`), `Math` functions, built-in constants like `quaternion.identity`, and operators, as in `float angle = Math.Radians(45);`. It can't read fields, singletons or `Time`.
- Singletons start with their defaults when the world is created, before `Main` runs.
- Components get their defaults whenever a value is created without setting that field: `Spawn(Health)`, `e.Add(Health)`, and fields left out of `Health { max = 200 }`.
- Fields without a default start at zero. `Entity` fields always start as the null entity and can't have another default.

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

### Default values

- `default` is the default value of the type where it goes, as in C#: `Draw.Circle(default, 5, Color.yellow)` draws at `float2(0, 0)`.
- It's the value a field of that type starts at when it declares no default: zero for numbers, vectors, matrices, quaternions, colors and rects, `false`, empty text, an empty list, the null entity, no player, an enum's 0, and for a struct or component, its fields' defaults, as `Stats { }` makes them.
- Its type comes from where it goes: a typed local (`float2 center = default;`), an assignment, an argument, a `return`, a field in a value (`Range { lo = default }`), a field's default, a list's element, and the other side of `==`, `!=` or `?:` (`target == default`).

### Structs

- A struct is plain data, copied when it's assigned, with no references. The world stays plain data, so copying it is still a snapshot.
- Structs are the types of fields (of components, singletons, inputs and other structs) and of locals. System parameters stay components, singletons, the input and `Devices`.
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
- A component's method has `this`: the entity whose component it's called on. A method that uses it, itself or through another of its methods, can only be called on a component the code runs for, a parameter of a system, view or handler: a copy belongs to no entity. A struct's methods and `Interpolate` have no `this`.
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
- A function's last parameter can be an `Action`: code the caller writes in braces after the call, `Section("Audio") { ... }`. A function takes at most one, always last, and it's always written after the call, never inside the parentheses.
- The function runs the block by calling it, `content();`, as many times as it chooses, including none. The block runs as if it were written at the call: it sees the caller's locals and parameters, and what it reads and writes counts toward the caller's signature.
- A function that takes an `Action` is inlined where it's called, so blocks cost nothing and need no closures. It can't call itself, directly or through other functions, and a block can only be run, not stored.

```csharp
// A container of your own: runs its content only while open.
void Foldout(string title, mut bool open, Action content)
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

- `this` is the entity the code runs for: `if (health.value <= 0) this.Destroy();`. It's an `Entity`, or a `LocalEntity` in code that runs for local entities. `this` is a keyword everywhere, and it can't be assigned.
- Only code that runs once per entity has `this`: a system, view or handler that takes a component or filters by one (`with Player`). Elsewhere it's an error that says why.
- The entity isn't a parameter: an `Entity` or `LocalEntity` parameter is an error that points to `this`.

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

- Expressions evaluate left to right, like C#: operands, arguments and field initializers run in source order. `Spawn` and calls to C are the only expressions with side effects today, so this is what makes entity IDs, and what C keeps, come out the same on every platform. `Spawn(Pair { a = Spawn(Thing), b = Spawn(Thing) })` spawns `a`'s Thing, then `b`'s, then the Pair.
- `cond ? a : b` runs the condition first, then only the side it picks, as in C#.

### Archetypes

- There are no archetype declarations. The compiler derives every archetype from the code: the component set at each spawn site, plus every combination reachable through adding and removing components.

## Provisional: implemented in v0, awaiting approval

tidec v0 needed answers to these to work end to end. They're implemented, but the owner hasn't approved them yet, so any of them can change.

### Types and values

- Built-in scalar types are `bool`, `int` (32-bit), `float` (32-bit), `Entity` and `LocalEntity` (see Local state). There is no `double`. Vector types are under Vector math, `Color` under Views and drawing, `string` under Text and `List<T>` under Lists.
- `1.5` is a `float`; the `f` suffix is optional. An `int` converts to `float` implicitly, never the other way.
- In `cond ? a : b`, the condition is a `bool`, and the two sides have the same type or one converts to the other's, as in C#: `ready ? 1 : 0.5` is a `float`. It binds looser than every binary operator and groups to the right.
- Integer arithmetic wraps on overflow. Integer division and modulo by zero give 0, so no input can crash the simulation.
- Bitwise operators `& | ^ ~ << >>` work on `int`, with compound forms `&= |= ^= <<= >>=`. As in C#, shift counts use their low 5 bits (`1 << 33` is `2`) and `>>` keeps the sign. Operator precedence follows C#.
- Integer literals can be decimal (`255`), hex (`0xFF`) or binary (`0b1010`). Decimal goes up to 2147483647. Hex and binary can use all 32 bits and are read as the int's bit pattern: `0xFFFFFFFF` is `-1` and `0x80000000` is the lowest int.
- `_` separates digits in any number, as in C#: `1_000_000`, `0b1111_0000`, `0x_FF_FF`. It's allowed between digits and right after `0x` or `0b`, but not at the start or end or next to `.`.
- Local variables need an initial value.
- `default` of a struct or component is its fields' defaults, not all zeros as in C#, where `default` skips field initializers: a Tide value never exists without them.
- As in C#, `default` is only compared: `default + 1` is an error. So is `var x = default;`, which says no type, and a place where nothing else does, like `if (default)`.
- In a built-in call, the other arguments pick the version, and `default` takes its parameter's type: `Math.Max(default, 1.5)` is a float. A call whose versions take different types there is an error, like `Math.Mul(q, default)` (a quaternion or a float3), and so is a `Math` function whose arguments are all `default`. Constructors like `float3(...)` don't take it.

### Systems

- A system with no component parameters runs once per tick. Otherwise it runs once per matching entity.
- A parameter that's never used, or declared `mut` and never written, is a warning: it makes other systems wait for nothing. A component that's only there to filter entities belongs in `with`.
- `return;` ends the system for the current entity.
- Locals can't reuse the name of another local or parameter in scope.
- Names starting with `tide_` are reserved for generated code.

### Entities

- `e.Destroy()` destroys an entity. It's deferred like other structural changes.
- The "fixed point" where structural changes apply is the end of each tick (and the end of `Main`). Changes apply in the order they were recorded.
- `Add`, `Remove` and `Destroy` on an entity that was already destroyed do nothing.
- In a system that splits its entities across threads, `Spawn` returns a temporary handle: the entity gets its ID once the system is done, in the order one thread would have given them, and the handles the system kept in the components it changes, the changes it recorded and its events become the real one, before any system that waits for it starts. So such systems don't wait for each other to spawn. Until then, text shows the handle as `Entity(new)`, and C can tell with `tide_entity_is_temporary`.
- `Spawn` can't appear on the right side of `&&` or `||`, or in a side of `?:`. Those parts only run sometimes, while spawns run first, in order (see Evaluation order). Spawn into a local before the condition, or use `if`/`else`.

### Built-ins

- `Time` is a built-in singleton with `float dt` (the fixed tick length) and `int tick` (starts at 0). Systems can read it but not write it.

### Syntax

- Statements: blocks, `if`/`else`, `switch`, loops (see Loops), `break;`, `continue;`, `return;`, `fail error;` (see Errors), local declarations, assignments (`= += -= *= /= %= <<= >>= &= |= ^=`), `i++` and `i--`, and calls: of `Spawn`, `Add`, `Remove`, `Destroy`, `Send`, the `Draw` and GUI functions, lists' methods, and methods and functions, with a block after the ones that take one, and with `!` after or `try` before the ones that can fail.

### Functions and blocks

- A call with a block after it is a statement, and the block's braces go on lines of their own, like any other's. A function that takes an Action returns nothing, since its call is a statement: it changes what its caller passes as `mut` instead.
- Only functions take an Action, not methods, and an Action is never `mut`. An Action can't be stored in a field or a local.
- In the block, `return` ends the caller, as if the block were written there, and `break` ends the caller's switch, even if the function runs the block inside a switch of its own. `return` in the function's own code ends the function.
- A function that takes an Action is copied into each call in generated C, with its locals renamed, so its names never hide the caller's in the block.
- A `mut string` parameter is the caller's text, a local's or a field's, which the function changes.
- Operators and their precedence follow C#. Comments are `//` and `/* */`.
- Source files are UTF-8, and a byte order mark at the start is skipped. Names are ASCII: other characters only go in comments and text.

### Limits

- 64 components and 256 archetypes. Everything else grows as it needs, with no limit but memory: entities, entities per archetype, and structural changes and events per tick.

### Namespaces

- `namespace Game.Combat;` at the top of a file puts everything in the file in that namespace. A file has at most one namespace; files without one are in the global namespace. Namespaces can be dotted.
- `using Physics;` at the top of a file lets it name Physics' declarations without the prefix. `namespace` and `using` come before any declaration.
- A plain name is looked up in the file's namespace, then the namespaces around it, then the `using` namespaces, then the global namespace. If two `using` namespaces both have it, it's ambiguous: write the namespace.
- Qualified names work wherever a type is named: parameters (`mut Combat.Health health`), `with` and `without`, component values (`Combat.Health { value = 10 }`), `Spawn`, `Add` and `Remove`.
- The same name can be declared in different namespaces. Built-in names can't start a namespace (`namespace Math;` is an error).
- In generated C, namespaced declarations are prefixed with their namespace: `Combat.Health` is `Combat_Health`, read with `tide_get_Combat_Health`.
- There's exactly one `Main` in a game, in any file or namespace. `namespace` and `using` are only keywords at the top of a file.

```csharp
// combat.tide
namespace Combat;

component Health { int value = 100; }

// main.tide
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
- Systems that share no data they write can run at the same time; the others wait, in this order. Above each system, the language server shows its stage and why it waits ("stage 2 · after Move: both write Body"), and `tidec --schedule` prints the whole plan.

```csharp
[After(Physics.Gravity)]
system Move(mut Body body) { ... }
```

## Vector math

Follows Unity.Mathematics, with Tide's naming. Everything in this section is decided.

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
- Quaternions: `Mul` (two rotations, or a rotation and a `float3`, which it rotates), `Rotate`, `Inverse`, `Conjugate`, `Normalize`, `NormalizeSafe`, `Dot`, `Slerp`, `Nlerp`, `Forward`, `Up`, `Right`, `Angle`.
- Matrices: `Mul` (two matrices, or a matrix and a vector of its size), `Transpose`, `Inverse`, `Determinant`, and for `float4x4`, `Transform` (a point) and `Rotate` (a direction).
- Everything is deterministic (see AGENTS.md). The transcendental functions are Tide's own, accurate to about 1 ulp but not correctly rounded.
- `Math.Hash(value)` on an `int`, `int2`, `int3` or `int4` gives random numbers that keep no state: the same int for the same value on every machine, like `Math.Hash(int3(x, y, seed)) % 6`.

### Provisional

Implemented, awaiting approval:

- `Math.Hash` gives an int from 0 to 2147483647, so `%` never goes negative. It's xxHash32 of the value's bytes (little-endian, seed 0) without the top bit: what Unity's `math.hash` gives for a block of memory. Unity's `math.hash` on vectors is a cheaper mix whose low bits follow the input's, so `& 3` of it makes stripes; Tide's differs from it on purpose.

## Constants

### Decided

- `const` declares a constant at the top level of a file: `const int STARTING_LIVES = 3;`. The type is written out, as a field's is, and the value is a constant expression, by the rules of field defaults, which can name other constants.
- Constants are named in FULL_CASE, like `Math.PI`.
- A file's namespace is its constants' too: `Combat.CRIT_MULTIPLIER` from outside it, or `CRIT_MULTIPLIER` with `using Combat;`.
- Constants are part of the build, so every machine has the same values. Any code reads them (match code, local code, views and `Sample`), and reading one makes no system wait, as reading `Math.PI` doesn't.
- They go wherever a constant does: field defaults, the bounds of `[Clamp]`, `[Min]` and `[Max]`, settings, and other constants.
- Changing one under `tide run` reloads the game and keeps the match where it is: constants aren't part of its data layout.
- A game's own values are constants. `settings` only holds the engine's (see Settings).

```csharp
const int MAX_HEALTH = 100;
const float REGEN_PER_SECOND = MAX_HEALTH / 20.0;

component Health
{
    float value = MAX_HEALTH;
}

system Regenerate(mut Health health, Time time)
{
    health.value = Math.Min(health.value + REGEN_PER_SECOND * time.dt, MAX_HEALTH);
}
```

### Provisional

Implemented, awaiting approval:

- `const` is a keyword only at the start of a top-level declaration. One inside code, or inside a type, is an error that points to the top of the file; a local is read-only already.
- A constant is a number, vector, matrix, quaternion, `bool`, `Color`, `Rect`, `PlayerID`, text, struct or enum: not a list, nor a struct that holds one, yet, and not an entity, which could only ever be the null one.
- `case` labels and enum members' values can use int constants and operators on them (`case MAX_LEVEL + 1:`). The compiler works them out as they would be at run time: wrapping on overflow, and 0 for a division by zero.
- Constants can name each other in any order, across files, but not in a circle.
- A local, parameter or field in scope hides a constant of the same name, and a constant's name can't be another declaration's in its namespace.
- In generated C, a constant is its value, written wherever it's read: it has no C name, and nothing of it is in the header.

## Events

### Decided

- An event is something that happened, sent from one part of the game to the rest: a hit, a pickup, a round ending. `event Hit { ... }` declares one, with fields like a struct's, which carry its context.
- `event(Hit hit) TakeHit(...)` declares a handler: code that runs when a `Hit` is sent. The same keyword declares both, and `event(` starts a handler. The first parentheses hold the trigger, exactly one event. The second list takes the same parameters as a system.
- An event with no fields needs no name in the trigger: `event(Spawned) Arm(...)`.
- `entity.Send(Hit { ... })` sends an event to an entity. `Send(RoundOver { ... })` sends it to the whole world.
- A handler's components and `this` come from the entity the event was sent to, as a system's come from the entity it runs for. If that entity doesn't match the handler's parameters, the handler doesn't run for it.
- A handler that takes components needs that entity, so every `Send` of its event must name one. The compiler checks every `Send`: `Send(Hit { ... })` is an error when a `Hit` handler needs an entity, and says to write `entity.Send(...)`.
- A handler that takes no components runs once per event, whether it was sent to an entity or not, and has no `this`.
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

system Explode(Bomb bomb)
{
    if (bomb.timer > 0) return;
    bomb.target.Send(Hit { attacker = bomb.owner, damage = 50 });
    this.Destroy();
}

// Health and this come from the entity the Hit was sent to.
event(Hit hit) TakeHit(mut Health health)
{
    health.value -= hit.damage;
    if (health.value <= 0) this.Destroy();
}

// Takes nothing from the entity, so it runs once per Hit.
event(Hit hit) CountHits(mut Stats stats)
{
    stats.hits += 1;
}

// Spawned has no fields, so it needs no name.
event(Spawned) Arm(with Player)
{
    Spawn(Weapon { owner = this });
}
```

### Provisional

- `event` is only a keyword at the start of a declaration, like `input`.
- A trigger's name is optional for any event: a handler that doesn't read the event leaves it out.
- `Spawned` handlers run as the spawn is applied, and `Destroyed` handlers just before the entity goes, both in the queue's order. A spawn's `Spawned` handlers run before the next change in the queue.
- Structural changes and events share one queue per tick, which grows as it needs. What handlers record goes on its end. A chain of events that never ends (handlers that set each other off) stops the program once it's 100,000 deep in one tick, each event sent or entity spawned by a handler of the one before, with a message naming the event. `TIDE_MAX_CHAIN` sets the depth for a game that needs deeper chains.
- Events can be locals (`var h = hit;`) and can be sent on (`other.Send(hit)`), but they can't be fields, function parameters or system parameters.
- `[Before]` and `[After]` only order handlers of the same event. Handlers and systems are ordered separately.
- Games can't send the built-in events. The host sends `PlayerJoined` and `PlayerLeft` with `tide_world_player_joined` and `tide_world_player_left`: they're handled at the end of the next tick, before anything that tick sends. `tide/run.h` has player 0 join before the first tick.
- A handler of an event nothing sends is a warning, and so is declared access a handler doesn't use, as for systems.
- `tidec --schedule` lists each event's handlers in the order they run.

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

- `enum Name { A, B = 5, C }` declares an enum. A member without a value is one more than the one before it, and the first is 0. A value is an int known while compiling: a literal, an int constant, or operators on them, worked out as they would be at run time. A comma after the last member is fine. `enum` is only a keyword at the start of a declaration.
- An enum is stored as an int, or as what it says after its name, as in C#: `enum Voxel : byte { ... }` takes one byte (0 to 255) and `: ushort` two (0 to 65535), for grids' cells and other data there's a lot of. A member out of that range is an error that gives the range. Anything else after the colon is an error that lists the three. Only the size changes: values still go through `int(...)`.
- Members are always written with their enum: `Phase.Playing`, or `Game.Phase.Playing` from another namespace. In generated C, `Phase.Playing` is `Phase_Playing`, a constant of the type `Phase`: an `int32_t`, or a `uint8_t` or `uint16_t` for `: byte` and `: ushort`.
- Enums are values, like structs: fields, locals, inputs, and functions' parameters and return values can hold them. A field without a default starts at 0, even if no member has that value, as in C#.
- `==` and `!=` compare two values of the same enum. `int(phase)` gives a member's value; there's no way from an int to an enum yet.
- An enum in an input that isn't one of its members, which only a bad client could send, becomes the field's default before `Sanitize`, like a NaN float.
- `switch` works on ints and enums. A case is an int known while compiling (a literal, an int constant, or operators on them), or one of the enum's members, maybe through a constant of the enum. Labels in a row share a section, `default` handles the rest, and each value appears once.
- Every section ends with `break;` or `return;` on every path, so none runs into the next, as in C#. Outside a switch, `break` ends a loop (see Loops); anywhere else it's an error.
- Each section has its own scope for locals.
- A function returns on every path when a switch with a `default` returns in every section.
- `switch`, `case`, `default` and `break` are keywords.

### Open

- Ordering enums (`<`), and turning an int into an enum.
- A warning when a switch on an enum has no `default` and misses a member.

## Loops

### Decided

- `for`, `foreach` and `while`, with `break` and `continue`, as in C#.

### Provisional

- `for (var i = 0; i < n; i++) { ... }`: its start, condition and step are each optional. The variable it declares changes in its step, and is read-only in its body unless it's declared `mut var`.
- `foreach (var item in list) { ... }` goes through a list's elements in order; `foreach (Type item in list)` names their type. Each element is a copy, read-only. The list's `count` is read each round, so elements added along the way are reached too. A foreach over a grid goes through its cells' places, and `parallel` through them at once (see Grids); `parallel` goes through a list's elements at once too, by index (see Lists).
- `i++`, `i--`, `++i` and `--i` are statements, on ints and floats, the same as `i += 1` and `i -= 1`. They aren't expressions.
- `break` ends the innermost loop or switch; `continue` goes on to the innermost loop's next round, from inside a switch too. In a block after a call, both are the caller's: they end or continue the caller's loop, even if the function runs the block inside a loop of its own.
- `Spawn`, `Scene.Load` and widgets can't be in a loop's condition or a for's step, which run again and again; they go in its body.
- `while (true)` and `for (;;)` with no `break` of their own never end, so nothing needs to follow them: a function can end with one, and so can a switch's section.
- `in` is only a keyword in a foreach or a parallel loop, and `by` and `offset` only in a parallel loop; `while`, `for`, `foreach`, `parallel` and `continue` are keywords everywhere.
- There's no `do ... while` yet.

## Text

### Decided

- `string` is text, as a value: assigning copies it, and nothing is ever shared.
- A string's `length` counts characters (Unicode code points), not bytes.
- Components, singletons, structs and events can hold text. A world keeps it in its heap, part of the world, so snapshots have it too. The heap grows as it needs, with no limit but memory.
- Nothing about text fails: past the end of a string, positions are clamped. Text that code makes stops growing when the scratch area is full.

### Provisional

- Text is UTF-8. Literals can hold any UTF-8 character, and `\"`, `\\` and `\n`.
- `$"score {score}"` puts values in text. After a value, a colon and a format, as in C#: `{x:F2}` for two decimals, `{n:D3}` for at least three digits (`007`), `{n:X}` for hex. Floats, vectors, quaternions, colors and rects take F, up to F9; ints and int vectors take D (up to D32), X (up to X8) and F. The letter can be lowercase. `{{` and `}}` are braces, and `?:` in a value goes in parentheses: `{(won ? 1 : 0)}`.
- Text can show numbers, bools, enums (their member's name), vectors and quaternions (`(1, 0.5)`), `Color` (`RGBA(1, 0, 0, 1)`), `Rect`, entities (`Entity(3:1)`, or `Entity(new)` for a temporary handle: see Entities) and players (`PlayerID(0)`). Floats are written with the fewest digits that read back as the same float, plainly from 1e-7 to 1e21 and with an exponent beyond (`1.5E+21`), the same on every platform: computed exactly, never with the platform's printf.
- Text shows values with fields (structs, components, singletons, events and inputs) as C# shows records: `Stats { hp = 3, speed = 1.5 }`, `Nothing { }`, nested ones inside. Lists show as `[1, 2, 3]`. Text in them is in quotes, `name = "Bob"`, so `""` shows; it isn't escaped. Scenes show their own fields, not the engine's. `Session` shows its `room` last. The device records (`Devices` and the rest) don't show, nor does anything holding a matrix, which text can't show yet; the error names what's in the way.
- `+` joins text with anything it can show: `"score " + score`, `1 + "st"`. `==` and `!=` compare text byte by byte. There's no `<` for text.
- `length`, and the methods `Contains`, `StartsWith`, `EndsWith`, `IndexOf` (-1 if it's not there), `Substring(start)` and `Substring(start, length)`, `ToUpper` and `ToLower` (ASCII letters only, for now), `Trim` and `Replace(from, to)`.
- Text that code makes, joining and formatting, lives in a scratch area that's cleared once the system, view or handler that made it is done, for each entity. It's only ever copied into a world's heap.
- Heap text never changes once it's written: changing a field writes new text. What's released only goes back once the code running is done, so a copy of a field made before it changed still reads the old text, with no copying.
- An input can't hold text: what players send each tick is numbers, bools and enums.
- `GUILayout.TextField(label, mut text)` and `GUI.TextField(rect, label, mut text)`: a field the player types into, which changes the text as they type. Enter or Escape stop typing, and Backspace takes off the last character.
- Systems that change the match's text or lists wait for each other, as their heap is one: `tidec --schedule` says "both change text or lists".

### Open

- A `char` type, and indexing text by character.
- Text with a caret that moves, and selection, in text fields.
- Case for letters past ASCII.

## Lists

### Decided

- `List<T>` is a list of values, as a value: assigning or passing one copies it, and changing a copy never changes the original.
- A world keeps its lists in its heap, like text.
- Nothing about lists fails: past the end, a read gives the element type's zero and a write does nothing. A world's lists grow with its heap, and a list that code makes stops growing when the scratch area is full.

### Provisional

- A list starts empty. `[a, b, c]` is a list where one goes, from what it goes into: `List<int> scores = [1, 2, 3];`, `scores = [];`, a field's default, an argument. `var x = [1, 2]` is an error, as it doesn't say the type.
- `count`, `items[i]` to read, `items[i] = x` and `items[i] += x` to write, and the methods `Add(item)`, `Insert(index, item)` (the index clamped), `RemoveAt(index)`, `Clear()`, and for elements that `==` compares (numbers, bools, enums, text, entities and players) `Contains(item)`, `IndexOf(item)` (-1 if it's not there) and `Remove(item)`, which returns whether it found one.
- An element is a copy, as with C#'s List of structs: `items[i].count = 1` is an error that says to take it out, change it and put it back. A `mut` parameter or a mut method can't change an element in place either.
- Changing a list needs it to be something that can change: a `mut` component or singleton, or a `mut` local.
- Elements are values: built-in types, text, enums and structs, not ECS data (keep an `Entity` instead), and not lists, or structs with lists in them, yet.
- Components, singletons, structs and events can hold lists; inputs can't.
- Taking a whole list out of a field or variable copies it; its elements, count, methods and `foreach` don't. A read-only list argument is passed without a copy unless a `mut` argument of the same call could change it.
- Lists in code (not in a world) live in the scratch area, like text.
- `parallel (var i in items) { ... }` goes through a list's elements at once, on threads, as it does a grid's cells (see Grids): its places are the elements' indices, so `i` is an `int`, and the step reads the element as `items[i]`. Each step reads the list as the loop found it and changes only its own element, `items[i]`, so the result is the same on any number of threads. `parallel (var i in items by 2 offset o)` gives each step a block of elements from `i` up to the block's size, which it changes as `items[i + 1]` and the like; blocks start at the offset plus any multiple of their size, and one that would go past the list's end is left out. The step's rules are a grid's: it changes nothing else (not the list's count either: no `Add`, `RemoveAt` or `Clear`), can't end the loop, wait, spawn or send, and so on.
- A parallel loop goes through a list the world keeps, a component's or a singleton's, not a variable's, and one whose elements hold no text: steps run on threads, where text can't be made.

### Open

- Lists of lists, dictionaries and sets.
- Sorting, and searching with a condition.
- Fixed-size arrays inside components, which need no heap.

## Grids

### Decided

- `Grid2<T>` and `Grid3<T>` hold cells at `int2` or `int3` positions. They're fields of components, singletons and scenes, so a world can have many: a dimension per scene, a canvas per player, a grid per ship.
- A grid keeps its cells in chunks, and a chunk only exists once a cell in it is set to something other than zero: everywhere else reads as zero, so an open grid costs what's in it, not the space it spans. A world and its snapshots share chunks until one of them changes one, and hashes cover each chunk on its own.
- A grid's size is given when it's made, `Grid2(1024, 1024)`. An axis given 0, or left out, is open: any int, negative ones too. Past its size, reads give zero and writes do nothing.
- `parallel (var at in cells) { ... }` goes through every cell of a grid at once, on threads. Each step reads the grid as the loop found it and changes only its own cell, so the result is the same on any number of threads, and the order the steps run in never shows. Code after the loop runs once every step is done.
- `parallel (var at in cells by 2 offset o) { ... }` goes through blocks instead: each step has the block of cells from `at` to `at + 1` on each axis (`by int2(2, 1)` sizes each axis), changes only those, and reads any cell. Blocks start at the offset, an int or an `int2`/`int3` given each time, and never overlap, so cells can move within a block: sand falls and slides in 2 by 2 blocks whose offset goes 0, 1, 0, 1, so each cell is in another block the tick after. A block that would go past a grid's size is left out, so along a sized edge, cells are only in the blocks of some offsets.
- `foreach (var at in cells) { ... }` goes through a grid's cells in order, in place, as C#'s would: rows from the first, each from its lowest x, and each step sees what the ones before it changed. When nothing a step does depends on the others (it changes only its own cell, and nothing outside the loop), its steps run at once instead, with the same result.
- `for` loops stay in order.
- There are no chunk systems: a system takes the grid's component or singleton like any other, and goes through the cells with a loop.

```csharp
singleton Field
{
    Grid2<int> cells = Grid2(512, 512);
}

// Ones fall a cell a tick, in blocks of a cell and the one above it, from
// even rows one tick and odd ones the next
system Fall(mut Field field, Time time)
{
    parallel (var at in field.cells by int2(1, 2) offset int2(0, time.tick % 2))
    {
        if (field.cells[at + int2(0, 1)] != 1 || field.cells[at] != 0) continue;
        field.cells[at] = 1;
        field.cells[at + int2(0, 1)] = 0;
    }
}
```

### Provisional

Implemented on the owner's go-ahead, to be revisited once games use them:

- Cells are plain values: numbers, bools, enums, vectors, quaternions, matrices, `Color`, `Rect`, `Entity`, `PlayerID`, and structs of those. Not text, lists or grids, which say to keep them elsewhere and a number for them in the cell.
- `cells[x, y]` is `cells[int2(x, y)]`, and `cells[x, y, z]` is `cells[int3(x, y, z)]`. `cells.size` is the grid's size, 0 on open axes. `cells.Clear()` sets every cell back to zero and keeps the size; `cells = Grid2(...)` gives the field a new, empty grid.
- A cell is a copy, like a list's element: `cells[p].heat = 1` is an error that says to take it out, change it and put it back. `cells[p] += 1` works on numbers. Setting a cell to zero where there's no chunk makes none.
- A grid is never copied, since every chunk would be: a local can't hold one, nor a component or singleton that has one, and a grid field is only assigned a new grid. Functions can't take or return grids yet. `Grid2(...)` takes its cell type from the field it goes in; anywhere else it's an error that says so.
- A chunk is the engine's: 4096 cells (64 by 64, or 16 by 16 by 16 in 3D), or for cells bigger than four bytes, as many as fit in a page (16 KiB), a power of two along each axis, the first axes the most.
- A loop over a grid with a size on every axis goes through every cell within it. An open axis goes on forever, so there a loop goes through the cells of the chunks the grid has, where something was set: in blocks, every block that takes in part of one. A foreach over an open grid goes through them in the same order, rows from the first, skipping what has no chunk.
- A parallel loop's step changes only its own cell or block, `at` plus constants (`at + int2(1, 0)`, `int2(at.x + 1, at.y)`), and the variables it declares. Anything else it changes is an error that says why: a variable outside the loop (to add up, use a `for` loop or a foreach that goes in order), another cell, the whole grid (`Clear`), through a `mut` argument too. It can't `break` out of the loop (`continue` ends the step), return, wait, start tasks, spawn, send, add, remove or destroy, load scenes, draw, use the GUI, or hold another parallel loop. It can call functions, and C.
- A block is 1 to 64 cells along each axis, a size known while compiling: a constant int, or `int2`/`int3`. The offset can be any int: blocks start at it plus any multiple of their size, so with blocks of 2, an offset of 2 gives the same blocks as 0.
- A parallel loop goes in a system, a view or an event handler that isn't async: not in functions, methods, async code or an input's `Sample`. A foreach there whose steps only touch their own cell runs at once; anywhere else, it goes in order.
- A foreach over a grid can't wait inside it: it goes through the cells as they are.
- Each step writes into a copy of its chunk, made from the chunk as it was, and the loop's end puts the copies into the grid in order, chunk by chunk, so where the steps ran never shows. The schedule says how many parallel loops each system has: `(once, 2 parallel loops)`. A tick with one runs on threads, and the loop's steps spread across them when there are enough to be worth it.
- Hosts read a world's cells with `tide_grid_read(&w->heap, w->Field.cells, x, y, 0, &tide_shape_Grid2_int)`: the generated header names each grid type's shape.
- Text can't show a grid.

### Open

- Generating chunks as they're first needed, keeping the regions around players loaded, and sending each player only theirs: an endless voxel world.
- Functions that take grids; parallel steps that spawn and send.
- Skipping what's settled: a parallel loop that only runs where something near changed (what `[Sleeps]` did for chunk systems).
- `for` loops that run at once when nothing a step does depends on the others.
- Resizing a grid; drawing one.

## Errors

### Decided

- Errors are values, never exceptions. An error is any type: usually an enum, a struct when it needs to carry more.
- A function says it can fail with `fails`, and `fail` ends it with an error, as `return` does with a value. A function with no value works the same: `void Open() fails OpenError`.
- The combined type, a value or an error, is never written: `var` holds it.
- At the call: `var score = ParseScore(text) ?? 0;` falls back to a value, `if (ParseScore(text) is int score) { ... }` runs only on success, `if (ParseScore(text) is ParseError why) { ... }` only on failure, and `var score = try ParseScore(text);` passes the error on to the caller.
- `try` only works inside a function that `fails` with the same error type. In a system it's an error, since systems have no caller: handling happens at the call.
- `T?` is for lookups where absence isn't an error: a value or nothing, unwrapped the same way (`??`, `is`).
- Postfix `!`, as in C#, after the expression (`ParseScore(t)!`), means: on failure, carry on with the type's default value (0 for int), never crash. It also silences the warning for ignoring an error.
- Using a failable call's value without unwrapping it is a compile error whose message says how to unwrap it (`??`, `is`, `!`, `try`). Calling a failable function as a statement and ignoring its error is a warning, which `!` silences.
- Forgiving defaults stay as they are for indexing, arithmetic and text. Asynchronous failures stay events (`Disconnected`).

```csharp
enum ParseError
{
    Empty,
    NotANumber,
}

int ParseScore(string text) fails ParseError
{
    if (text == "") fail ParseError.Empty;
    mut var score = 0;
    for (var i = 0; i < text.length; i++)
    {
        var digit = "0123456789".IndexOf(text.Substring(i, 1));
        if (digit < 0) fail ParseError.NotANumber;
        score = score * 10 + digit;
    }
    return score;
}

int Doubled(string text) fails ParseError
{
    var score = try ParseScore(text);
    return score * 2;
}

int? Find(List<int> items, int wanted)
{
    for (var i = 0; i < items.count; i++)
    {
        if (items[i] == wanted) return i;
    }
    return null;
}

system Score(mut Board board)
{
    board.points = ParseScore(board.typed) ?? 0;
    if (Doubled(board.typed) is ParseError.NotANumber) board.mistakes += 1;
    if (Find(board.picks, 3) is int at) board.last = at;
}
```

### Provisional

- `fail`, `try`, `is` and `null` are keywords everywhere; `fails` is one only right after a function's parameters.
- Methods can fail too. Operators, `Interpolate`, extern functions and functions that take an Action can't. Systems, views, handlers and the input's `Sample` and `Sanitize` can't fail, and can't `try`.
- An error's type is one a function can return. It can't be the type the function returns, which `is` couldn't tell apart, nor a `T?`, and a function that fails can't return a `T?` too.
- `try` binds like a unary operator, as C#'s `await` does: `try Parse(a) + 1` adds 1 to the value. It works on a variable that holds a result too, and as a statement: `try Open();`. It keeps its place in the evaluation order: what's before it in its statement runs first, and when it passes an error on, nothing after it runs. In a block written after a call, `try` and `fail` leave the function the block is written in, as `return` does. Leaving a function this way closes the GUI containers it opened.
- `??` binds and groups as in C#: looser than `||`, tighter than `?:`, to the right. Its right side only runs when the left fails or is nothing, so, like the right side of `&&` and `||`, it can't spawn, load scenes or draw widgets. The right side takes the value's type (`?? []`, `?? default`), an int widens to a float (`ParseScore(t) ?? 0.5` is a float), and it can be another failable call or `T?`, which is then what the whole gives: `a ?? b ?? 0`.
- `is` is relational, as in C#. After it goes the value's type (true when it succeeds, or a `T?` has one), the error's type (true when it fails), or one of an error enum's members, `is ParseError.Empty` (true when it fails with that one). Tide has no other type tests: `is` on a plain value is an error.
- A name after `is`'s type declares a read-only local. It goes in the condition of an `if`, `while` or `for`, alone or joined with `&&`, and is in scope in the rest of the condition and where it's true: the `if`'s body (not its `else`), or the loop's body and a for's step. Elsewhere `is` takes no name.
- `!` gives the type's default as `default` does: a struct's field defaults. A `T?` takes `!` too. `F()!;` and `try F();` are statements, for functions with no value.
- `var` holds a failable call's result or a `T?` as it is, and a `mut var` can take another of the same type. A result can't be passed, returned (that's `try`), shown in text or compared; it's unwrapped first.
- `T?` goes on functions' and methods' return types and parameters, and on locals: `int? best = null;`. Not on fields, in lists or inputs, or on system parameters yet. `null` and `default` are nothing, and a value converts to a `T?` by itself, as in C# (an int to a `float?` too). `x == null` and `x != null` tell whether it's nothing. C functions can't take or return one.
- The warning for ignoring an error is on a statement that calls a failable function; `try` and `!` handle it.
- Editors show a failable call's result as `int fails ParseError`, and offer `fail` and `try` in functions that fail.
- In generated C, a failable call's result and a `T?` are a small struct, `tide_result<n>`: the value, the error and whether it succeeded, plain data with no padding the compiler adds. A zeroed one is nothing. Results never go in the world, so snapshots never hold one.

### Open

- Built-in calls that fail, like `Session.Open()` when it can't take players: today Session calls are requests the host acts on after the frame, so their failures come later, as events.
- `is not`, as in C#'s `if (ParseScore(t) is not int score) return;`, which keeps the name in scope after the `if`.
- `T?` in fields, lists and inputs, and `??=`.
- `switch` on an error, and functions that take an Action failing.

## Tasks

### Decided

- `async` and `await`, as in C#. `async` before a function lets it wait with `await`, and pick up where it stopped. There's no `Task<T>`: the function's type is its value's, `async int Doubled(int x)`, and `await Doubled(3)` gives it.
- A call of an async function that isn't awaited starts a task, which goes on by itself.
- Tasks run in the match as well as in local code. A match task is part of the match: snapshots, hashes and rollback hold it, players who join get it, and it carries on when the match changes hands. Match code only waits for what's the same on every machine (ticks, time, events); a service's answer reaches the match through input.
- A task ends when whatever started it does.

```csharp
singleton Round
{
    int count;
}

event RoundStarted { }

async event(RoundStarted) Countdown(mut Round round)
{
    for (var i = 3; i > 0; i--)
    {
        round.count = i;
        await Wait.Seconds(1);
    }
    round.count = 0;
}

async int Doubled(int x)
{
    await Wait.Ticks(1);
    return x * 2;
}

system Start(Round round)
{
    if (round.count == 0) Doubled(2); // Starts a task; its value is dropped
}
```

### Provisional

Implemented, awaiting approval:

- `async` goes before a function or an event handler, before or after `local` if it has one (`local async event(...)`); before anything else it's an error that says where it goes. Methods can't be async yet, nor extern functions or functions that take an Action. `await` is a keyword, and binds like `try`: `await Doubled(3) + 1` adds 1 to the value. A `!` after an awaited call is the awaited value's: `await Fetch(name)!`.
- An async call is awaited, from async code (async functions and handlers), or a statement of its own, which starts a task; using its value without `await` is an error that says which to write. A started task's value is dropped; one that can fail is warned about as a call that can fail is, and `Load(name)!;` starts it without the warning. Only systems, views, handlers and async code start tasks: a plain function or method can't, as a task belongs to a world.
- `await` waits for an async call, or for `Wait.Ticks(n)` (the match's ticks; match code only), `Wait.Frames(n)` (this machine's frames; local code only) or `Wait.Seconds(s)`: in the match, the nearest whole number of ticks, at least one; in local code, this machine's time, which hosts give each frame (`tide_local_frame_time`) and a task counts down frame by frame, within a tenth of a millisecond. A wait of 0 or less doesn't wait. `Wait` only goes after `await`. `await` isn't allowed in a block written after a call.
- Async code is a state machine, as in C#: a call of an async function runs until it first waits, inside the code that calls it. An awaited call's frame is part of the caller's, so a task is one frame however deep it awaits, and an async function can't await itself (it can start itself again, as a task of its own).
- A task keeps its parameters and locals while it waits, copied, with their text and lists in its world's heap. An async function's component and singleton parameters aren't kept: it gets them again each time it goes on, as they are then, from the entity it belongs to, so the caller passes its own (a parameter), and a task whose entity lost one ends. Its `mut` parameters are only components and singletons; it can't keep the caller's other variables. Plain functions still take no singletons.
- An async handler is a task per event: its event is copied into the task, and its components are the entity's, got again after each wait.
- An async function's world comes from what it does: changing the match (spawning, `mut` match components and singletons, sending match events, entity changes, `Snap`, `Wait.Ticks`) makes it match code, and local state (local components and singletons, `Session` calls, `Wait.Frames`) makes it local. What it awaits or starts decides it too. One that does neither runs in whichever world starts it, and tidec makes it for each. Doing both is an error, as is starting or awaiting one from the other world. Tasks can't draw or read this frame's `Devices`.
- A task belongs to the entity the code that started it runs for, or with a task, to that task's entity, and ends when it's destroyed; code that runs for no entity starts tasks that belong to their world. A local task can belong to a match entity (a view of the match's entities started it), and ends with it, or when there's no match.
- Tasks go on at the end of the tick, after its changes and events, one after another in the order they started, those whose time has come; what they change applies after them, and their handlers can start more. Local tasks do the same at the end of the frame. A world keeps its waiting tasks in a table for each async function and handler, a row each: when it started, when it looks again, its owner and its frame.
- A system that starts tasks runs on one thread, and waits for the systems before it that start tasks, spawn or change text, as the code its tasks run until they first wait can do those. `--schedule` names it.
- Under `tide run`, a waiting task carries over to a build where its function's code, the code of what it awaits, and its frame's layout are the same; otherwise it's dropped, and tide says how many were.

### Open

- Awaiting an event: `await Hit` on an entity, which a task would wake for in the event's turn.
- Async C functions, which C finishes later (a service's answer), for local code.
- Holding a running call to await later, like C#'s `Task.WhenAll`: `var` could hold it without its type being written, as with errors.
- Cancelling a task from code, through a handle.
- Methods and functions that take an Action being async, and `await` inside a block written after a call.
- Local handlers of match events (see Events), which a local task awaiting a match event would need too.

## Input

### Decided

- One `input` declaration per game describes one player's input for one tick. It's what the network sends.
- Input is one value per player per tick. The engine writes it, and the simulation can only read it.
- `Owner` is a built-in component that ties an entity to a player. It's a normal component: it can be read, written, added and removed. Writing it hands control over.
- Players are identified by a built-in `PlayerID` type, not an `int`. Like `Entity`, it's opaque, comparable with `==`, and has a null value. The simulation only sees `PlayerID`s and never connections, so a player who reconnects and gets their `PlayerID` back keeps everything they owned. `PlayerID(0)` names a player by index, for local play and tests.
- **Sampling is Tide code:** the input's `Sample()` method. The engine calls it on the client once per tick. Fields start at their defaults, and `Sample` assigns them by name, without `mut`. It runs outside the simulation: it reads this machine's `Devices`, and the local singletons it takes, but not the match.
- **Devices are read both ways.** Local code (views, and the input's `Sample`) reads this machine's as `Devices`: `Devices.keyboard.escape.down`. Match code takes a parameter, `Devices devices`: the devices of the player who owns the entity, or the server's, as with an input parameter. The input sends what match code reads of them, and nothing else, and the engine repairs them, since it knows their ranges.
- The input stays, for what's worked out on the player's machine: an aim direction from the mouse, or anything that depends on local state. The match can't read positions on the screen (the mouse's, a touch's, the pointer's), which are in this machine's window; `Sample` works out what the match needs from them.
- Singletons stay parameters, in `Sample` too: `Sample(Settings settings)`.
- **Input carries whether buttons are held, not whether they just went down,** because a missing remote input is guessed by repeating the last one. On a device, `.pressed` means down at any point since the last sample, so a quick tap between ticks is never lost. In the simulation, `bool` input fields get `.down` and `.up`, computed against the previous tick, inside structs too (`input.aim.fire.down`).
- An input parameter in a system gives the input of the player who owns the entity, and so does a `Devices` parameter. Entities without an `Owner`, or whose owner isn't a known player, get the **server's input**, and so do systems that run once per tick. This is how the server controls what no player owns. An input parameter doesn't filter entities: add `with Owner` to only run on owned ones.
- An input can have a `Sanitize()` method. Every input passes through it before the simulation reads it, including input from other players, so systems can rely on what it guarantees without checking again. It assigns the input's fields by name, like `Sample`, and reads nothing else.
- Input fields can declare bounds: `[Clamp(lo, hi)]`, `[Min(x)]` and `[Max(x)]`, and so can the fields of structs an input holds. The engine applies them to every input before `Sanitize`, so `Sanitize` only handles what they can't express. Bounds are constants; a number bounds every component of a vector. They go on input and struct fields only, for now: on a component's or singleton's field, they're an error.
- **Input is an attack point,** so the engine is forgiving with it. Before `Sanitize` runs, NaN and infinite floats become the field's default. Nothing a client sends can put NaN in the simulation, and `Sanitize` only deals with values that are merely out of range.
- `Devices` has a keyboard, a mouse, a gamepad, a touchscreen and the pointer; pen, joysticks and sensors come later. Every button has `.pressed` (held), `.down` (went down since the last sample) and `.up` (went up), named as in Unity: the Input System's `isPressed`, and the old `GetKeyDown` and `GetKeyUp`.
  - **Keyboard:** every key by physical position, named after the US layout (`keys.w`, `keys.space`, `keys.leftShift`, `keys.digit1`, `keys.upArrow`, `keys.f1`). WASD works on AZERTY. `text` is what was typed since the last frame, a `string`, which follows the keyboard's layout as keys don't. It's a frame's: views read it, and the functions they call. `Sample` and the match can't, and the input never sends it.
  - **Mouse:** `position`, `delta` and `scroll` (`float2`), and buttons `left`, `right` and `middle`.
  - **Gamepad:** `connected`; `leftStick` and `rightStick` (`float2`); `leftTrigger` and `rightTrigger` (`float`, 0 to 1); face buttons by position (`buttonSouth`, `buttonEast`, `buttonWest`, `buttonNorth`); `dpad.up` and the other directions; `leftShoulder`, `rightShoulder`, `start` and `select`.
  - **Touchscreen:** `connected`, `primaryTouch`, and `touches`, a slot for each of 10 fingers, as Unity's. A `Touch` has `press` (a button: `.down` when the finger touched, Unity's Began, and `.up` when it lifted, Ended), `id` (Unity's `touchId`: the same while the finger touches, a new one for each touch), and `position`, `delta` and `startPosition` (`float2`). A finger keeps its slot while it touches. `primaryTouch` is the finger that touched while no other was the primary one, until it lifts. A finger that touches and lifts between two samples reads as held for one, so no tap is lost. A finger is never the mouse, as in Unity's Input System.
  - **Pointer:** `position`, `delta` and `press`: the mouse, with its left button, or the primary touch, whichever was used last, as Unity's `Pointer.current`. Code that reads it works with a mouse and a finger alike.
  - **Axes follow Unity:** `y` is positive up for sticks, the mouse, touches and the pointer. Their positions are in window pixels from the bottom left. `scroll.y` is positive when scrolling away from the user.

```csharp
input PlayerInput
{
    float2 move;
    bool jump;

    Sample()
    {
        var keys = Devices.keyboard;
        if (keys.d.pressed) move.x += 1;
        if (keys.a.pressed) move.x -= 1;
        move += Devices.gamepad.leftStick;
        jump = keys.space.pressed || Devices.gamepad.buttonSouth.pressed;
    }
}

system Jump(PlayerInput input, mut Velocity velocity)
{
    if (input.jump.down)
        velocity.value.y = 5;
}

// The same without an input: the player's devices, as they read them.
system Hop(Devices devices, mut Velocity velocity)
{
    if (devices.keyboard.space.down || devices.gamepad.buttonSouth.down)
        velocity.value.y = 5;
}
```

```csharp
input PlayerInput
{
    [Clamp(-1, 1)] float2 move;
    [Min(0), Max(3)] int gear = 1;
    bool boost;

    Sample() { move = Devices.gamepad.leftStick; }

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
- The input's defaults go through the same steps as any input: NaN repair, bounds, then `Sanitize`.
- Up to 16 players for now.
- A system can have one input parameter.
- Extra device members beyond the list above: mouse `back` and `forward`, gamepad `leftStickButton` and `rightStickButton`, and the full key list in `engine/include/tide/devices.h`.
- The types inside `Devices` are `Keyboard`, `Mouse`, `Gamepad`, `Dpad`, `Touchscreen`, `Touch`, `Pointer` and `Button`. Functions take them and `Devices` as parameters, read-only, passed without a copy: `float2 Steer(Gamepad pad)`.
- `touches` is a fixed-size array, read-only: `touches[i]` (an empty touch past the last, as forgiving as a list), `touches.count` (10: every slot, touching or not) and `foreach`, which goes through every slot. Its type has no name in Tide yet: it's the first fixed-size array, and fixed-size arrays in components (see Lists, Open) will decide how one is written.
- `Sample` takes local singletons, read-only: `Sample(Settings settings)`. Hosts pass the local state to `tide_input_sample`.
- `Devices` in views is this frame's: `.down` and `.up` since the last frame, and the `delta` of the mouse, touches and the pointer, and the mouse's `scroll`, too. What the GUI is using is hidden from views, as from `Sample`, and so is what another view claimed (see GUI). A function that reads `Devices` needs the frame, like one that draws: views and the functions they call can call it, and `Sample` can't (pass it `Devices` instead).
- `keyboard.text` is the characters typed since the last frame, with Shift, dead keys and a phone's keyboard applied, and what's pasted too, but for newlines and tabs. Backspace, Enter and the arrows are keys, not characters. It holds up to 32 characters a frame: the platform keeps the rest for the frames after, up to 1024, so a long paste comes over a few frames, as it does into the GUI's fields. It's empty while the GUI has the keyboard (a widget with the focus, a field being typed into, a modal). A function that reads it through a `Keyboard` or `Devices` parameter needs the frame, as one that reads `Devices` does, so `Sample` and systems can't call it. It can't be written, like the rest of the devices.
- A `Devices` parameter goes in systems and match event handlers, one per system; views and local handlers read `Devices`. Match code can't read `Devices`: the error says to take the parameter.
- What the input sends of the devices comes from the whole program: each value read through a `Devices` parameter, in systems and handlers and in the functions and methods they call, and every value of a part used whole, like `var pad = devices.gamepad;`, but its positions in the window, which count once they're read. A `Touch` read through a function's parameter or a copy (a `foreach` over `touches`, `touches[i]` at an index that isn't a number) is sent for every place it could be: the primary touch, every slot, or both. A game without an `input` declaration gets one that only sends the devices.
- Buttons are sent as held (`.pressed`), like bool input fields: `.down` and `.up` in match code are against last tick's input, so a guessed input that repeats the last one doesn't press them again. A button let go and pressed again between two ticks is one press.
- Repairs: sticks -1 to 1 on each axis, with NaN 0; triggers 0 to 1; the `delta` and `scroll` of the mouse, and the `delta` of touches and the pointer, finite, or 0.
- On the server's machine, its own player's input is the server's too, so entities without an owner read it. A server with no player of its own keeps the defaults.
### Open

- Pairing devices with players, for local multiplayer.
- Compact input types (bytes, quantized floats) to save bandwidth.
- The server's input computed from the game's state (AI) rather than from devices.

## Views and drawing

### Decided

- Drawing is immediate mode: code calls `Draw` functions every frame, and nothing is kept between frames.
- Views never change the match. They can change local state (see Local state).
- Views draw at whatever frame rate the game renders, not the tick rate, and see the match blended between its last two ticks, so motion is smooth even at 20 ticks per second. They draw up to a tick late for it.
- Floats are blended by default: every float, vector, quaternion and color a view reads of the match. Ints, bools, enums, entities and text are as they are at the latest tick. `[Snap]` on a field keeps it out of blending, for angles that wrap and values that jump:

```csharp
component Body
{
    float2 position;        // Blended
    [Snap] float heading;   // Wraps from 360 to 0: as it is
    int lives;              // Ints are as they are
}
```

- Something that jumps, like a respawn, a portal or a camera cut, says so from match code: `entity.Snap()` or `singleton.Snap()`. For the tick it happens in, views draw it as it is instead of sliding from where it was.
- A struct or component can say how it blends with an `Interpolate` override: `T Interpolate(T from, T to, float t)`, declared in it like an operator, with no value of its own. It replaces the default blend for that type wherever views see it:

```csharp
struct Angle
{
    float degrees;

    Angle Interpolate(Angle from, Angle to, float t)
    {
        mut var d = to.degrees - from.degrees;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return Angle { degrees = from.degrees + d * t };
    }
}

component Body
{
    float2 position; // Blended as usual
    Angle heading;   // Blended the short way round
}

event(Died dead) Respawn(mut Body body, Arena arena)
{
    body.position = arena.start;
    this.Snap(); // No sliding from where it died
}
```

### Provisional

- `view` declares a view. It looks like a system and takes the same parameters, but it runs once per rendered frame instead of once per tick. Its `mut` parameters, `Spawn`, `Add`, `Remove`, `Destroy` and `Send` are local (see Local state); input parameters are errors in a view.
- Views run in declaration order, after all the systems of the frame's ticks. Within a view, entities run in the same order as in systems.
- `Draw` functions can only be called from views and the functions they call (see Functions). Calling them from systems needs to tell predicted ticks from verified or replayed ones, which comes with multiplayer.
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
- Text is written in double quotes, with the escapes `\"`, `\\` and `\n`. Its type is `string` (see Text).
- Blending: a view's match components and singletons are copies, their fields that blend set between last tick's value and this tick's, as far as this moment is between the two ticks. Vectors, matrices, colors and rects blend component by component, quaternions the short way round (normalized), and structs field by field. An entity that wasn't there last tick is drawn as it is. Local state isn't blended: it's this machine's, as it is.
- `[Snap]` goes on floats, vectors, quaternions, colors and rects of components, singletons and structs; on anything else it's an error that says why.
- `entity.Snap()` is recorded and applied at the end of the tick, like `Destroy`, so it never makes systems wait. Each entity counts its snaps, and views only blend it between two ticks with the same count. `singleton.Snap()` needs the singleton as a `mut` parameter. Local state is never blended, so snapping it is an error, and so is `component.Snap()`, which says to snap the entity.
- `Interpolate` isn't for code to call. Singletons have no methods, so a singleton blends its own way through a struct in it that has an `Interpolate`. A `[Snap]` field of such a type is still drawn as it is.
- Hosts pass the two ticks and how far between them this moment is to `tide_frame`; sessions work that out (`tide_session_view`).

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
- 3D drawing, sprites and textures, layers.

## GUI

### Decided

- The GUI is immediate mode, called from views, and drawn over the world. It follows Unity's IMGUI.
- `GUILayout` lays widgets out automatically: they stack top to bottom, and `GUILayout.Horizontal()` puts them side by side. `GUI` has the same widgets, each at an explicit `Rect`, which comes first, as in Unity: `GUI.Button(rect, "Quit")`.
- Containers take a block, and they're ordinary functions with an `Action` parameter (see Functions): `GUILayout.Horizontal() { ... }`, `GUILayout.Vertical() { ... }` and `GUILayout.Area(...) { ... }`. Calls always have parentheses.
- Widgets edit values through `mut` parameters, and return whether the value changed: `GUILayout.Toggle("Fullscreen", settings.fullscreen)`. A button returns whether it was pressed.
- Widgets have no IDs to write. They're told apart by where they're called from and the entity the view runs for.
- The engine builds nothing a game couldn't build itself: containers are functions with an `Action`, and widgets are made of pieces games can use too.
- Gamepad and keyboard navigation are built in: focus moves between widgets, the south button presses, the east button goes back.
- Whatever the GUI is using, such as a click on a button or typing in a field, is hidden from the input's `Sample`.
- Typing into a field uses the characters the player types, which follow their keyboard layout, not keys by position.
- `Screen.width` and `Screen.height` are the window's size.
- The widgets: `Label`, `Button`, `Toggle`, `Slider`, `IntSlider`, `TextField`, `IntField`, `FloatField`, `Float2Field`, `Float3Field`, `Float4Field`, `ColorField` and `Space`, and the containers `Horizontal`, `Vertical`, `Area` and `Modal`.
- `GUILayout.Modal(anchor, mut bool open) { ... }` is a panel over the whole screen while `open` is true, like a pause menu. While it's up, it has the focus, the widgets outside it don't work, the game and views get nothing from the devices, and back (Escape or the east button) closes it.
- **Claims:** a view with widgets of its own, drawn with `Draw` or a C library's, says what they're using: `GUI.ClaimPointer()` and `GUI.ClaimKeyboard()`. What's claimed is hidden from the input's `Sample` and from the other views, as what the GUI uses is, and the view that claimed it goes on reading it.
- A claim lasts one frame. The view makes it again every frame its widget uses the device, as immediate-mode code draws every frame, so a view that stops running leaves nothing claimed.
- `GUI.ShowKeyboard()` shows a phone's keyboard, as typing into a field does, each frame it's called. It's apart from the keyboard's claim, so a claim for a widget's shortcuts doesn't bring the keyboard up.

```csharp
local singleton Settings
{
    bool open;
    bool fullscreen;
    float volume = 1;
}

view Options(mut Settings settings)
{
    if (GUI.Button(Rect(Screen.width - 210, 10, 200, 40), "Options")) settings.open = true;
    if (Devices.keyboard.escape.down || Devices.gamepad.start.down) settings.open = true;

    GUILayout.Modal(Anchor.MiddleCenter, settings.open)
    {
        GUILayout.Toggle("Fullscreen", settings.fullscreen);
        GUILayout.Slider("Volume", settings.volume, 0, 1);
    }
}

local singleton Toolbox
{
    bool dragging;
    bool renaming;
    string name;
}

// A widget of the view's own, in the window's lower left corner
view Tools(mut Toolbox box)
{
    var pointer = Devices.pointer;
    var over = pointer.position.x < 200 && pointer.position.y < 200;
    if (over && pointer.press.down) box.dragging = true;
    if (!pointer.press.pressed) box.dragging = false;
    if (over || box.dragging) GUI.ClaimPointer();

    if (box.renaming)
    {
        GUI.ClaimKeyboard();
        GUI.ShowKeyboard();
        box.name += Devices.keyboard.text;
        if (Devices.keyboard.enter.down) box.renaming = false;
    }
}
```

### Provisional

Implemented, awaiting approval:

- Positions and sizes are in pixels, as on a web page, so widgets keep their size when the window changes size. On a display scaled to 150%, a pixel is the display's logical one, 1.5 real pixels wide, as CSS pixels are.
- `Rect(x, y, width, height)` is a built-in value type measured from the top left corner, with `y` down, as in Unity's GUI, with `x`, `y`, `width` and `height`. World drawing and the mouse have `y` up. `GUI`'s rects are on the screen, inside an area or not.
- `GUILayout.Area(anchor)` places a panel sized to its content at one of nine anchors, the built-in enum `Anchor`, named as Unity's `TextAnchor` (`UpperLeft` to `LowerRight`), 12 pixels from the screen's edges. `GUILayout.Area(rect)` places it at a rect. An anchored area is placed with its size from the frame before, and moves at the end of the frame if the size changed. One too big for the screen starts at its top left margin.
- `GUILayout` widgets outside any area stack from the screen's top left, across every view. In a vertical container, buttons, toggles, sliders and fields stretch to the widest widget's width. Labelled widgets put their label in a column at least 130 wide, so a column of them lines up.
- Widgets shrink when there isn't room: an anchored area is at most as wide as the screen less its margins, and a vertical container's widgets are at most as wide as it is. A row that's too wide shrinks each of its widgets by as much as it can give, with the frame before's widths. Fields and sliders shrink down to a small minimum, a labelled widget's label column first, down to the label; buttons give up half their padding; labels and toggles don't shrink.
- The widgets, with `GUI`'s taking a `Rect rect` first (`TextField` too, see Text):
  - `Label(string text)` and `Button(string text) -> bool`, which returns whether it was pressed.
  - `Toggle(string text, mut bool value)`, `Slider(string label, mut float value, float min, float max)`, `IntSlider(string label, mut int value, int min, int max)`, `IntField(string label, mut int value)`, `FloatField`, `Float2Field`, `Float3Field`, `Float4Field` and `ColorField` (a swatch, and fields for r, g, b and a, from 0 to 1). Each returns whether it changed its value.
  - `GUILayout.Space(float size)`, and the containers `GUILayout.Vertical()`, `GUILayout.Horizontal()` and `GUILayout.Area(...)`.
- A widget's `mut` argument is the variable itself, so its type matches exactly: `Slider` takes a `float` variable, not an `int`.
- Widget IDs come from where each call is in the program, mixed with the entity a view runs for, and for a function that draws, with where it's called from. Calls from one place with the same entity count up, in the order they run.
- Widgets draw as they're called, so, like `Spawn`, they can't be on the right of `&&` or `||` or in a side of `?:`. A statement with several runs them left to right.
- Navigation: Tab and Shift+Tab move the focus between widgets, in the order they were drawn. The arrows, d-pad and left stick move it once a widget has it, and only start moving it while the game isn't reading the devices, so a HUD's button never takes the d-pad from the player. Enter, Space and the south button press; Escape and the east button let go. With the focus, left and right step a slider or number field.
- Typing: clicking a number field, or pressing Enter on it, starts typing into it with its value selected, so the first character replaces it; typing a number into a focused field starts too. Enter or leaving the field keeps a valid number; Escape keeps the old value.
- Widgets follow the pointer: the mouse, or a finger. A finger that lifted is nowhere, so nothing stays hovered where it was.
- Hidden from the input's `Sample` and from views' `Devices`: the keyboard and gamepad while a widget has the focus, and the mouse's buttons and scroll, the pointer's press and the primary touch while the pointer is over a widget or an area, or pressing a widget. While a modal is up, everything is, the movement of the mouse and the pointer too.
- The pointer is over a widget or an area where it is now, against where the last frame drew them: the game samples, and views read, before the frame's GUI sees the devices. So a press is the GUI's from the frame it lands in, with nothing before it: a finger that touches a button, or a click the mouse made as it came. And a click made as the mouse left a widget is the game's. A widget takes the pointer a frame after it first appears, so a press on it in that frame reaches the game. A frame remembers the places of up to 1024 widgets and areas; past those, the pointer is the GUI's a frame after the GUI saw it there.
- What a claim hides: `GUI.ClaimPointer()` the mouse's buttons and scroll, the pointer's press and the primary touch, which is what the pointer over a widget hides; `GUI.ClaimKeyboard()` the keys and what's typed, and not the gamepad (the GUI's focus hides both, as both move it). Where the pointer is stays everyone's.
- Claims are statements of views and the functions they call, like widgets; a function's claim is the view's that called it. A view that runs once per entity is one view.
- A claim stands from the end of its frame to the end of the next: it hides from the samples taken after it, and from the next frame's views. So a widget claims the pointer while the pointer is over it, before any press, and a click on it never reaches the game. The engine doesn't know where a view's own widgets are, as it does the GUI's, so a press with nothing before it (a finger that touches, a widget that appears under the pointer) reaches the game and the other views for the frame it lands in.
- The GUI's own use comes first: a view never reads what the GUI is using, whatever it claimed, and the GUI's widgets work under a claim, as they're drawn on top. While a view has the keyboard, Tab and the arrows don't start moving the GUI's focus.
- Views that claim the same device all go on reading it: which of their widgets is on top is theirs to know. Up to 16 views claim in one frame and go on reading; past that, a claim still hides.
- A modal is an anchored area over the screen, dimmed. The one drawn last is on top, and only its widgets work. Its first widget takes the focus the frame after it comes up, and back doesn't close it on the frame it came up, so the press that opened it doesn't. `GUI` has no modal at a rect yet.
- The drawing: a dark panel behind each area and the engine's default font, with no style to change yet.
- `GUI.Disabled(bool disabled) { ... }` grays out the widgets in its block while `disabled` is true, as Unity's `GUI.enabled = false` and `EditorGUI.DisabledScope` do: they're drawn at half opacity, and can't be hovered, pressed, focused or typed into, so Tab skips them. A widget disabled while it's pressed lets go, and a field disabled while it's typed into keeps its old value. The block lays nothing out: its widgets go on in the container around it, `GUI`'s and `GUILayout`'s alike. The mouse on a disabled widget is still the GUI's, hidden from the input's `Sample`. Inside a disabled block, another stays disabled whatever its own `disabled` is. An area's panel doesn't fade, only its widgets.
- **Clipboard:** `Clipboard.Copy(string text)` puts text on this machine's clipboard, like a "Copy" button next to a room's code. It's a statement of local code, as `Session`'s calls are (views and local handlers, not functions yet, `Sample` or match code), and the host does it after the frame; the frame's last one wins. Up to 255 bytes of UTF-8 for now, cut where a character starts. A browser takes it only shortly after a click or a key, as when a button is pressed. Pasting needs no call: Ctrl+V (Cmd+V on macOS) types what's on the clipboard into the text field that has the focus, but for newlines and tabs, on desktop and the web. Reading the clipboard from code isn't offered, since a browser only gives it to a paste.

### Open

- A field for any enum. A game couldn't write one itself until there are generics.
- Styles and themes.
- More than one block per function, like Swift's labelled trailing closures.
- The pieces widgets are made of, so a game can build its own like the built-in ones: a control's ID, which the compiler derives from the call as for the built-in widgets, whether it's hovered, pressed or focused, and drawing in GUI units.
- Scrolling, clipping, and keys that repeat while held.

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
  - Match code (systems, match event handlers, match scenes) can't read or write anything local, and can't read this machine's `Devices`: it takes a `Devices` parameter, the owner's, which the input sends. The input's `Sample` reads this machine's, and writes only the input.
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

view DrawSparks(mut Spark spark)
{
    Draw.Circle(spark.position, 0.1, Color.yellow);
    spark.framesLeft -= 1;
    if (spark.framesLeft <= 0) this.Destroy();
}

// Error: match code can't read local state.
system Count(Spark spark) { }
```

### Provisional

- `local` is only a keyword at the start of a declaration. It goes before components, singletons, events and event handlers; before anything else it's an error that says why (structs and functions belong to neither side, views are always local, systems run the match).
- **`LocalEntity`** is an entity of the local world. `Spawn` in local code returns one, and `this` is one in a view of local components, where in a view of match components it's an `Entity`. Local code can hold and read an `Entity` (the unit a player selected, say) but never change one. The match's declarations can't hold a `LocalEntity`, and neither can structs, which both sides share.
- A view runs for the entities of one world: its components are all local or all the match's.
- Local handlers handle local events, and `Spawned` and `Destroyed` of local entities. They only take local state for now.
- The host keeps the local state and calls `tide_local_init(local)` once, then `tide_frame(w, previous, alpha, local, draw, gui)` every frame. `tide_frame` runs the views, then applies their local changes and events. Outside a match, `w` is NULL, and views that read the match don't run.
- The local world has its own entity table and queue, which grow as they need, as the match's do.

### Open

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
- `Main` is the fallback: when the last scene of `Main`'s world unloads, `Main` loads again, set up by its `Spawned` handlers as at the start. A match `Main` comes back in the match, a local one in the local state.
- When `Main` is local, a match can't load it, so a match whose last scene unloads ends. Every machine in it goes offline, back to its local `Main`, with `Disconnected` and the reason `Ended` (see Sessions).
- Loaded scenes share their world: the same systems, singletons and `Time`. Scenes that share nothing are separate worlds, which never communicate. Only the server creates worlds, so match code can't (see Open).
- **Visibility:** scenes are public by default, seen by every player in the world. `Scene.Load(Hand { ... }, SceneVisibility.Private)` loads a private one, which only the server and the players given it see: `Scene.AddPlayer(scene, player)` and `Scene.RemovePlayer(scene, player)`. Membership is match state, so the server decides it, and a player who's added receives the scene's state.
- A private scene with no players exists only on the server, which is where secrets like RNG seeds go. Code that reads a private scene only predicts correctly on machines that see it, and the server corrects the others.
- A client never loads match scenes on its own: it has the ones the server has it in.
- A system named `Main` is an ordinary system. Without a `scene Main`, the missing entry point's error says so.

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

system Collapse(Arena arena)
{
    if (arena.size <= 0) Scene.Unload(this);
}
```

### Provisional

- `scene` and `local scene` are only keywords at the start of a declaration. A scene's component can have methods, and `Add` can give its entity other components, like any entity.
- `Spawn`, `Add` and `Remove` of a scene's component are errors that point to `Scene.Load` and `Scene.Unload`. `Destroy` on a scene's entity unloads it, and `Scene.Unload` of an entity that isn't a scene does nothing.
- `Scene.Load` makes its entity right away, like `Spawn`, so it follows the same evaluation order and can't be inside `?:` or on the right of `&&` and `||`.
- An entity spawned into a scene that's been unloaded by the spawn's turn is never made, like the loot an enemy drops while its arena unloads.
- Entities unload in archetype order, each archetype's from its last row, and the scene's own entity last. Each gets its `Destroyed` handlers.
- `SceneVisibility` is a built-in enum, `Public` and `Private`. A local scene has no visibility. Local code can't change who sees a scene.
- In generated C, a scene's component holds its visibility and players too, as `tide_visibility` and `tide_players`, one bit per player. Code can't name them.
- The engine loads `Main` itself: `tide_world_init` loads a match `Main`, and `tide_local_init` a local one, with `TIDE_MAIN_IS_LOCAL` defined. `tide/run.h` starts without a match when `Main` is local.
- The `Main` scene is the world's first entity.
- The fallback waits for the end of the tick (the frame, in local state), once every change and handler has applied, so unloading one scene and loading another in the same tick never brings `Main` back. Entities in no scene don't count. `Main` comes back at most once a tick: one that unloads itself as it loads leaves the world without a scene until the next.
- A match that ends runs no more ticks after the one that left it without a scene. The server tells every player then, and anyone who tries to join. Only the match ends: local state carries on as it was, so whatever local scenes were loaded stay loaded.

### Open

- The server's own code: creating worlds from scenes, and moving players between them.
- Entity references that say what they point to, so the compiler can check `Scene.Unload` on an entity read from a field. It can already check `this` in a system taking the scene's component, as in `Collapse`.
- The details of private scenes: what players outside one see of it, and how an added player catches up.

## Sessions

### Decided

- Single-player and multiplayer are the same: every match runs on a server, and this machine's player connects to it, over a loopback transport when the server is on this machine. The engine assumes nothing about what a game does with it, like pausing; games build that from inputs and state.
- Local code decides which match this machine is in: `Session.Start(scene)` starts one on this machine, `Session.Join(code)` joins the match in a room by its code, `Session.Connect(address, port)` joins another machine's by its address, and `Session.Leave()` leaves. `Start` names the scene the match starts in, with its values like `Scene.Load`'s: `Session.Start(Arena { size = 30 })`. `Join` and `Connect` get whatever the server runs.
- There's one kind of match. Whether others can join it is a switch on it, not another way to start one: a match starts closed, `Session.Open()` lets others join the match this machine runs, and `Session.Close()` stops letting them, at any time. Single-player is a match nobody else was let into.
- The machine that runs a match sends players out of it with `Session.Kick(player, message)`, or every other machine's with `Session.KickAll(message)`. The message is text, so a game can say anything, and kicked players get it with their `Disconnected`. A kick isn't a ban.
- The machine that runs a match says what it means by leaving it: `Session.Leave()` leaves, and with host migration the match goes on without this machine; `Session.End()` ends the match for everyone, who go offline with `Ended`. A crash, or closing the window, is a `Leave`.
- **Host migration** is a game's choice, off by default: `settings { hostMigration = true; }`. When the machine running a room's match leaves or stops answering, another player's machine takes the match over from the last tick it verified, and the other players join it again as the same players. The old host's player leaves the match (`PlayerLeft`), and from then on, entities without an owner read the new host's input.
  - Only room matches change hands. A match joined by address (`Session.Connect`) has no relay to meet at again, so it ends, as it does without host migration.
  - A new host only has what its machine could see: private scenes it wasn't in are lost, whole. A game that uses host migration shouldn't keep secrets.
  - No player ever gets another's cookie. With host migration on, every player gets a hash of each player's cookie, so a new host can tell who's coming back and nobody can pass for someone else.
  - A host that leaves says so, and the match changes hands at once. One that stops answering is only noticed after the time-out, so players wait a few seconds first.
- Players find each other's matches in rooms, by a code. The hosting machine picks its room's code itself, so local code has it at once, with nothing to wait for: `Session.room`.
- Local code sees where this machine stands through a built-in local singleton, `Session` (taken as a parameter like any singleton), and the built-in local events `Connected` and `Disconnected`.
- Clients have no input delay: their own input applies at once, and they run ahead of the server so it arrives in time. Only other players' inputs are ever guessed.
- The desktop transport is our own thin layer on UDP. The web's is WebRTC data channels that neither order nor resend, so browsers host matches as well as join them. A relay we host introduces players to each other; their packets go straight between them whenever their networks allow it (see AGENTS.md, Networking).

```csharp
local scene Main { }

scene Arena
{
    int size = 20;
}

view Menu(Session session)
{
    if (session.state != SessionState.Offline) return;
    GUILayout.Area(Anchor.MiddleCenter)
    {
        if (GUILayout.Button("Play")) Session.Start(Arena);
        if (GUILayout.Button("Host"))
        {
            Session.Start(Arena { size = 40 });
            Session.Open();
        }
        if (GUILayout.Button("Join")) Session.Join("K7QF2M");
        if (GUILayout.Button("Connect")) Session.Connect("192.168.1.5");
    }
}

local event(Disconnected gone) BackToMenu()
{
    // gone.reason says why: Left, TimedOut, Refused, ServerLeft, Failed, Ended or Kicked,
    // and gone.message what a kick said.
}
```

### Provisional

Implemented, awaiting approval:

- `Session` has `state` (`SessionState.Offline`, `Connecting` or `Connected`), `player` (this machine's `PlayerID`, once connected), `ping` (the round trip to the server, in milliseconds), `server` (whether this machine runs it), `open` (whether others can join it, which only the server's machine knows) and `room` (the code of the room the match is in, or `""`, as it is while the match is closed). It's read-only. A single-player match is `Connected` too, with `server` true and `open` false.
- `Connected` is sent once the match's world has arrived and this machine plays in it; `Disconnected { DisconnectReason reason; string message; }` when it leaves: `Left` (it called `Leave`, or started another match), `TimedOut` (the server stopped answering, or never did), `Refused` (another build of the game, no room left, or a closed match), `ServerLeft` (the server's machine left, which ended the match), `Failed` (it couldn't start: no network, a port in use, an address that isn't one, a room nobody has) `Ended` (the match's last scene unloaded, and `Main` is local; see Scenes) or `Kicked` (the server's machine sent it away). `message` is the kick's, and `""` for the other reasons.
- `Session.Kick(player)` and `Session.KickAll()` send no message. A kicked player leaves the match at once, as if they had left: `PlayerLeft` follows, and they can join again, as the same player, while the match is open. A goodbye can be lost: a kicked player who didn't hear it is told again whenever they're in touch, while they still send to the server and when they join it again, before they're let in (taking a match over with host migration included). Once they've heard, they can join again. Kicks only change a match this machine runs: on a client, or offline, they do nothing, and this machine's own player can't be kicked (it calls `Leave`). A message is up to 255 bytes of UTF-8, cut where a character starts. A frame takes up to 16 kicks; a `Start`, `Join`, `Connect` or `Leave` after them drops them, as it does Opens. A machine kicked from a match it joined with `--join` or `--connect` doesn't join it again by itself.
- `Session.Open(port)` takes players in a room, and on `port` too, 7777 without one, except on the web, which has no ports. A closed match turns away anyone who isn't in it, coming back or not; the players in it stay. Opened again, it takes players in the same room and on the same port. `Open` and `Close` only change a match this machine runs: on a client, or offline, they do nothing. When it can take players neither in a room nor on the port, the match stays closed. Opens and Closes before a `Start`, `Join`, `Connect` or `Leave` in the same frame were for the match it ends, so they're dropped. `Session.Play` and `Session.Host` are errors that point to `Start` and `Open`.
- `Session.Connect(address, port)` takes `"192.168.1.5"`, `"192.168.1.5:7777"` or a name like `"localhost"`, and the port is 7777 without one.
- A room's code is 6 letters and digits, without look-alikes (no `0`, `O`, `1` or `I`), like `K7QF2M`: about a billion codes. Case and spaces don't matter when joining. `Session.Join` with text that can't be a code is an error, which points to `Connect` for an address.
- A code another room already has is refused by the relay, and the host picks another at once, before anyone could have read it: `Session.room` changes. It's `""` while the relay can't be reached; the host keeps trying, and the room opens again under the same code if it's still free. Players already in keep playing: only joining needs the relay.
- Joining a room that doesn't exist, or whose host can't be reached, ends with `Failed` at once. A room's host gets 15 seconds to answer for the first time, rather than 5, since WebRTC can take a while to find a way through routers.
- Desktop and web players meet in the same rooms: desktop games speak WebRTC too, with an implementation of our own (no third-party code, so games carry no license terms for it). Everyone reaches the relay over `wss://`: desktop games with their system's TLS (on Linux, OpenSSL's libssl, which they load if it's there).
- Under `tide run --web`, a reload goes on with the match closed: the room closes, and the other players drop out.
- Session calls are statements, in views and local handlers. Functions can't make them yet, nor can match code, which runs the same on every machine, nor `Sample`.
- A match can't start in a scene that holds text or lists yet.
- Starting a match leaves the one this machine is in first, which sends `Disconnected` with `Left` before the new one's `Connected`.
- The server's player joins before the match's first tick, as `PlayerJoined` handled at the end of it; the server only starts ticking then.
- When `Main` is the match's, `tide/run.h` starts it at once, and opens it with `--host [port]`, or joins another with `--join code` and `--connect address` on the command line (and `tide run --host`, `--join` and `--connect`).
- Up to 16 players.
- A client predicts at most a second ahead of the last tick the server confirmed, however many ticks that is at the match's tick rate; beyond it, it waits for the server. The server keeps four seconds of ticks to send again; a player further behind gets the whole world again. A player that was sent the world has every tick since kept for it until it catches up, however long the world took to arrive.
- **Coming back:** joining a server gives this machine a cookie, and joining the same server again (the same room, or the same address) presents it, so the player gets their `PlayerID` back, and with it whatever the game kept for them. If the server still has them connected (their old connection went quiet), the new one takes over with no events at all; if they'd left, `PlayerJoined` comes again with the same `PlayerID`. A server keeps a slot for a player who left until it has no slot that was never used; then it gives away the one away longest, and that player's cookie stops working.
- The cookie lives as long as the program: it doesn't survive a restart yet, and it's not safe against someone on the network guessing it.
- **Host migration:** the first player's machine to reach the room takes the match over: the relay pings the room's host for 3 seconds, and if it doesn't answer, or it left, gives the room to the first player there and introduces the others to it. A host that answered keeps the room, and the players who came join it again.
- While a match changes hands, local code sees no `Connected` or `Disconnected`: `Session.state` is `Connecting`, `Session.player` stays the same, and views see the last world this machine had. On the new host, `Session.server` becomes true.
- The new host runs the match at its tick rate, and goes on from the last tick it verified. The last host's player gets `PlayerLeft` at the first tick. A player who doesn't come back within 20 seconds gets `PlayerLeft` too; one who comes back later is the same player, with `PlayerJoined` again.
- A closed match changes hands closed: its players come back, but no one new joins. The room keeps its code, and its key (128 random bits) only goes to the match's players, so no one else can take the room over.
- A host that's still running but doesn't answer the relay in time, like one stopped at a breakpoint, loses the room to its players. When it carries on, its match fails on its own machine (`Failed`).
- `Session.End()` on a client does nothing. A match that ends, by `Session.End()` or by its last scene unloading, ends at the relay too: for five minutes, players who come back to its room to take it over are told it ended, and go offline with `Ended`, whatever goodbye they missed. The room's code stays taken that long.

### Open

- Keeping the cookie across restarts, and making it unguessable.
- Servers with no window and no player of their own.
- Keeping a room open across reloads under `tide run --web`.
- Lobbies, and finding matches without a code.
- Telling predicted state from verified state in game code (see AGENTS.md, Networking).

## Settings

### Decided

- `settings { ... }` sets the engine's settings for the game. Each is set without a type: `tickRate = 30;`. The game's own values are constants (see Constants).
- `settings` is a keyword only at the top level of a file, so it still works as a name everywhere else.
- Every setting has a default, so the block only lists what it changes. A game has at most one block, in any of its files.
- Values are constant expressions, and can name constants.
- Settings are part of the build, so every machine in a match has the same ones.
- The editor completes the settings' names and shows each one's default and what it does. A name that isn't a setting is an error that says which one was meant.
- The first settings are `title` (the window's), `tickRate` (ticks per second, 60 by default), `hostMigration` (see Sessions), `appId` (the app's ID on phones) and `version` (the version people see). The window's size, the most players and the default port can come later.

```csharp
settings
{
    title = "Asteroids";
    tickRate = 30;
    hostMigration = true;
}
```

### Provisional

Implemented, awaiting approval:

- `hostMigration` is true or false, or a constant that is.
- `tickRate` is an int from 1 to 1000, known while compiling. The machine that runs a match decides its rate (its desc's, or its game's), and the players who join tick at it. A new `tickRate` under `tide run` takes effect when a match starts.
- `title` is text written out, or a constant that is. `--title` and CMake's `TITLE` go over it, and without any of them, the window has the game's name: its folder's under `tide`, its target's under CMake. Under `tide run`, a new title shows the next time the game runs. On Android it's the app's name under its icon.
- `appId` is text written out, or a constant that is: what Android and iOS, and their app stores, know the app by, like `com.studio.game`, the same for every version. It's two parts or more between dots, of letters and digits, each starting with a letter: what Android and iOS both take. Without it, apps are `dev.tide.<the game's name>`, which is fine to test with; an app made to ship (`tide build --android --release`) needs one.
- `version` is text written out, or a constant that is: the version people see, like `1.2.0`, one to three numbers between dots, which Android and iOS both take (iOS's version has to be). Without it, it's `1.0`. The number app stores order updates by is each build's own: on Android, the minutes since 2020.
- A setting that's set twice, a name that isn't a setting, and a type written before one are errors. The last two say that a game's own values are constants.
- In generated C, the settings are in `tide_game_api` (`tick_rate` and `title`), in the `.c` rather than the header, so they don't change the game's hash.

### Open

- Changing settings when the game starts, from the command line or a file: the server would send its settings to the players who join.
- The web page's title: it doesn't show the `title` setting yet.

## C functions

### Decided

- Games can call C libraries. `extern` declares a function written in C, with no body: `extern float Noise(float x);`. Calls cost what a call between C functions does.
- Any code that can call a function can call an extern one, match code and local code alike. The language doesn't mark or check what C does: its determinism, the state it keeps and its thread safety are the game's to get right, and the compiler takes a C call as touching nothing it tracks, so C never makes systems wait for each other, and a system that calls C splits its entities across threads like any other. An `Entity` a system that splits its entities passes C may be one it just spawned, whose ID comes once the system is done: `tide_entity_is_temporary` (`tide/entity.h`) tells, and a handle C keeps stays temporary.
- The C function's name is the extern's own name as written, or the one `[NativeName("...")]` gives, so the Tide name can follow Tide's style: `[NativeName("stb_perlin_noise3")] extern float Noise(...);`. A namespace doesn't change the C name.
- A game's C is in its folder, with nothing to set up: every `.c` file there compiles with the game, with the engine's determinism flags, and every prebuilt library there (`.a`, `.lib`, `.so`, `.dll`, `.dylib`) links with it when it was built for the platform being built for. tide tells which platform a library is for from its contents, not its name or folder, so one folder holds every platform's libraries. C for one platform only uses `#ifdef`, as any C does.
- Writing `external` gets an error that points to `extern`.
- C takes pointers without Tide having pointer arithmetic: the parameter says how a value is passed, and the call takes its address. `mut T` is `T *`, `in T` is `const T *`, a `List<T>` is a pointer to its elements (`T *` with `mut`), with the count passed separately, and a `string` is a zero-terminated UTF-8 copy. Each is only valid during the call. A `const char *` that C returns is copied into text.
- C calls keep the order of evaluation: calls to C, and to functions that call it, run left to right like the rest of Tide, whatever order C would pick.

```csharp
[NativeName("stb_perlin_noise3")]
extern float Noise(float x, float y, float z, int xWrap, int yWrap, int zWrap);

component Ground
{
    float2 position;
    float height;
}

system Shape(mut Ground ground)
{
    ground.height = Noise(ground.position.x, ground.position.y, 0, 0, 0, 0);
}
```

### Provisional

Implemented, awaiting approval:

- Extern functions take and return numbers, `bool`, vectors, matrices, quaternions, `Color`, `Rect`, `Entity`, `PlayerID`, enums (`int32_t` in C, or `uint8_t` and `uint16_t` for `: byte` and `: ushort`), and structs and components of those, by value. A struct is a C struct with the same fields in the same order: Tide's types have no padding the compiler adds, so the layouts match. The vector types are `tide/math.h`'s (`tide_float3` and the like), which C files can include.
- A `mut` parameter is a pointer to the caller's variable (`float *`, `Stats *`), which C can change, as `mut` works for Tide functions.
- An `in` parameter points at the caller's variable or field when it's one of the parameter's type, and otherwise at a copy made for the call, like a computed value or one that converts (`int3` to `in float3`). `in` is only for extern functions: Tide functions' parameters are read-only already. It doesn't go on text or lists, which go by address anyway.
- A list's elements are plain data (no text or lists in them). C gets NULL for an empty list. With `mut`, C can change the elements, but not how many there are. C can't return a list.
- C gets text as it is when a zero follows it, and a copy in the scratch area otherwise. Text C returns is copied into the scratch area, as C may reuse its memory; NULL is empty text. A `mut string` can't go to C.
- An `Action` and the devices can't be passed to C, nor structs that hold text or lists.
- Calls are put in order the way spawns and GUI calls are: what has to go first runs before its statement, and before a loop's condition each round. An `&&` or `||` whose right side calls C, or a `?:` whose sides do, runs as `if` statements then, so each part still only runs when it would, with its own calls in order. So do struct operators that call C.
- `extern` declarations go at the top level of a file, not in structs. `local` doesn't apply to them.
- The C name must be a C identifier, not a C keyword. `[NativeName]` can't name the engine's functions (`tide_...`), and two externs can't name the same C function.
- tidec declares each extern function itself in the generated C, from its Tide signature, rather than including the library's header, whose names could clash with the game's. If the signature doesn't match C's, the game is wrong the way C would be.
- Libraries: static libraries and Windows import libraries link in; `.so` and `.dylib` files link and are copied next to the game, which finds them there; a `.dll` is copied next to the game, and links through its import library (`.lib`). Windows games build for MinGW, so a static library built with Microsoft's compiler may need its C runtime and fail to link; rebuild it with clang or MinGW. A library tide can't read (LLVM bitcode, text) is left out, saying so.
- On the web, only C files and WebAssembly libraries define functions. A web build fails when an extern function has no definition there, rather than when the page calls it.
- `tide run` builds again when a C file, header or library changes. C code is part of the game's library, which each build replaces, so the state C keeps starts over at each reload.
- tide's own CMake (`tide_add_game`) compiles the `.c` files in the game's folder but the host's, or those listed after `SOURCES`. It doesn't pick up libraries.

### Open

- Objects C owns (`ma_engine *`): a handle type only local state can hold, since pointers differ between machines.
- Keeping the state of C libraries across hot reloads, by building them apart from the game's library.
- Reading declarations from C headers, with tide's built-in clang.

## Packages

### Decided

- Games share code as packages: Tide code (and C) from a git repository, at a commit, or from a folder on this machine.
- A game lists its packages in a file of its own, `tide.packages`. A package from git is always pinned to a commit there, so every build of a game builds the same code; there's no lock file, and nothing is generated.
- A package declares nothing global: everything it declares is in its namespace.
- `tide update` moves packages to the newest commit of what they follow: a branch, or the newest tag of a version.
- Packages come from git hosts. Code adapts to the version of tide it builds with (see Conditional compilation).

```
# tide.packages
github.com/someone/tide-physics@v1 b01aac4d779e32c9c415283b65e40c86d691e7ab
../shared
```

### Provisional

Implemented, awaiting approval:

- `tide.packages` has a line each, and `#` at the start of a line or after a space starts a comment. A package line is a source, then its commit: `github.com/owner/repo 0123...` (40 hex digits, or 64), with `@ref` after the source for what `tide update` follows (the default branch without one) and `//folder` for a package in a folder of the repository. A line that starts with `.`, `/` or a drive (`../shared`, `D:/shared`) is a folder, relative to the file's, and has no commit. A line without its commit is an error that says `tide update` pins it.
- A package is a folder whose `tide.packages` says `package Name`: its name and namespace, dotted or not. Its files are in that namespace or one inside it, and it can't declare `Main`, the input or settings, which are the game's. `tide 0.3` says the oldest tide it builds with; a game can say it too. Its other lines are the packages it needs; a package from git can't need a folder.
- The game's `tide.packages` lists every package it builds with, those its packages need included: each one's lines must be in the game's, as the same source (any commit) or the same folder, and the game's line is what's built. `tide add` and `tide update` add what's missing. Two packages of the same name are an error.
- Files compile with each package's first, in an order where each package comes after those it needs (by name where that leaves a choice), each package's by path, then the game's. That's the default order of systems.
- A subfolder with a `tide.packages` of its own is a game or a package of its own, and isn't part of the folder's game or package: a package's example game lives inside it, listing `..`.
- A version-looking ref (`v1`, `v1.2`, `1.2.3`) follows the newest tag whose numbers start with it, leaving out pre-releases; any other ref is a branch, or else a tag, of that name.
- Packages from git download as their host's archive of the commit (GitHub, GitLab, Codeberg and Bitbucket), with curl and tar, into a cache shared by every game: `$TIDE_PACKAGES`, or `%LOCALAPPDATA%/Tide/packages` on Windows and `~/.tide/packages` elsewhere, at `<host>/<owner>/<repo>/<commit>`. `tide run` and `tide build` download what's missing. Branches and tags come from git's own list of them (`info/refs`), which every host serves.
- `tide add <source>` takes URLs as people copy them (`https://...git`, `git@host:owner/repo`) and writes the source as `tide.packages` does. `tide update [names]` matches a package by its name, its repository's or folder's name, or its source, and only changes commits, leaving the rest of the file as it is.
- A package's systems that match no entity, and its handlers of events nothing sends, aren't warned about: a game may use only part of a package.
- The language server analyzes a game with its packages, and a package's file with an open game that uses it, or else the package on its own, which needs no `Main`.

### Open

- A registry of names, so a line doesn't need its host.
- Packages from private repositories, and hosts other than those four.
- Symbols for the packages a game has, so code can add to another package only when it's there (`#if PHYSICS`).

## Conditional compilation

### Decided

- Code can adapt to the version of tide it builds with, as C# code adapts to Unity's with `#if UNITY_2022_3_OR_NEWER`: tide gives the tools, and packages adapt to the versions they support.

```csharp
#if TIDE_0_4_OR_NEWER
const int SLOTS = 8;
#else
const int SLOTS = 4;
#endif
```

### Provisional

Implemented, awaiting approval:

- C#'s directives: `#if`, `#elif`, `#else` and `#endif`, each on a line of its own (spaces before it are fine, and a `//` comment after it), nesting. Conditions take symbols, `true`, `false`, `!`, `&&`, `||`, `==`, `!=` and parentheses.
- The symbols are tide's versions: `TIDE_0_4_OR_NEWER` is true in tide 0.4 and after, `TIDE_0_4_2_OR_NEWER` in 0.4.2 and after. Only a version's numbers count: a pre-release (0.4.0-nightly.3) is the version it comes before. Any other symbol is an error, so a misspelling doesn't quietly leave code out.
- Lines a directive leaves out aren't read, only looked through for the directives that end them, as in C#: they can hold anything, like code with syntax a newer tide reads. A directive inside a `/* */` comment in code that's read is part of the comment.
- There are no symbols for platforms, debug and release, or anything else that differs between the machines of a match, which all have to run the same code.
- Editors show the lines left out as comments, fold each branch, and the formatter leaves directives and those lines as they are.

## Open

- How entities authored as data (levels, prefabs) feed into archetype derivation.
- Archetype growth. Every `Add` and `Remove` can apply to any entity, so the compiler assumes every combination is reachable, and a game can have at most 256 archetypes. Storage grows on demand; narrowing the combinations safely needs more analysis.
- How modules, such as the engine's built-in systems, initialize when there's a single `Main`.
- Groups of systems (phases such as input, simulation, late), which Before and After could order as a whole.
- Access control: whether a namespace can keep declarations to itself.
