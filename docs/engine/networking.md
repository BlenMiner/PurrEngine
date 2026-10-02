# Networking

Tide uses rollback netcode with full server authority. The server is the source of truth, and clients predict ahead of it, rolling back when it disagrees. Since the simulation is [deterministic](./determinism.md), what goes over the network is mostly inputs.

For how a game starts, hosts and joins matches, see [Multiplayer](../language/multiplayer.md). This page is about what happens underneath.

## One path for everything

Single-player is a match too: the machine runs a server itself, and connects to it through a loopback transport. There's no separate offline path, so what works alone works online.

A minimized window or a browser tab in the background doesn't stop a game: it keeps running there without drawing, so the match goes on for the other players. Browsers slow a hidden page's timers and stop its animation frames, so a hidden page's frames come from a worker's timers instead. Meanwhile it runs a frame when its match is due a tick, at the game's `tickRate`, and none in between. When the machine that runs the server does stop for a while, like a program paused in a debugger, or a browser that freezes the page (a phone putting the browser away), the match stops with it, and goes on from where it was when the machine comes back.

## What the server sends

The server ticks the one true world. Every tick, it sends each player:

- who joined or left,
- the inputs that changed,
- and a hash of the world after the tick.

An input that didn't change, or didn't arrive, keeps its last value, on every machine. One that changed goes as what changed: a bit for each value that didn't. Players send theirs the same way, each tick's from the tick before. Its ints and enums take as little as they need: a number from -64 to 63 takes a byte, one up to about 8,000 either way two, and so on, and one that changed goes as by how much, so a big number that moves a little takes a byte too. A float that changed goes as the bits that changed, which for a value that moved a little, or a round one, is far fewer than its 32. Nothing is rounded: every value arrives exactly as it was sent, bit for bit.

## Prediction and rollback

Clients have no input delay. A client runs ahead of the server by about half the round trip, plus a small margin, so its inputs arrive in time for the tick they're meant for. Its own input applies at once, and only other players' inputs are guessed, by repeating their last one. In a game without an input there's nothing to get there in time, so clients keep the small lead they joined with.

A client keeps a snapshot of every tick from the last one the server confirmed, the **verified tick**, to the one it's predicting. When the server's tick arrives:

- If it went as the client guessed, the client only checks its hash against its snapshot.
- If an input was different, the client goes back to the snapshot before that tick, and runs it and the ticks after it again with the right inputs.
- If a hash differs anyway, the client gets the world again from the server: only the pages its own world lacks (see [Sending worlds](#sending-worlds)).

A client predicts at most a second ahead of the verified tick; beyond that, it waits for the server. The server keeps four seconds of ticks to send again, and a player further behind gets the whole world. A player that was sent the world has every tick since kept for it until it catches up, however long a big world took to arrive.

Snapshots are cheap, which is what makes this work. The world is plain data, with no pointers, and everything past what's in use is zero, so a snapshot is a copy of the bytes in use, and a hash covers only those. Their cost follows what's in the world, not how big it could be.

## Sending worlds

A world goes over the network page by page, as a delta: each page is either the same as the one the receiver has in its place, or its bytes, with runs of zeros packed small. Where its rows or cells are like the ones before them (entities lined up, a grid of sand), a page goes as how each one differs from the one before, which is mostly zeros too. Every byte arrives exactly as it was. A player with a world like the match's, such as its own after a hash differed, or the last one it had of a match that changed hands, first gets a hash of each page, says which ones its world lacks, and gets only those. A player joining with nothing does the same with the match as it started, which it starts itself, when starting a match calls no C (see [C functions](../language/c-functions.md#what-c-is-trusted-with)): what never changed since, like a level the match made as it started, never goes over the network. A world small enough to go in one update goes whole, as do worlds of games whose start calls C, and of matches that went on from another's world. A world that doesn't come out as the server's hash says is asked for again, whole.

## Views and prediction

Views draw the predicted world, blended between its last two ticks (see [Views](../language/views.md#smooth-at-any-tick-rate)). A tick that's run again after a rollback just changes what the next frame draws.

## Joining

A player who joins gets the whole world and a cookie. Joining the same server again with the cookie gets them their `PlayerID` back, and with it everything the game kept for them.

## Host migration

With the game's `hostMigration` setting, the server tells every player what they need for its match to go on without it: its room's code, the room's key (which lets only them take the room over), which players are in the match, and the SHA-256 of each one's cookie. When the server leaves or stops answering, its players go to the room again. The relay pings the room's host: if it's gone, the first player there hosts the room, and the relay introduces the others to it. That machine runs the server from the last tick it verified, with the players already in its world, and the others join it with their cookies, which it checks against the hashes. Their own last worlds are nearly the new server's, so they only get the pages theirs lack. A match that ends says so at the relay too, which keeps its room as ended for five minutes: a player who missed the goodbye and comes back to take the match over is told it ended. See [Multiplayer](../language/multiplayer.md#host-migration).

## Reliability

Everything is sent again until it's acknowledged. Nothing waits on a reliable stream, so one lost packet never holds up the ones after it.

## Transports

On desktop, matches run over Tide's own thin layer on UDP. On the web, they run over WebRTC data channels that neither order nor resend, like UDP, so a browser can host matches as well as join them. The protocol above the transport is the same.

Players find each other in rooms, by a code (see [Multiplayer](../language/multiplayer.md#rooms)), whether they're in a browser or a desktop game: desktop games speak WebRTC too, with an implementation of Tide's own. A relay of ours introduces them, passing along what WebRTC needs to connect them, and their packets then go straight between them. Players whose networks can't connect directly go through a TURN server, which carries their packets. Everything to and from the relay is encrypted (`wss://`), on desktop with the system's own TLS, and so are the matches, as WebRTC requires.

## Who sees what

The server can keep state from players: a [private scene](../language/scenes.md#private-scenes) is only sent to the players given it. Secrets like random seeds go in one with no players, which only the server has. Code that reads it only predicts correctly where it's seen, and the server corrects the others.

## Coming later

- Telling predicted state from verified state in game code, for example to wait until a player's death is verified before showing it.
- Browsers joining matches.
- Servers with no window and no player of their own, lobbies, and finding matches.
