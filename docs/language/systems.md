# Systems

A system is code that runs every tick. Its parameters say which entities it runs for, and what it does with their data.

```csharp
system ClampPlayer(mut Transform trs, with Player)
{
    if (trs.position.y < 0)
        trs.position.y = 0;
}
```

## Parameters are the query

A system's parameter list is its whole query. Each parameter's modifier says how the system relates to that component or singleton:

| Modifier | Meaning |
|---|---|
| none | Reads it |
| `mut` | Writes it |
| `with` | The entity must have it. No data, so no data dependency. |
| `without` | The entity must not have it. |

So `ClampPlayer` runs for every entity that has a `Transform` and a `Player`, and changes its `Transform`.

Keeping what a system touches (its access) apart from which entities it picks (its filters) is what lets the engine run systems side by side safely: two systems that only filter on the same component never wait for each other.

Other parameters:

- **Singletons**, read or `mut`, like components: `system Score(mut Stats stats, Rules rules)`.
- **The input**, or **`Devices`**: what the player who owns the entity does (see [Input](./input.md)).

The entity itself isn't a parameter: it's `this` (see [Entity handles](./entities.md#entity-handles)).

## Once per entity, or once per tick

A system with component parameters runs once for every matching entity, and `this` is the entity it's running for. One without them runs once per tick, for no entity, so it has no `this`:

```csharp
system CountDown(mut Round round, Time time)
{
    round.timeLeft -= time.dt;
}
```

`return;` ends the system for the current entity, and it goes on with the next one.

## Order

Systems run in one order, the same on every machine. By default, it follows the files, sorted by path, then the order of the systems in each file. A game's [packages](../guide/packages.md) come first, each after the packages it needs.

`[Before]` and `[After]` change that. They take any number of systems:

```csharp
[After(Gravity, Collisions)]
system Move(mut Body body, Time time)
{
    body.position += body.velocity * time.dt;
}
```

Of the systems whose constraints are met, the earliest in the default order runs next. Constraints that form a loop are an error that names the loop. Hover a system in your editor to see its place in the order.

## Running side by side

Two systems that write the same component isn't a mistake: it's normal. Such systems run one after the other, in the order above. Systems that share nothing they write can run at the same time. The compiler works this out from the parameters, and can tell you why each system waits (see [The schedule](../engine/schedule.md)):

```
stage 2  Move  (per entity)
         after Steer: both write Body
```

Declaring more than a system uses makes others wait for nothing, so it's a warning: a parameter that's never used, or a `mut` one that's never written. A component that's only there to pick entities belongs in `with`. The editors offer quick fixes for both.

## What systems can't do

Systems run the match, so they can't read this machine's local state, and they can't draw: drawing happens every frame, in [views](./views.md).
