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

Cells are plain values: numbers, bools, enums, vectors, colors, entities and structs of those. A cell is a copy, as a list's element is, so a struct cell is taken out, changed and put back:

```csharp
var cell = field.cells[p];
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

## Chunk systems

A grid's cells can all change every tick, like sand falling everywhere at once. A chunk system runs once for each chunk, on threads: its `chunk` parameter names the grid field, and `min` and `max` are its chunk's cells.

```csharp
singleton Field
{
    Grid2<int> cells = Grid2(2048, 2048);
}

// Ones fall a cell a tick, into the chunk below too
system Fall(chunk mut Field.cells cells)
{
    for (var y = Math.Max(cells.min.y, 1); y < cells.max.y; y++)
    {
        for (var x = cells.min.x; x < cells.max.x; x++)
        {
            if (cells[x, y] != 1 || cells[x, y - 1] != 0) continue;
            cells[x, y] = 0;
            cells[x, y - 1] = 1;
        }
    }
}
```

Fall reads and writes `cells[x, y - 1]`, a cell below its own, so it reaches into the chunk below: tidec works that out from the cells it indexes. Chunks then run in phases, placed so no two that run together touch the same chunk: Fall's run in 2, every other row of chunks at a time. The result is exactly the same on one thread as on many, on every machine. Sand that also slides to the sides gets into 6 chunks, and runs in 6 phases. Heat that spreads to the four cells beside each one gets into the chunks beside its own but never those across a corner, and runs in 5, where a whole block of 9 would take 9. Work that stays inside each chunk, like filling it in, runs every chunk at once.

tidec works it out from `cells.min` and `cells.max`, constants, loop variables between them, and locals made from those. A cell it can't place from the chunk, like one at a position read from a singleton, is an error. Index it from the chunk instead, or say how far it reaches above the system: the cells around each cell it touches, or how many cells past its chunk it touches every way.

```csharp
[Reach(int2(-1, 0), int2(1, 0))] // The cells to the left and right of each
system Gust(Wind wind, chunk mut Field.cells cells) { ... }

[Reach(2)] // Up to 2 cells past its chunk, every way
system Blur(Brush brush, chunk mut Field.cells cells) { ... }
```

`[Reach]` where tidec works the reach out is a warning, and the editor offers to remove it.

A chunk system changes only the cells within its reach. It reads singletons, and the other components of its grid's entity (which is `this`), but it can't spawn, send or change entities yet, nor take the component or singleton its grid is in: the other chunks are changing meanwhile, so it reads the grid through its `chunk` parameter. It runs for the chunks that existed when the tick began; chunks made during a tick join in from the next one.

`[Sleeps]` makes a chunk system skip chunks where nothing within its reach changed in the last tick: settled sand, or water that's still. It's for systems where a chunk whose surroundings didn't change can't change either, whatever else they read.

```csharp
[Sleeps]
system Flow(chunk mut World.water water) { ... }
```

`tidec --schedule` shows each chunk system's phases, how far it reaches, and what it waits for: `Fall  (per chunk, 2 phases, reaches y -1)`, or `Spread  (per chunk, 5 phases, reaches x -1..+1, y -1..+1, 5 of those 9 chunks)`.
