# Your first game

This page builds a small game: a ball bouncing around, and a player for everyone who joins, which they steer with the keyboard. It takes a few minutes, and shows most of what a PurrEngine game is made of.

You'll need `purr` [installed](./install.md).

## A folder of files

A game is a folder of `.purr` files: every `.purr` file in it and its subfolders. Every declaration is visible from every file, with no imports. Make a new folder, and a file in it called `game.purr`.

## Data: components

Entities are made of **components**, plain data with fields. Fields can have defaults, and start at zero otherwise.

```csharp
component Body
{
    float2 position;
    float2 velocity;
    float radius = 20;
}

component Ball { }
```

`Ball` has no fields: it marks which bodies are balls.

## Where it starts: the Main scene

The game starts in the scene called `Main`. When it loads, it gets the built-in `Spawned` event, and a handler of that event sets it up:

```csharp
scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Body { velocity = float2(160, 120) }, Ball);
}
```

`Spawn` makes an entity from a list of components. `Body { velocity = ... }` sets one field and leaves the others at their defaults, and a bare `Ball` has all of them at their defaults.

## Behavior: systems

A **system** runs every tick, once for every entity that has the components it takes. Parameters are read-only, unless they're `mut`. `with Ball` only picks entities that have a `Ball`, without reading it.

```csharp
system Move(mut Body body, Time time)
{
    body.position += body.velocity * time.dt;
}

system Bounce(mut Body body, with Ball)
{
    if (Math.Abs(body.position.x) > 300) body.velocity.x = -body.velocity.x;
    if (Math.Abs(body.position.y) > 200) body.velocity.y = -body.velocity.y;
}
```

`Time` is a built-in singleton: there's one per world, and `time.dt` is the length of a tick in seconds. Systems run in the order they're written, and always in the same order on every machine.

## Drawing: views

A **view** looks like a system, but it runs once per frame instead of once per tick, and it can only read the match. `Draw` functions only work in views.

```csharp
view DrawBalls(Body body, with Ball)
{
    Draw.Circle(body.position, body.radius, Color.red);
}
```

The camera starts at the origin, one world unit per pixel, with `y` up.

## Run it

In the folder:

```sh
purr run
```

`purr` compiles the game and opens a window with the ball bouncing. It keeps its work in a hidden `.purr` folder, which you can delete any time. Leave the window open: whenever you save, `purr` rebuilds the game and the window picks up the new code (see [hot reload](./cli.md#hot-reload)). The next part adds players.

## Players and input

Each player's **input** is what the network sends of them, once per tick. It's declared once per game, and its `Sample` method fills it on the player's own machine, from this machine's `Devices`:

```csharp
input Controls
{
    float2 move;

    Sample()
    {
        var keys = Devices.keyboard;
        if (keys.d.pressed) move.x += 1;
        if (keys.a.pressed) move.x -= 1;
        if (keys.w.pressed) move.y += 1;
        if (keys.s.pressed) move.y -= 1;
    }
}
```

Keys are named by their position on a US keyboard, so `WASD` is in the same place on every layout.

When a player joins, the world gets the built-in `PlayerJoined` event. This handler gives them a body, with an `Owner`, a built-in component that ties an entity to a player:

```csharp
event(PlayerJoined joined) AddPlayer()
{
    Spawn(Body { radius = 16 }, Owner { player = joined.player });
}
```

A system that takes the input gets the input of the player who owns the entity it's running for:

```csharp
system Steer(Controls input, mut Body body, with Owner)
{
    body.velocity = Math.NormalizeSafe(input.move) * 250;
}
```

Put `Steer` before `Move`, so bodies move the tick they're steered. Last, draw the players. Views can read `Session`, where this machine stands, so yours can be yellow and the others gray:

```csharp
view DrawPlayers(Body body, Owner owner, Session session)
{
    var color = owner.player == session.player ? Color.yellow : Color.gray;
    Draw.Circle(body.position, body.radius, color);
}
```

Save, then type `r` and press Enter in `purr`'s terminal, and steer with `WASD`. The game carried over to the new code when you saved, but your player had joined before `AddPlayer` existed, so they had no body yet. Starting over makes them join again.

## Play together

Single-player was already a match, on a server this machine ran for itself. So the same game plays with others. In one terminal:

```sh
purr run --host
```

And in another, on this machine or another one on the network:

```sh
purr run --join localhost
```

Use the host's address instead of `localhost` from another machine, like `purr run --join 192.168.1.5`. The host takes players on UDP port 7777; `--host 8000` picks another. Each player steers their own yellow body, with no input delay.

## Build it for the web

```sh
purr run --web
```

This builds the game as a web page, using WebGL 2, and opens it. For a page to put online:

```sh
purr build --release --web
```

It goes in `build/`, as one self-contained `.html` file with the game inside, which opens straight from disk. Web games can only play single-player for now.

## The whole game

::: details game.purr
```csharp
component Body
{
    float2 position;
    float2 velocity;
    float radius = 20;
}

component Ball { }

input Controls
{
    float2 move;

    Sample()
    {
        var keys = Devices.keyboard;
        if (keys.d.pressed) move.x += 1;
        if (keys.a.pressed) move.x -= 1;
        if (keys.w.pressed) move.y += 1;
        if (keys.s.pressed) move.y -= 1;
    }
}

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Body { velocity = float2(160, 120) }, Ball);
}

event(PlayerJoined joined) AddPlayer()
{
    Spawn(Body { radius = 16 }, Owner { player = joined.player });
}

system Steer(Controls input, mut Body body, with Owner)
{
    body.velocity = Math.NormalizeSafe(input.move) * 250;
}

system Move(mut Body body, Time time)
{
    body.position += body.velocity * time.dt;
}

system Bounce(mut Body body, with Ball)
{
    if (Math.Abs(body.position.x) > 300) body.velocity.x = -body.velocity.x;
    if (Math.Abs(body.position.y) > 200) body.velocity.y = -body.velocity.y;
}

view DrawBalls(Body body, with Ball)
{
    Draw.Circle(body.position, body.radius, Color.red);
}

view DrawPlayers(Body body, Owner owner, Session session)
{
    var color = owner.player == session.player ? Color.yellow : Color.gray;
    Draw.Circle(body.position, body.radius, color);
}
```
:::

## Next

- `purr schedule` shows which of these systems could run at the same time, and why the others wait. See [the schedule](../engine/schedule.md).
- Learn the language a topic at a time, starting with [the basics](../language/basics.md).
- The [demo](./demo.md) is a slightly bigger game, with firing and an options menu.
