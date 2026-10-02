# Components and entities

Tide is an ECS: the world is made of **entities**, entities are made of **components**, and **systems** run code over them. This page is about the data; [Systems](./systems.md) is about the code.

## Components

A component is plain data, declared with `component`:

```csharp
component Health
{
    int value = 100;
    int max = 2 * 2 * 25;
}

component Poisoned { }
```

A component with no fields, like `Poisoned`, is still useful: systems can pick the entities that have it, or don't.

### Defaults

Fields can have a default. It must be a constant: literals, constructors of built-in types, struct values of constants, `Math` functions, built-in constants like `quaternion.identity`, and operators on those. A default can't read other fields, singletons or `Time`.

```csharp
component Turret
{
    float angle = Math.Radians(45);
    float3 offset = float3(0, 1.5, 0);
    Color tint = Color.red;
}
```

Fields without a default start at zero. `Entity` fields always start as the null entity, and can't have another default.

## Singletons

A singleton holds state there's one of per world, like the rules, the score or a random seed. Other ECSs call them resources.

```csharp
singleton Physics
{
    float3 gravity = float3(0, -9.81, 0);
    int collisionCount;
}
```

Singletons start with their defaults when the world is created. Systems take them as parameters, like components (see [Systems](./systems.md)).

`Time` is a built-in singleton, with `dt`, the length of a tick in seconds, and `tick`, which counts from 0. Systems can read it, but not write it.

## Spawning entities

`Spawn` makes an entity from a list of component values:

```csharp
var e = Spawn(Transform { scale = float3(1, 1, 1) }, Player);
```

A component value is written like `Health { max = 200 }`, without `new`. Fields left out take their defaults, and a bare type name, like `Player`, takes every default.

An entity has at most one component of each type. When it needs several of something, use a list inside one component, one entity per item that points back at its owner, or separate component types.

## Changing entities

```csharp
e.Add(Stunned { duration = 2 });
e.Remove(Stunned);
e.Destroy();
```

- `Add` of a component the entity already has replaces its value.
- `Remove` of a component the entity doesn't have does nothing.
- `Add`, `Remove` and `Destroy` on an entity that's already gone do nothing.

These are **structural changes**, and they don't happen right away. They're recorded, and applied together at the end of the tick, in the order they were made. The handle `Spawn` returns can be used at once, for example to store in a component, but the entity's data can only be read once the changes apply.

In a system that splits its entities across threads, the handle is temporary until the system is done, when the entity gets its ID and every handle the system kept becomes the real one (see [The schedule](../engine/schedule.md#splitting-across-threads)). Text shows it as `Entity(new)` meanwhile.

That's what lets systems run side by side: nothing they do changes which entities exist until they're all done.

## Entity handles

`Entity` is a handle to an entity. Components can hold them, to point at each other:

```csharp
component Weapon
{
    Entity owner;
}
```

`default` is the null entity, so `weapon.owner == default` tells whether it points anywhere, and `weapon.owner = default;` clears it.

In a system, view or event handler that runs for an entity, `this` is that entity:

```csharp
system Expire(mut Lifetime life)
{
    life.ticks -= 1;
    if (life.ticks <= 0) this.Destroy();
}
```

In local code that runs for a local entity, like a view of local components, `this` is a `LocalEntity`. Code that runs once per tick, frame or event runs for no entity, so it has no `this`, and neither do functions.

A component's methods have `this` too: the entity whose component they're called on.

```csharp
component Health
{
    int value;

    bool IsMine(Entity attacker) { return attacker == this; }
}
```

So a method that uses `this` can only be called on a component the code runs for, like a system's `Health health`. A copy of one, in a local variable or a function's parameter, belongs to no entity.

## Archetypes

An archetype is a set of components that entities share, and entities with the same set are stored together. There are no archetype declarations: the compiler finds every one from the code, from what each `Spawn` makes and what `Add` and `Remove` can turn it into.

## Limits

For now, a game has at most 64 components and 256 archetypes. Everything else grows as it needs, with no limit but memory: entities, the rows of each archetype, and the structural changes and events in a tick (see [C hosts](../engine/c-hosts.md#memory)).
