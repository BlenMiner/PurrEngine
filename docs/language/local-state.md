# Local state

The **match** is what every machine simulates the same way. **Local state** belongs to one machine, and lives outside the match: menus, settings, connection status, animation timers, particles. It's never sent, rolled back or checked against other machines.

`local` in front of a declaration makes it local:

```csharp
local singleton Settings
{
    float volume = 1;
    bool showFps;
}

local component Spark
{
    float2 position;
    int framesLeft = 30;
}
```

`local singleton`, `local component`, `local scene` and `local event` are the local kinds. Everything else belongs to the match. Structs and functions belong to neither, and both sides use them.

A local singleton exists once per machine, for the whole program. Local components make local entities.

## Views change local state

Views run every frame, in a match or not. They read the match, and read and write local state: `mut` local parameters, and `Spawn`, `Add`, `Remove` and `Destroy` of local entities.

```csharp
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
```

`LocalEntity` is an entity of the local world: local `Spawn` returns one, and in a view of local components, `this` is one. Local code can hold and read an `Entity` of the match, like the unit a player selected, but never change it.

Local structural changes and local events apply at the end of the frame, as the match's do at the end of the tick.

A view only runs when everything it reads exists, so a view of the match's components or singletons doesn't run outside a match. A view runs for the entities of one world: its components are all local, or all the match's.

## The compiler keeps them apart

No mistake here can reach a running game, because the compiler enforces the boundary:

- **Match code** (systems, match event handlers, match scenes) can't read or write anything local, and can't read this machine's `Devices`: it takes a `Devices` parameter, the owner's, which the input sends.
- **Local code** (views, local event handlers, local scenes) can read the match, but never change it: no `mut` on the match's components or singletons, and no spawning, changing or sending match things.
- The input's `Sample` reads this machine's devices and local singletons, and writes only the input.

The only way from local code into the match is input. Starting, joining and leaving a match are session calls, which never touch a running match (see [Multiplayer](./multiplayer.md)). The errors say where to go instead:

```csharp
// Error: match code can't read local state.
system Count(Spark spark) { }
```

## Local scenes and events

A local `Main` starts the program outside any match, usually in a menu:

```csharp
local scene Main { }
```

Local handlers handle local events, and `Spawned` and `Destroyed` of local entities. They only take local state for now.
