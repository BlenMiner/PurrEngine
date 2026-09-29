# Events

An event is something that happened, sent from one part of the game to the rest: a hit, a pickup, a round ending. Events carry their context in fields, like a struct's.

```csharp
event Hit
{
    Entity attacker;
    int damage;
}
```

## Sending

`entity.Send(...)` sends an event to an entity, and `Send(...)` sends it to the whole world:

```csharp
system Explode(Entity self, Bomb bomb)
{
    if (bomb.timer > 0) return;
    bomb.target.Send(Hit { attacker = bomb.owner, damage = 50 });
    self.Destroy();
}

system EndRound(Round round)
{
    if (round.timeLeft <= 0) Send(RoundOver { winner = round.leader });
}
```

The sender isn't recorded. When handlers need it, put it in a field: the entity that sends is often not the one that matters, like a bomb sending a `Hit` for whoever threw it.

## Handling

A handler is code that runs when an event is sent. It's declared with `event` too: the first parentheses hold the event that triggers it, and the second hold the same parameters as a system's.

```csharp
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
```

- A handler's components and `Entity` come from the entity the event was sent to. If that entity doesn't match its parameters, the handler doesn't run for it.
- A handler that takes no components and no `Entity` runs once per event, whether it was sent to an entity or not.
- A handler that needs an entity needs every `Send` of its event to name one. The compiler checks: `Send(Hit { ... })` is an error when a `Hit` handler takes components, and says to write `entity.Send(...)`.
- A handler that doesn't read the event can leave its name out: `event(Spawned) Arm(...)`.

## When they run

Events are handled at the end of the tick, along with structural changes, in the order they were all recorded. A `Send` before a `Destroy` of the same entity is handled while the entity still exists. An event sent to an entity that's gone by its turn is dropped.

Handlers can send events and change entities in turn. Those are handled next, until nothing is left, so everything settles within the tick. Systems later in the same tick don't see an event's effects yet, as with `Spawn`.

The handlers of one event run in the order they're declared, and `[Before]` and `[After]` order them as they do systems.

Events waiting to be handled are part of the world: a snapshot holds them, and running a tick again sends and handles them again the same way.

## Built-in events

The engine sends these. They cost nothing where no handler takes them.

| Event | Sent to | When |
|---|---|---|
| `Spawned` | The entity | It's spawned, or its scene is loaded |
| `Destroyed` | The entity | It's destroyed, while its components can still be read |
| `PlayerJoined` | The world | A player joins, with their `PlayerID` in `player` |
| `PlayerLeft` | The world | A player leaves, with their `PlayerID` in `player` |

```csharp
// Spawned has no fields, so it needs no name.
event(Spawned) Arm(Entity player, with Player)
{
    Spawn(Weapon { owner = player });
}

event(PlayerJoined joined) GiveBody()
{
    Spawn(Body, Owner { player = joined.player });
}
```

The server picks the tick a player joins or leaves on, so every machine handles it on the same one.

Handlers can't draw: drawing happens every frame, in [views](./views.md). Local state has events of its own (see [Local state](./local-state.md)).
