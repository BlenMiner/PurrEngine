# Scenes

A scene is a group of entities that load and unload together: a menu, a level, an arena. Several scenes can be loaded at once, and several copies of the same one.

```csharp
scene Arena
{
    int size = 20;
}
```

A loaded scene is an entity like any other. The declaration is its component, and its fields are the scene's state, so a system that takes `Arena` runs once per loaded arena.

## Main

The game starts in the scene called `Main`, and there's exactly one. It sets up the first scene, not the game loop: the engine runs the tick.

```csharp
scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Transform { scale = float3(1, 1, 1) }, Player);
}
```

A match `Main`, like this one, starts a single-player match with it right away. A local `Main` starts the program outside any match, usually in a menu, which starts one later (see [Local state](./local-state.md) and [Multiplayer](./multiplayer.md)).

## Loading and unloading

```csharp
var arena = Scene.Load(Arena { size = 30 });
Scene.Unload(arena);
```

`Scene.Load` loads a scene into the current world and returns its entity. `Scene.Unload` unloads it, destroying every entity it owns. `Spawn` of a scene is an error that says to use `Scene.Load`.

Loading and unloading happen at the end of the tick, like `Spawn` and `Destroy`. A scene is set up by `Spawned` handlers, and its entities get `Destroyed` when it unloads, both within that tick.

```csharp
// Sets up each arena. The floor joins that arena.
event(Spawned) SetupArena(Arena arena)
{
    Spawn(Floor { size = arena.size });
}

system Collapse(Arena arena)
{
    if (arena.size <= 0) Scene.Unload(this);
}
```

## Which scene owns what

A spawn joins the scene of the entity the code is running for. In a handler, that's the entity the event was sent to, so the floor above joins the arena it was spawned for. Code that isn't running for an entity, like a system that runs once per tick, spawns into no scene, and those entities live until they're destroyed.

A scene itself is never owned: it lives until it's unloaded, whoever loaded it.

Loaded scenes share their world: the same systems, singletons and `Time`.

## Private scenes

Scenes are public by default: every player in the world sees them. A private one is only seen by the server and the players given it, which is how a card game keeps each hand to its player:

```csharp
scene Hand
{
    PlayerID player;
}

event(PlayerJoined joined) DealIn()
{
    var hand = Scene.Load(Hand { player = joined.player }, SceneVisibility.Private);
    Scene.AddPlayer(hand, joined.player);
}
```

`Scene.AddPlayer(scene, player)` and `Scene.RemovePlayer(scene, player)` change who sees it. Who sees a scene is part of the match, so the server decides it, and a player who's added receives the scene's state.

A private scene with no players exists only on the server, which is where secrets like random seeds go. Code that reads a private scene only predicts correctly on machines that see it, and the server corrects the others.

A client never loads match scenes on its own: it has the ones the server has it in.
