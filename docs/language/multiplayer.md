# Multiplayer

Every match runs on a server, and this machine's player connects to it. When the server is on this machine, that's over a loopback transport, so single-player and multiplayer are the same game with the same code. The engine assumes nothing about what a game does with a match, like pausing: games build that from inputs and state.

## Starting a match from `purr run`

A game whose `Main` scene is the match's starts playing at once. `purr run` can host it or join one instead:

```sh
purr run --host                 # others can join: in a room, and on UDP port 7777
purr run --host 8000            # on another port
purr run --connect 192.168.1.5  # join another machine's match, by its address
purr run --join K7QF2M          # join the room with this code
purr run --web --host           # on the web, in a room only
```

## Starting a match from code

A game that starts in a menu has a local `Main`, and its local code decides which match this machine is in:

| Call | What it does |
|---|---|
| `Session.Play(scene)` | Starts a match on this machine alone |
| `Session.Host(scene)` | Starts a match others can join: in a [room](#rooms), and on port 7777 too (not on the web) |
| `Session.Host(scene, port)` | The same, on another port |
| `Session.Join(code)` | Joins the match in the room with this code, like `"K7QF2M"` |
| `Session.Connect(address)` | Joins another machine's match by its address: `"192.168.1.5"`, `"192.168.1.5:7777"` or a name like `"localhost"` |
| `Session.Connect(address, port)` | The same, on another port than 7777 |
| `Session.Leave()` | Leaves the match |

`Play` and `Host` name the scene the match starts in, with its values as for `Scene.Load`: `Session.Host(Arena { size = 30 })`. `Join` and `Connect` get whatever the server runs. Starting a match leaves the one this machine is in first.

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
        if (GUILayout.Button("Play")) Session.Play(Arena);
        if (GUILayout.Button("Host")) Session.Host(Arena { size = 40 });
        if (GUILayout.Button("Join")) Session.Join("K7QF2M");
        if (GUILayout.Button("Connect")) Session.Connect("192.168.1.5");
    }
}
```

Session calls are statements, in views and local event handlers. Match code can't make them: it runs the same on every machine.

## Rooms

Players find each other's matches by a room's code, like `K7QF2M`: 6 letters and digits, without look-alikes like `0` and `O`. A machine that hosts opens a room, and `Session.room` is its code at once, for the game to show; other players join it with `Session.Join(code)`, from a browser or a desktop game alike.

```csharp
view RoomCode(Session session)
{
    if (session.room == "") return;
    GUILayout.Area(Anchor.UpperCenter)
    {
        GUILayout.Label($"Room {session.room}");
    }
}
```

The relay, a server of ours, only introduces players to each other. Their packets then go straight between them, over WebRTC, which finds a way through their routers. When their networks can't connect directly, a TURN server carries the packets instead. The host's machine picks the code itself, so there's nothing to wait for. If another room had it first, the host picks another before anyone could read it out, and `Session.room` changes. It's `""` while the relay can't be reached, and the room opens again once it can: players already in keep playing, and only joining needs the relay.

## Where this machine stands

`Session` is a built-in local singleton, taken as a parameter like any other, and read-only:

| Field | What it is |
|---|---|
| `state` | `SessionState.Offline`, `Connecting` or `Connected` |
| `player` | This machine's `PlayerID`, once connected |
| `ping` | The round trip to the server, in milliseconds |
| `server` | Whether this machine runs the server |
| `room` | The code of the room the match is in, or `""` if it's in none |

The built-in local events `Connected` and `Disconnected` say when that changes. `Connected` is sent once the match's world has arrived and this machine plays in it. `Disconnected` has a `reason`:

| Reason | Why |
|---|---|
| `Left` | This machine called `Leave`, or started another match |
| `TimedOut` | The server stopped answering, or never did |
| `Refused` | The server runs another build of the game, or has no room |
| `ServerLeft` | The server's machine left, which ended the match |
| `Failed` | It couldn't start: no network, a port in use, an address that isn't one, or a room nobody has |
| `Ended` | The match's last scene unloaded (see [Scenes](./scenes.md#loading-and-unloading)) |

```csharp
local event(Disconnected gone) BackToMenu(mut Menu menu)
{
    menu.message = $"Disconnected: {gone.reason}";
}
```

## Players in the match

In the match, players come and go through the built-in events `PlayerJoined` and `PlayerLeft`, with the player's `PlayerID`. The server's own player joins before the match's first tick.

```csharp
event(PlayerJoined joined) GiveBody()
{
    Spawn(Body, Owner { player = joined.player });
}
```

What each player controls is up to the `Owner` component (see [Input](./input.md)).

**Coming back:** joining a server gives this machine a cookie, and joining the same server again presents it, so the player gets their `PlayerID` back, and with it whatever the game kept for them. If the server still thought they were connected, the new connection takes over with no events at all; if they'd left, `PlayerJoined` comes again with the same `PlayerID`. The cookie lasts as long as the program, for now.

## No input delay

A player's own input applies at once: clients run ahead of the server, by about half the round trip, so their input arrives in time. Only other players' inputs are ever guessed, and when a guess is wrong, the client rolls back and runs the ticks again. See [Networking](../engine/networking.md) for how.

## Limits for now

- Up to 16 players.
- Under `purr run --web`, a reload plays on alone: the room closes, and the other players drop out.
- A match can't start in a scene that holds text or lists yet.
