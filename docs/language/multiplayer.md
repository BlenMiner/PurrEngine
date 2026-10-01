# Multiplayer

Every match runs on a server, and this machine's player connects to it. When the server is on this machine, that's over a loopback transport, so single-player and multiplayer are the same game with the same code. The engine assumes nothing about what a game does with a match, like pausing: games build that from inputs and state.

## Starting a match from `tide run`

A game whose `Main` scene is the match's starts playing at once. `tide run` can open it to others, or join one instead:

```sh
tide run --host                 # others can join: in a room, and on UDP port 7777
tide run --host 8000            # on another port
tide run --connect 192.168.1.5  # join another machine's match, by its address
tide run --join K7QF2M          # join the room with this code
tide run --web --host           # on the web, in a room only
```

## Starting a match from code

A game that starts in a menu has a local `Main`, and its local code decides which match this machine is in:

| Call | What it does |
|---|---|
| `Session.Start(scene)` | Starts a match on this machine, which runs its server |
| `Session.Open()` | Lets others join the match this machine runs: in a [room](#rooms), and on port 7777 too (not on the web) |
| `Session.Open(port)` | The same, on another port |
| `Session.Close()` | Lets no one else join; the players in the match stay |
| `Session.Kick(player, message)` | Sends a player out of the match this machine runs, telling them why (the message is optional) |
| `Session.KickAll(message)` | The same for every player on another machine |
| `Session.Join(code)` | Joins the match in the room with this code, like `"K7QF2M"` |
| `Session.Connect(address)` | Joins another machine's match by its address: `"192.168.1.5"`, `"192.168.1.5:7777"` or a name like `"localhost"` |
| `Session.Connect(address, port)` | The same, on another port than 7777 |
| `Session.Leave()` | Leaves the match. With [host migration](#host-migration), a match this machine runs goes on without it |
| `Session.End()` | Ends the match this machine runs, for everyone |

`Start` names the scene the match starts in, with its values as for `Scene.Load`: `Session.Start(Arena { size = 30 })`. `Join` and `Connect` get whatever the server runs. Starting or joining a match leaves the one this machine is in first.

There's one kind of match. It starts closed, so single-player is just a match nobody else was let into. `Open` and `Close` change that at any time: a game can start alone and open its match to friends later, then close it once the party's full. Closing turns away anyone who isn't in the match, and opening it again uses the same room and port.

`Kick` and `KickAll` send players out, and they get the message with their `Disconnected` (see below). A kicked player always hears it: one whose network lost the goodbye is told the next time they're in touch with the server. It's a kick, not a ban: once they know, a kicked player can join again, as the same player, while the match is open. To end a party and play on alone, close the match and kick everyone:

```csharp
Session.Close();
Session.KickAll("Thanks for playing!");
```

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
        if (GUILayout.Button("Play")) Session.Start(Arena);
        if (GUILayout.Button("Host"))
        {
            Session.Start(Arena { size = 40 });
            Session.Open();
        }
        if (GUILayout.Button("Join")) Session.Join("K7QF2M");
        if (GUILayout.Button("Connect")) Session.Connect("192.168.1.5");
    }
}
```

Session calls are statements, in views and local event handlers. Match code can't make them: it runs the same on every machine.

## Rooms

Players find each other's matches by a room's code, like `K7QF2M`: 6 letters and digits, without look-alikes like `0` and `O`. Opening a match opens a room, and `Session.room` is its code at once, for the game to show; other players join it with `Session.Join(code)`, from a browser or a desktop game alike.

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
| `open` | Whether others can join it (only the server's machine knows) |
| `room` | The code of the room the match is in, or `""` if it's in none, as when it's closed |

A single-player match is a match too, so `state` is `Connected` there as well: this machine runs the server, and its player connects to it without going through the network. That's `server` true and `open` false. `$"{session}"` shows all of it at once.

The built-in local events `Connected` and `Disconnected` say when that changes. `Connected` is sent once the match's world has arrived and this machine plays in it. `Disconnected` has a `reason`:

| Reason | Why |
|---|---|
| `Left` | This machine called `Leave`, or started another match |
| `TimedOut` | The server stopped answering, or never did |
| `Refused` | The server runs another build of the game, has no room left, or its match is closed |
| `ServerLeft` | The server's machine left, which ended the match |
| `Failed` | It couldn't start: no network, a port in use, an address that isn't one, or a room nobody has |
| `Ended` | The match's last scene unloaded (see [Scenes](./scenes.md#loading-and-unloading)), or the machine running it called `Session.End()` |
| `Kicked` | The server's machine sent it away, saying why in `message` |

```csharp
local event(Disconnected gone) BackToMenu(mut Menu menu)
{
    menu.message = gone.reason == DisconnectReason.Kicked ? $"Kicked: {gone.message}" : $"Disconnected: {gone.reason}";
}
```

## Host migration

When the machine running a match leaves, the match ends for everyone, unless the game turns on host migration in its [settings](./basics.md#settings):

```csharp
settings
{
    hostMigration = true;
}
```

Then, when the machine running a room's match leaves or stops answering, another player's machine takes the match over, and the others join it again as the same players:

- The match goes on from the last tick the new host had verified. Players' predictions after it are rolled back, as when any guess is wrong.
- The last host's player leaves the match (`PlayerLeft`), and from then on entities without an owner read the new host's input.
- Local code sees no `Connected` or `Disconnected` while the match changes hands: `Session.state` is `Connecting` meanwhile, and views keep showing the last world they had. On the new host, `Session.server` becomes true.
- A player who doesn't come back to the new host within 20 seconds leaves the match.
- A host that leaves says so, and the match changes hands at once. One that stops answering is noticed after a few seconds.
- `Session.End()` ends the match for everyone instead: every player gets `Disconnected` with `Ended`, even one who missed the goodbye, as the relay keeps the room as ended for a while. So does a match whose last scene unloads.
- Only matches in a room change hands, as the room is where the players meet again. A closed match stays closed: its players come back, but no one new joins.

The new host only has what its machine could see: [private scenes](./scenes.md#private-scenes) it wasn't in are lost. A game that uses host migration shouldn't keep secrets. Players can't pass for each other: they only ever get each other's cookies hashed (see [Coming back](#players-in-the-match)).

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
- Under `tide run --web`, a reload plays on alone: the room closes, and the other players drop out.
- A match can't start in a scene that holds text or lists yet.
