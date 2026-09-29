# Views and drawing

A **view** draws the game. It looks like a system and takes the same parameters, but it runs once per rendered frame instead of once per tick, and it never changes the match.

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

Drawing is immediate mode: views call `Draw` functions every frame, and nothing is kept between frames. Views run after the frame's ticks, in the order they're declared, and later ones draw on top.

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

Each frame starts black, with the camera at the origin and one world unit per pixel.

## Colors

`Color` has `r`, `g`, `b` and `a`, floats from 0 to 1, as in Unity. `Color(r, g, b)` has alpha 1, and `Color(r, g, b, a)` sets it. The constants are `Color.white`, `black`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `gray` and `clear`. Components and singletons can hold colors.

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
event(Died dead) Respawn(Entity self, mut Body body, Arena arena)
{
    body.position = arena.start;
    self.Snap(); // No sliding from where it died
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

Views can read `Devices`, this machine's input devices, for the frame: `.down` and `.up` since the last frame, and the mouse's `delta` and `scroll` too. What the GUI is using is hidden from them. A function that reads `Devices` needs the frame, like one that draws, so only views and the functions they call can call it.
