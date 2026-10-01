# The schedule

Every system declares what it reads and writes, so the compiler knows, before the game runs, which systems can run at the same time and which have to wait. That plan is the **schedule**.

The tick follows it on every core: a system starts as soon as what it waits for is done, and big systems split their entities across threads too. Running in parallel never changes results, so every machine still gets the same match. A small tick, where waking the other threads would cost more than they'd save, runs on one thread, and so does every tick on the web.

## Reading it

`tide schedule` prints it. This is the [demo's](../guide/demo.md):

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
- **Structural changes and events.** `Add`, `Remove`, `Destroy` and `Send` are recorded and applied at the end of the tick, in order. Event handlers run then too, so they aren't part of the tick's schedule.
- **Input and `Time`**, which systems only read.
- **C.** Calls to C are trusted: they make no system wait, and a system that calls C splits across threads like any other. What C does when it's called from several threads at once is the game's to get right.

Some things are the whole match's, so systems wait for each other over them whatever entities they run for:

- **Text and lists.** Two systems that change the match's text or lists conflict, since they share its heap. Reading text alongside them is fine.
- **Spawns.** Entities get their IDs in order. A system that runs on one thread gives a new entity its ID as it spawns, so it waits for the systems before it that spawn. A system that splits its entities across threads doesn't wait: see below.

## Keeping it wide

- Take a component as `with` when a system only needs it to pick entities.
- Don't declare `mut` for what a system only reads. Unused access is a warning, and the editors offer a quick fix for it.
- Splitting a big component into the parts different systems use lets them run side by side.

## Splitting across threads

The other kind of parallelism is a system splitting its entities across threads: each thread takes a chunk of them, up to 1,024. It doesn't change results either, and needs nothing from you, but a system only splits when nothing it does has to happen in order across its entities. One that changes text or lists, or changes a singleton, runs on one thread, alongside the others. Everything a system records (spawns, adds, removes, destroys, events) is applied in the order one thread would have recorded it.

A system that splits can spawn. Its `Spawn` gives a temporary handle, which works like any other: store it in the entity's components, `Add` to it, `Send` to it or put it in an event. Once the system is done, its new entities get their IDs, in the order one thread would have given them, and every handle it kept (in the components it changes, the changes it recorded and its events) becomes the real one, before any system that waits for it starts. Only while the system runs does the difference show: text shows a temporary handle as `Entity(new)`, and C can tell with `tide_entity_is_temporary` (see [C functions](../language/c-functions.md)).

## In CMake builds

In a CMake build of the engine's repo, `<game>_schedule` is a build target that prints it, and `tidec --schedule` prints it for any files.
