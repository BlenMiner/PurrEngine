# Views and drawing

A **view** draws the game. It looks like a system and takes the same parameters, components and singletons, but it runs once per rendered frame instead of once per tick, and it never changes the match. It reads this machine's devices as `Devices`, not as a parameter, and can't read the input (see [Input](./input.md#devices-in-the-match)).

```csharp
view DrawBalls(Body body, Ball ball)
{
    Draw.Circle(body.position, body.radius, ball.color);
}

view DrawHud(Arena arena)
{
    Draw.Camera(float2(0, 0), arena.halfSize.y);
    Draw.Text("fire: space", float2(-arena.halfSize.x, arena.halfSize.y), 18, Color.gray);
}
```

Drawing is immediate mode: views call `Draw` functions every frame, and nothing is kept between frames. Views run after the frame's ticks, in the order they're declared, and later ones draw on top. `[Before]` and `[After]` order views among themselves, as they do systems. A frame can draw as much as it needs: shapes go to the GPU together, so tens of thousands of rects or circles a frame are cheap, on the web too.

A view's `mut` parameters, `Spawn`, `Add`, `Remove`, `Destroy` and `Send` are all local: views can change this machine's own state, like particles or a menu, but not the match (see [Local state](./local-state.md)). Only views, and the functions they call, can draw.

## Draw functions

Positions and sizes are in world units, with `y` up. The camera maps them to the screen.

| Function | What it draws |
|---|---|
| `Draw.Clear(color)` | Fills the screen |
| `Draw.Camera(center, size)` | Sets the camera for the calls after it: `center` is the world position at the middle of the screen, and `size` is half the visible height, like Unity's orthographic size |
| `Draw.Circle(center, radius, color)` | A filled circle |
| `Draw.WireCircle(center, radius, color)` | A circle's outline |
| `Draw.Rect(center, size, color)` | A filled rectangle |
| `Draw.WireRect(center, size, color)` | A rectangle's outline |
| `Draw.Line(from, to, color)` | A line |
| `Draw.Text(text, position, size, color)` | Text, with `position` its top left corner and `size` its height |
| `Draw.Mesh(vertices, indices)` | Triangles, with a color at each corner (see [Meshes](#meshes)) |
| `Draw.Mesh(vertices, indices, texture)` | Triangles drawn with a grid of colors (see [Textures](#textures)) |
| `Draw.Clip(rect)` | Only what's inside `rect` draws, for the calls after it; `Draw.Clip()` draws everywhere again (see [Clipping](#clipping)) |
| `Draw.Screen()` | The calls after it are in the screen's pixels, from the top left, with `y` down (see [On the screen](#on-the-screen)) |

Each frame starts black, with the camera at the origin and one world unit per pixel.

## Colors

`Color` has `r`, `g`, `b` and `a`, floats from 0 to 1, as in Unity. `Color(r, g, b)` has alpha 1, and `Color(r, g, b, a)` sets it. The constants are `Color.white`, `black`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `gray` and `clear`. Components and singletons can hold colors.

## Meshes

`Draw.Mesh(vertices, indices)` draws triangles, the shape every other one can be made of. `vertices` is a `List<Vertex>`, the corners, and `indices` a `List<int>`: every three are a triangle, each a corner's place in `vertices`, so triangles share corners.

```csharp
struct Vertex // Built in
{
    float2 position;
    float2 uv;                 // Where in the texture: (0, 0) to (1, 1)
    Color color = Color.white;
}
```

```csharp
view Arrows(Body body)
{
    List<Vertex> corners = [
        Vertex { position = body.position + float2(0, 1), color = Color.red },
        Vertex { position = body.position + float2(-1, -1), color = Color.blue },
        Vertex { position = body.position + float2(1, -1), color = Color.blue },
    ];
    Draw.Mesh(corners, [0, 1, 2]);
}
```

Each pixel of a triangle is its corners' colors, blended across it, over what's behind it by its alpha. Triangles draw whichever way round their corners go, in the order they're given, among everything else a view draws. An index past the end of `vertices` leaves its triangle out.

A mesh is one call however many triangles it has, and meshes drawn one after another go to the GPU together, so thousands of triangles a frame are cheap.

## Textures

A texture is a grid of colors with a size, `Grid2<Color>` (see [Grids](./grids.md)). Give one to `Draw.Mesh`, and each pixel is the triangle's color times the grid's at its `uv`: (0, 0) is the outer corner of cell (0, 0), and (1, 1) the outer corner of the last cell.

```csharp
local singleton Art
{
    Grid2<Color> checker = Grid2(8, 8);
    bool painted;
}

view Board(mut Art art)
{
    if (!art.painted)
    {
        foreach (var at in art.checker)
        {
            art.checker[at] = (at.x + at.y) % 2 == 0 ? Color.white : Color.black;
        }
        art.painted = true;
    }
    List<Vertex> corners = [
        Vertex { position = float2(-4, -4), uv = float2(0, 0) },
        Vertex { position = float2(4, -4), uv = float2(1, 0) },
        Vertex { position = float2(4, 4), uv = float2(1, 1) },
        Vertex { position = float2(-4, 4), uv = float2(0, 1) },
    ];
    Draw.Mesh(corners, [0, 1, 2, 0, 2, 3], art.checker, Filter.Point);
}
```

There's nothing to create, upload or free. The grid is ordinary state: change its cells whenever, with `[x, y]`, a loop or `Clear()`, and the next frame draws what it holds. The engine keeps a copy on the GPU while views draw with it, and sends the cells again only when they changed. Nothing holds a handle, so hot reloading carries a texture as it carries any state.

The grid can be local state or the match's, like a canvas each player paints. Views read the match's grids as they are at the latest tick: cells aren't blended.

The last argument says how the texture is read between cells: `Filter.Bilinear` blends them, and is what a call without it does; `Filter.Point` takes the nearest, for pixel art. Past 0 and 1, `uv` reads the cells at the edge.

- A cell is four floats, and the GPU's copy a byte a channel: values are clamped to 0 to 1.
- A grid with an open axis has no size, so it's no texture: the mesh draws with its colors alone.
- How big a texture can be is the GPU's limit: at least 2048 by 2048 wherever Tide runs.

A game's C can draw meshes too, with pixels of its own, like a font atlas a library made: see [Drawing from C](./c-functions.md#drawing-from-c).

## Clipping

`Draw.Clip(rect)` keeps what's drawn after it inside a rectangle: meshes, shapes, text, and `Draw.Clear`, which then fills the rectangle. `Draw.Clip()` draws everywhere again.

```csharp
view Panel()
{
    Draw.Screen();
    Draw.Clip(Rect(20, 20, 200, 100));
    Draw.Clear(Color.black); // The panel only
    Draw.Text("A line too long for the panel is cut off at its edge", float2(28, 28), 18, Color.white);
    Draw.Clip();
}
```

The `Rect` goes from (x, y) to (x + width, y + height), in the units the calls are in: the world's under a camera, where `y` is up, and pixels after `Draw.Screen()`. It stays where it is on the screen when the camera changes afterwards. Each frame starts with none, and a view's clip never clips the GUI.

## On the screen

After `Draw.Screen()`, Draw calls are in the screen's pixels: from the top left, with `y` down, as the GUI's are, `Screen.width` by `Screen.height`. `Draw.Camera` goes back to the world.

```csharp
view Health(Stats stats)
{
    Draw.Screen();
    Draw.Rect(float2(110, 30), float2(200, 20), Color.gray);
    Draw.Rect(float2(10 + stats.health, 30), float2(stats.health * 2, 20), Color.red);
}
```

What views draw there is under the GUI's widgets, which draw last.

## Smooth at any tick rate

Views draw at whatever frame rate the game renders, not the tick rate, and see the match blended between its last two ticks. So motion is smooth even at 20 ticks per second, at the cost of drawing up to a tick late.

Every float, vector, quaternion and color a view reads of the match is blended. Ints, bools, enums, entities and text are as they are at the latest tick. An entity that wasn't there last tick is drawn as it is.

`[Snap]` keeps a field out of blending, for angles that wrap and values that jump:

```csharp
component Body
{
    float2 position;        // Blended
    [Snap] float heading;   // Wraps from 360 to 0: as it is
    int lives;              // Ints are as they are
}
```

### Jumps

Something that jumps, like a respawn, a portal or a camera cut, says so from match code: `entity.Snap()` or `singleton.Snap()`. For the tick it happens in, views draw it as it is, instead of sliding from where it was.

```csharp
event(Died dead) Respawn(mut Body body, Arena arena)
{
    body.position = arena.start;
    this.Snap(); // No sliding from where it died
}
```

`entity.Snap()` is applied at the end of the tick, like `Destroy`, so it never makes systems wait. `singleton.Snap()` needs the singleton as a `mut` parameter.

### Blending your own way

A struct or component can say how it blends with an `Interpolate` method, declared in it like an operator. It replaces the default blend for that type wherever views see it:

```csharp
struct Angle
{
    float degrees;

    Angle Interpolate(Angle from, Angle to, float t)
    {
        mut var d = to.degrees - from.degrees;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return Angle { degrees = from.degrees + d * t };
    }
}

component Body
{
    float2 position; // Blended as usual
    Angle heading;   // Blended the short way round
}
```

`Interpolate` isn't for code to call. Singletons have no methods, so a singleton blends its own way through a struct in it that has one.

## Reading this machine's devices

Views can read `Devices`, this machine's input devices, for the frame: `.down` and `.up` since the last frame, and the `delta` of the mouse, touches and the pointer, and the mouse's `scroll`, too. What the GUI is using is hidden from them. A function that reads `Devices` needs the frame, like one that draws, so only views and the functions they call can call it.
