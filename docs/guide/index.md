# Introduction

Tide is a game engine built for multiplayer first. Its simulation is deterministic: given the same inputs, it gives the same results on every machine, down to the last bit of every float. So players only send each other their inputs, and players on different machines, the web included, share one world.

Games are written in **Tide**, a C#-like language built around an ECS. It compiles to C, and the C to native code or WebAssembly, together with the engine, so calls between your code and the engine's cost nothing.

::: warning Early days
Tide is young, and its language changes freely until 1.0. The [language spec](../spec.md) says which parts are decided and which are still provisional.
:::

## The big ideas

**A game is data and systems.** Entities are made of components, which are plain data. Systems are code that runs every tick, once for each entity that has the components it asks for. Singletons hold what there's one of per world, like the score or the rules.

```csharp
component Health
{
    int value = 100;
}

system Poison(mut Health health, with Poisoned)
{
    health.value -= 1;
}
```

**Systems say what they touch.** `mut` means a system writes a component, and a plain parameter that it only reads it. `with` and `without` pick entities without reading anything. From that, the compiler plans which systems can run at the same time and which have to wait, and shows you why (see [the schedule](../engine/schedule.md)).

**Everything is a match.** The simulation, called the match, is what every machine runs the same way. It runs on a server, and even single-player is a match on a server this machine runs itself. The engine has rollback netcode built in: clients predict ahead with no input delay, and correct themselves when the server disagrees. See [multiplayer](../language/multiplayer.md).

**Local state is everything else.** Menus, settings, particles and the camera belong to one machine and are never sent. **Views** read the match and draw it once per frame, and they can change local state but never the match. The compiler enforces that boundary, so a view can't break the simulation by accident.

**Input is the only way in.** What a player does reaches the match as their input, sampled once per tick and sent over the network. Everything else follows from the simulation.

## A tick and a frame

The match advances in fixed **ticks**, 60 per second by default:

1. Systems run, in a fixed order that's the same on every machine.
2. Structural changes (spawning, adding and removing components, destroying) and events apply at the end of the tick, in the order they were made.

The screen updates every **frame**, as fast as the display allows:

1. Views run, drawing the match blended between its last two ticks, so motion is smooth at any tick rate.
2. The GUI draws over the world.

## Where to go next

- [Install tide](./install.md), then make [your first game](./first-game.md).
- Read about [the language](../language/basics.md), a topic at a time.
- See how the engine keeps every machine in step: [determinism](../engine/determinism.md) and [networking](../engine/networking.md).
- [Try the demo](./demo.md), running in your browser.
