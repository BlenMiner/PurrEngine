# Async and tasks

Some code takes longer than a tick: a countdown before a round, an ability that ends after a moment, a sign-in that waits for a service. An `async` function can wait, with `await`, and pick up where it stopped. Calling one starts a **task**, which goes on by itself, tick after tick or frame after frame, until it's done.

```csharp
singleton Round
{
    int count;
}

event RoundStarted { }

// Counts down a second at a time, then the round begins.
async event(RoundStarted) Countdown(mut Round round)
{
    for (var i = 3; i > 0; i--)
    {
        round.count = i;
        await Wait.Seconds(1);
    }
    round.count = 0;
}
```

## Async functions

`async` before a function lets it wait. It returns its value like any function, with no `Task<T>` to write: `async int`, `async string`, or `async void` for nothing.

```csharp
async int Doubled(int x)
{
    await Wait.Ticks(1);
    return x * 2;
}
```

A call of an async function is used in one of two ways:

- **`await` it** for its value, from async code: `var n = await Doubled(3);`. The code waits there until the call is done, then goes on with its value. `await` binds like `try`, so `await Doubled(3) + 1` adds 1 to the value.
- **Call it as a statement** to start a task: `Doubled(3);`. The task goes on by itself, and the code that started it doesn't wait. A started task's value is dropped.

Using an async call's value without `await` is an error that says which of the two to write.

An async function that can fail works as any function that fails (see [Errors](./errors)): `await Fetch(name) ?? "nobody"`, `if (await Fetch(name) is string token) { ... }`, `try await Fetch(name)`, and `await Fetch(name)!`.

## Async handlers

`async` before an event handler makes each event start a task, which can wait like an async function:

```csharp
async event(Spawned) Arm(mut Turret turret)
{
    await Wait.Seconds(2); // Ready two seconds after it's spawned
    turret.armed = true;
}
```

Local handlers can be async too: `local async event(...)` (see [Local state](./local-state.md)).

Systems and views can't wait, as they run again every tick or frame: they start tasks instead, by calling an async function.

## What tasks wait for

`await` waits for an async function's call, or for one of these:

| | |
|---|---|
| `await Wait.Ticks(n)` | `n` of the match's ticks. Match code only. |
| `await Wait.Frames(n)` | `n` of this machine's frames. Local code only. |
| `await Wait.Seconds(s)` | `s` seconds: in the match, the nearest whole number of ticks (at least one), the same on every machine; in local code, this machine's time. |

A wait of 0, or less, doesn't wait at all.

For now:

- Methods, extern functions and functions that take an `Action` can't be async.
- `await` can't go in a block written after a call (an `Action`), and `Wait` only goes after `await`.
- Tasks can't draw, or read this frame's `Devices`: that's what views do, every frame.
- Only systems, views, handlers and async code start tasks. A plain function or method can't, as a task belongs to a world.

## Where tasks run

A task belongs to a world, like the code that starts it. A system's or a match handler's tasks are the match's: they're part of it, in snapshots, state hashes and rollback, they go to players who join, and they carry on when the match changes hands. A view's or a local handler's tasks are this machine's, in its local state.

An async function's world comes from what it does: changing the match (spawning its entities, changing its components through `mut`, sending its events) makes it match code, and using local state makes it local. One that does neither runs in whichever world starts it. Doing both is an error, as a task runs in one world.

## What a task keeps

While a task waits, it keeps its locals and its parameters, copied. Its **components and singletons** it gets again each time it goes on, as they are then, so it sees what changed while it waited. They have to be the caller's own, its parameters:

```csharp
async void Dash(mut Body body)
{
    body.speed = 10;
    await Wait.Seconds(0.2);
    body.speed = 1; // The entity's Body as it is now
}

system StartDash(mut Body body, Controls controls)
{
    if (controls.dash.down) Dash(body);
}
```

An async function's `mut` parameters are its components and singletons; it can't keep the caller's other variables while it waits, so it returns what it works out instead.

## When tasks go on, and when they end

Tasks whose time has come go on at the end of the tick (or frame), after its changes and events, in the order they started; what they change, spawn and send then applies too. A task ends when its code does, and:

- **with what started it**: a task started by code that runs for an entity belongs to that entity, and ends when the entity is destroyed. A task started by a task belongs to the same entity. Code that runs for no entity starts tasks that belong to the world, which last as long as it does.
- **without its components**: a task whose entity no longer has the components it takes ends.

A task waiting for an async call waits in the same place: the call is part of the task, and ends with it. An async function can't await itself, as its task would hold itself; it can start itself again instead, as a task of its own.

## In C

Hosts tell local tasks how long each frame is, before `tide_frame`: `tide_local_frame_time(local, seconds)`. `tide/run.h` does. Under `tide run`, a waiting task carries over to a new build when its code is the same; when it changed, the task is dropped, and tide says so.
