# Grids

A grid holds cells at positions: the pixels of a canvas, the sand of a falling-sand game, the blocks of a voxel world. `Grid2<T>` takes `int2` positions and `Grid3<T>` takes `int3` ones. Grids are fields of components, singletons and scenes, so a game can have as many as it needs: a canvas for each player, a grid for each ship, a dimension for each scene.

```csharp
component Canvas
{
    Grid2<Color> pixels = Grid2(1024, 1024);
}

singleton World
{
    Grid3<int> blocks; // No size: open every way
}
```

## Cells

`cells[x, y]` reads and writes a cell (`cells[x, y, z]` in 3D), and so does `cells[p]` for an `int2` or `int3` position `p`. A cell nobody set reads as zero.

```csharp
system Paint(Brush brush, mut Canvas canvas)
{
    canvas.pixels[brush.x, brush.y] = brush.color;
}
```

Cells are plain values: numbers, bools, enums, vectors, quaternions, matrices, colors, rects, entities, players and structs of those. Text, lists and grids can't be cells: keep them elsewhere, and a number for them in the cell. A cell is a copy, as a list's element is, so a struct cell is taken out, changed and put back:

```csharp
mut var cell = field.cells[p];
cell.heat += 1;
field.cells[p] = cell;
```

## Size

A grid's size is given when it's made: `Grid2(1024, 1024)`, or `Grid3(16, 384, 16)`. An axis given 0, or left out, has no size: it takes any position, negative ones too. A grid with no size at all is open every way, like a voxel world that goes on forever.

Nothing about a grid fails: past its size, reads give zero and writes do nothing, so paint stays on its canvas without the game checking.

- `cells.size` is its size, 0 on open axes.
- `cells.Clear()` sets every cell back to zero, and keeps the size.
- `cells = Grid2(512, 512)` gives the field a new, empty grid.

## What a grid costs

A grid keeps its cells in chunks of 64 by 64 cells, or 16 by 16 by 16 in 3D: 16 KB for four-byte cells, 4 KB for a byte enum (`enum Material : byte`), which makes a big grid a quarter of the memory to keep, hash and send. Cells bigger than four bytes make chunks of fewer cells, 16 KB at most. A chunk only exists once a cell in it is set to something other than zero, so a blank canvas costs nothing, and an open grid costs what's in it rather than the space it spans.

A match is snapshotted every tick and rolled back when a guess about another player went wrong (see [Multiplayer](./multiplayer.md)). Snapshots share the chunks nothing changed, and hashes cover each chunk on its own, so changing a few cells copies and hashes a few chunks, never the whole grid. A player joining gets only the chunks that exist.

A grid is never copied, since every chunk would be: a local can't hold one, nor a component that has one, and functions can't take grids yet. Read and change cells through the component or singleton.

A `Grid2<Color>` with a size draws as a texture: `Draw.Mesh` takes one (see [Textures](./views.md#textures)).

## Going through every cell

A grid's cells can all change every tick, like sand falling everywhere at once. A `parallel` loop goes through every cell at once, on threads:

```csharp
singleton Heat
{
    Grid2<int> cells = Grid2(1024, 1024);
}

// Each cell becomes the average of itself and the four beside it
system Spread(mut Heat heat)
{
    parallel (var at in heat.cells)
    {
        var sum = heat.cells[at] * 4 + heat.cells[at + int2(1, 0)] + heat.cells[at + int2(-1, 0)]
                + heat.cells[at + int2(0, 1)] + heat.cells[at + int2(0, -1)];
        heat.cells[at] = sum / 8;
    }
}
```

Each step reads the grid as the loop found it, and changes only its own cell, `at`. So it doesn't matter which steps run first, or on which thread: the result is exactly the same on one thread as on many, on every machine. Code after the loop runs once every step is done.

A step can declare variables, call functions (and C) and read any cell. What it can't do is change anything else: another cell, a variable from outside the loop, the whole grid (`Clear`), or anything whose order would count. tidec says so, and what to write instead. So a step can't:

- `break` out of the loop: `continue` ends the step.
- `return`, wait, or start tasks.
- Spawn, send events, add, remove or destroy, or load scenes.
- Draw or use the GUI.
- Hold another parallel loop.

To add things up, use a `for` loop, or a foreach that goes in order (see below).

A list's elements go through a parallel loop the same way, by index (see [Lists](./text-and-lists.md#going-through-every-element-at-once)).

A parallel loop goes in a system, a view, or an event handler that isn't async: not in functions, methods, async code or an input's `Sample`.

### Moving things: blocks

Sand moves: a grain leaves its cell and lands in another. A step that only changes its own cell can't do that, so `by` gives each step a block of cells instead. Each step owns its block, reads any cell, and changes only the cells in its block, so things can move within it:

```csharp
// 2 by 2 blocks: their corners are on even cells one tick, and odd ones the next
system Fall(Time time, mut Field field)
{
    parallel (var at in field.cells by 2 offset time.tick % 2)
    {
        mut var top = field.cells[at + int2(0, 1)];
        mut var bottom = field.cells[at];
        if (top == Material.Sand && bottom == Material.Empty)
        {
            top = Material.Empty;
            bottom = Material.Sand;
        }
        field.cells[at + int2(0, 1)] = top;
        field.cells[at] = bottom;
    }
}
```

Blocks never overlap, so steps never fight over a cell. `offset` says where the blocks start, and changing it each tick moves where their edges are: a grain at the bottom of a block one tick is at the top of another the next, so it keeps falling. `by int2(1, 2)` gives blocks of 1 by 2, and `offset int2(0, 1)` moves them along one axis only. A block is 1 to 64 cells along each axis, a size known while compiling: numbers, or constants. The offset can be any int, or `int2` or `int3`, and can change every time. A block that would go past the grid's size is left out, so along a sized edge some cells are only in every other tick's blocks: walls around the field keep sand off it, as the [sand demo](../guide/sand.md) does.

### In order: foreach

`foreach (var at in cells)` goes through the cells in order, rows from the first, each from its lowest x. Each step sees what the ones before it changed, as in C#: adding up a row, or anything that runs along the grid.

```csharp
system Count(Field field, mut Stats stats)
{
    mut var sand = 0;
    foreach (var at in field.cells)
    {
        if (field.cells[at] == Material.Sand) sand += 1;
    }
    stats.sand = sand;
}
```

When a foreach's steps only touch their own cell, and nothing outside the loop, the order can't show, so tidec runs them at once, like a parallel loop.

A grid with no size goes on forever, so a loop over it goes through the cells around what's been set: the chunks the grid has.

`tide schedule` shows which systems have parallel loops: `Fall  (once, 1 parallel loop)`.
