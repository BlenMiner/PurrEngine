---
layout: home

hero:
  name: Tide
  text: A networking-first game engine
  tagline: One deterministic simulation that every player shares, on desktop or in the browser, written in a language made for it.
  image:
    src: /favicon.svg
    alt: Tide
  actions:
    - theme: brand
      text: Get started
      link: /guide
    - theme: alt
      text: Try the demo
      link: /guide/sand
    - theme: alt
      text: GitHub
      link: https://github.com/BlenMiner/tide-engine

features:
  - title: Multiplayer by default
    details: Rollback netcode with full server authority. Clients have no input delay, and single-player is a match too, so there's only one path to get right.
  - title: Deterministic floats
    details: The same build and the same inputs give the same bits on every platform, the web included. Plain floats, no fixed point.
  - title: A language for the ECS
    details: A C#-like language built around the ECS. Systems say what they read and write, and the compiler works out the rest. It compiles to C, then to native code or WebAssembly.
  - title: A schedule you can read
    details: The compiler knows every system's data, so it plans which ones can run at the same time, and tells you why the others wait.
  - title: The web is a real platform
    details: Every game also builds as one self-contained web page on WebGPU or WebGL 2, running the same simulation as desktop.
  - title: Nothing to set up
    details: One tide command, with clang built in. Write a .tide file and run tide run. Editor support for VS Code and JetBrains IDEs.
---

## A whole game

A game is a folder of `.tide` files, and needs no other code. This one bounces a ball:

```csharp
component Ball
{
    float2 position;
    float2 velocity = float2(160, 120);
}

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Ball);
}

system Move(mut Ball ball, Time time)
{
    ball.position += ball.velocity * time.dt;
    if (Math.Abs(ball.position.x) > 300) ball.velocity.x = -ball.velocity.x;
    if (Math.Abs(ball.position.y) > 200) ball.velocity.y = -ball.velocity.y;
}

view DrawBalls(Ball ball)
{
    Draw.Circle(ball.position, 20, Color.red);
}
```

Save it as `game.tide`, then run `tide run` in its folder. [Install tide](./guide/install) to get started, or see [your first game](./guide/first-game) for a longer tour.
