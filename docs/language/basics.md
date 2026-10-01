# Basics

Tide's syntax is C#-like. If you know C#, C or Java, most of it will read as you expect. This page covers what's the same everywhere in the language: values, variables and control flow. The pages after it cover what's special to Tide.

## Style

- Types, systems and methods use PascalCase: `Transform`, `MovePlayer`, `Spawn(...)`.
- Fields, parameters and locals use camelCase: `trs.position`, not `trs.Position`. So do public properties, even static ones: `Color.red`, `quaternion.identity`.
- True constants use FULL_CASE: `Math.PI`, `Math.TAU`.
- Braces go on lines of their own, and so does an `else` after a block. A block that fits on one line can stay there: `scene Main { }`, `if (dead) { return; }`.

The editors' formatter lays code out this way (see [Editors](../guide/editors.md)).

## Types

| Type | What it holds |
|---|---|
| `bool` | `true` or `false` |
| `int` | A 32-bit integer |
| `float` | A 32-bit float. There's no `double`. |
| `float2`, `float3`, `float4`, `int2`, `int3`, `int4` | Vectors (see [Math](./math.md)) |
| `quaternion`, `float2x2`, `float3x3`, `float4x4` | Rotations and matrices (see [Math](./math.md)) |
| `Color` | A color, with `r`, `g`, `b` and `a` from 0 to 1 (see [Views](./views.md)) |
| `string` | Text (see [Text and lists](./text-and-lists.md)) |
| `List<T>` | A list of values (see [Text and lists](./text-and-lists.md)) |
| `Entity` | A handle to an entity (see [Components and entities](./entities.md)) |
| `PlayerID` | A player (see [Input](./input.md)) |

Everything is a value: assigning copies it, and nothing is shared, text and lists included. That's what keeps the whole world plain data, which the engine copies to take snapshots.

### Numbers

- `1.5` is a `float`, and the `f` suffix is optional. An `int` converts to `float` by itself, never the other way: `int(x)` truncates toward zero.
- Integers can be written in decimal (`255`), hex (`0xFF`) or binary (`0b1010`), and `_` separates digits: `1_000_000`.
- Integer arithmetic wraps on overflow, and dividing by zero gives 0. No input can crash the simulation.
- Bitwise operators `& | ^ ~ << >>` work on `int`, as in C#.

## Variables

Everything is read-only unless it's `mut`, locals included:

```csharp
var speed = player.speed * 2;    // read-only, type inferred
mut var total = 0.0;             // mutable, type inferred
float limit = 10;                // read-only, explicit type
mut float scale = 1;             // mutable, explicit type
```

A local needs a value when it's declared, and can't reuse the name of another local or parameter in scope.

## Constants

`const` declares a value that code reads by name. Constants go at the top level of a file:

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

- The type is written out. The value is worked out from literals, other constants, constructors, `Math` and operators, like a field's default. It can't read fields, singletons or `Time`.
- Constants are named in FULL_CASE.
- A constant is the same on every machine. Any code can read one: systems, views, handlers, functions and other constants. Reading one never makes a system wait for another.
- They work anywhere a constant value is needed: field defaults, `[Clamp]` bounds, `case` labels and the values of enum members. In the last two, int constants and operators on them work: `case MAX_LEVEL + 1:`.
- A constant can be a number, vector, matrix, quaternion, `bool`, `Color`, `Rect`, text, struct or enum. It can't be a list or an entity.
- A constant in a namespace belongs to it: code outside writes `Combat.CRIT_MULTIPLIER` (see [Namespaces and files](./namespaces.md)).
- Changing a constant under `tide run` reloads the game and keeps the match where it is.

## Settings

`settings` sets the engine's settings for the game. It goes at the top level of any one of the game's files:

```csharp
settings
{
    title = "Asteroids";
    tickRate = 30;
}
```

| Setting | What it is | Without it |
|---|---|---|
| `title` | The window's title | The game's folder's name |
| `tickRate` | How many times a second the match ticks, from 1 to 1000: `Time.dt` is 1 / `tickRate` | 60 |

- Settings are set without a type, to constant expressions, which can name constants. A game's own values go in constants, not settings.
- A game has one `settings` block. The editors complete the settings' names and show what each one does.
- Settings are part of the build. The machine that runs a match decides its tick rate, and every player ticks at that rate. Under `tide run`, a new `tickRate` takes effect when a match starts.
- `--title` goes over the `title` setting (see [The tide command](../guide/cli.md)).

## Default values

`default` is the default value of the type where it goes, so the type doesn't have to be spelled out:

```csharp
Draw.Circle(default, 5, Color.yellow);  // At float2(0, 0)
float2 center = default;
if (target == default) { return; }      // The null entity
```

It's the value a field of that type starts at: zero for numbers, vectors, matrices and colors, `false`, empty text, an empty list, the null entity, no player, and for a struct or component, its fields' defaults, as `Stats { }` makes them.

Its type comes from where it goes: a typed local, an assignment, an argument, a `return`, a field's value, or the other side of `==`, `!=` or `?:`. `var x = default;` says no type, so it's an error. `default` is only ever compared, never added or multiplied.

## Operators

Operators and their precedence follow C#, including compound assignments (`+=`, `<<=` and the rest) and `cond ? a : b`. `i++`, `i--`, `++i` and `--i` are statements, the same as `i += 1` and `i -= 1`, not expressions.

Expressions run left to right: operands, arguments and field values in the order they're written. That's part of what makes every machine spawn the same entities with the same IDs.

## Control flow

`if` and `else`, `switch`, and the loops `for`, `foreach` and `while`, with `break`, `continue` and `return`, as in C#.

```csharp
for (var i = 0; i < 10; i++)
{
    if (i == 3) continue;
    total += i;
}

foreach (var score in scores)
{
    best = Math.Max(best, score);
}

while (fuel > 0)
{
    fuel -= burn;
}
```

A `for` loop's variable is read-only in its body unless it's declared `mut var`; the loop's step can change it either way. `foreach` goes through a list in order, and each element is a read-only copy. There's no `do ... while` yet.

## Enums and switch

Enums and `switch` follow C#:

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

- A member without a value is one more than the one before it, and the first is 0. A value is an int, which can come from constants: `Playing = FIRST_LEVEL + 1`.
- Members are always written with their enum: `Phase.Playing`.
- `==` and `!=` compare two values of the same enum, and `int(phase)` gives a member's value.
- `switch` works on ints and enums. A case is an int, an enum's member, or a constant. Every section ends with `break` or `return`, so none runs into the next.

## Comments and attributes

Comments are `//` and `/* */`.

Attributes, in square brackets, are only metadata: when a system runs, or the bounds of an input's field, never what code does. The engine can enforce what one declares, as it clamps an input's field to its `[Clamp]`.

```csharp
[After(Gravity)]
system Move(mut Body body) { ... }
```
