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

- A member without a value is one more than the one before it, and the first is 0.
- Members are always written with their enum: `Phase.Playing`.
- `==` and `!=` compare two values of the same enum, and `int(phase)` gives a member's value.
- `switch` works on ints and enums. Every section ends with `break` or `return`, so none runs into the next.

## Comments and attributes

Comments are `//` and `/* */`.

Attributes, in square brackets, are only metadata: when a system runs, or the bounds of an input's field, never what code does. The engine can enforce what one declares, as it clamps an input's field to its `[Clamp]`.

```csharp
[After(Gravity)]
system Move(mut Body body) { ... }
```
