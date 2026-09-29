# The schedule

Every system declares what it reads and writes, so the compiler knows, before the game runs, which systems can run at the same time and which have to wait. That plan is the **schedule**.

::: info
The tick doesn't run on threads yet: systems run one after the other, in order. The schedule is already worked out, and shown to you, so games are written for it from the start. Running in parallel will never change results.
:::

## Reading it

`purr schedule` prints it. This is the [demo's](../guide/demo.md):

```
demo: 5 systems each tick, 4 stages deep; 5 can run alongside others.
A system starts once everything it runs after is done.

stage 1  Steer  (per entity)
         writes Body, Aim
         alongside Expire
stage 2  Fire  (per entity)
         reads Body, Aim
         after Steer: it writes Body and Aim, which this reads
         alongside Expire
stage 3  Move  (per entity)
         reads Time
         writes Body
         after Steer: both write Body (and through Fire)
         after Fire: it reads Body, which this writes
         alongside Expire
stage 4  Bounce  (per entity)
         reads Arena
         writes Body
         after Steer: both write Body (and through Fire)
         after Fire: it reads Body, which this writes (and through Move)
         after Move: both write Body
         alongside Expire
stage 1  Expire  (per entity)
         writes Lifetime
         alongside Steer, Fire, Move, Bounce

Views, every frame in this order (they only read, so nothing waits):
         DrawArena
         DrawBalls
         DrawPlayers
         Options

Event handlers, at the end of the tick, as their events are sent:
         Spawned: Setup
```

`Expire` only touches `Lifetime`, so it can run alongside everything else. The others all work on `Body`, so they wait for each other, in the order they're written.

In the editors, each system shows its stage and why it waits right above it, like `stage 2 · after Move: both write Body`, and on hover.

## When systems wait

Two systems **conflict** when one writes a component or singleton the other reads or writes. Conflicting systems keep their order in the tick; `[Before]` and `[After]` order systems too. A system starts as soon as everything it waits for is done, with no barriers between stages: a system's stage is only how deep it is in that chain.

Some things never make systems wait:

- **Filters.** `with` and `without` read nothing.
- **Entities that can't overlap.** Two systems that write the same component don't conflict if they can never touch the same entity, which the compiler proves from the archetypes: `with Player` against `with Enemy`, when nothing is both.
- **Structural changes and events.** `Spawn`, `Add`, `Remove`, `Destroy` and `Send` are recorded and applied at the end of the tick, in order. Event handlers run then too, so they aren't part of the tick's schedule.
- **Input and `Time`**, which systems only read.

Two systems that change the match's text or lists do conflict, since they share its heap.

## Keeping it wide

- Take a component as `with` when a system only needs it to pick entities.
- Don't declare `mut` for what a system only reads. Unused access is a warning, and the editors offer a quick fix for it.
- Splitting a big component into the parts different systems use lets them run side by side.

The other kind of parallelism, a system splitting its entities across threads, doesn't change results either, and needs nothing from you.

## In CMake builds

In a CMake build of the engine's repo, `<game>_schedule` is a build target that prints it, and `purrc --schedule` prints it for any files.
