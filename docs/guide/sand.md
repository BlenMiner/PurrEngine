<script setup>
import { withBase } from 'vitepress';
</script>

# Falling sand

Sand, water and walls that everyone in a room paints into at once, built for the web from the code below. Click it to give it the mouse and keyboard.

<iframe class="demo-frame square-ish" :src="withBase('/sand/')" title="Tide sand" allow="fullscreen"></iframe>

| | Mouse and keyboard |
|---|---|
| Paint | Left mouse button |
| Erase | Right mouse button |
| Pick sand, water, wall or the eraser | `1` to `4`, or the buttons |

<a :href="withBase('/sand/')" target="_self">Open it on a page of its own</a>.

## Paint together

Press **Invite**, and the room's code shows beside it, with **Copy** to put it on the clipboard. Whoever opens this page on another machine pastes the code next to **Join** (Ctrl+V, or Cmd+V) and presses it, and you're both painting into the same sand. Up to 16 players can join.

The corner shows the frame rate, the ping, and what goes over the network each second, up and down.

Only the brushes go over the network: where each one is, what it paints, and whether it's down. Every machine runs the same simulation from them, and gets the same sand, bit for bit. A player who joins gets the world as it is then, and only the parts of it that changed since the match started.

## The code

The game is `bench/sand/sand.tide` in the repo. Its cells are a [grid](../language/grids.md) of a byte each, and `Step` is a chunk system: the grid moves a chunk at a time, on every core the machine has. It's also one of the engine's benchmarks, which bots play to measure it.

<<< @/../bench/sand/sand.tide

`bench/sand/play.tide` draws it, a rect for each run of cells of one material along a row, and has the GUI. Views only change this machine's local state (`Canvas`, `Lobby`), never the sand.

<<< @/../bench/sand/play.tide
