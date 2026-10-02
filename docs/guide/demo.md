# Try the demo

The engine's demo, built for the web from the code below. Click it to give it the keyboard.

<iframe class="demo-frame" src="/demo/" title="Tide demo" allow="gamepad; fullscreen"></iframe>

| | Keyboard | Gamepad |
|---|---|---|
| Move | `WASD` or the arrows | Left stick |
| Fire | Space | A (south) |
| Options | Escape | Start |

[Open it on a page of its own](/demo/). It's the same simulation as the desktop build, down to the bit.

## The code

The whole demo is one file, `demo/demo.tide` in the repo. Systems move and bounce the balls every tick, and views draw them every frame. The options are local state: they change how this machine draws the game, never the game itself.

<<< @/../demo/demo.tide
