# Input

A player's **input** is what they do in one tick, and it's what the network sends. Each game declares one `input`, and systems read it.

```csharp
input PlayerInput
{
    float2 move;
    bool jump;

    Sample()
    {
        var keys = Devices.keyboard;
        if (keys.d.pressed) move.x += 1;
        if (keys.a.pressed) move.x -= 1;
        move += Devices.gamepad.leftStick;
        jump = keys.space.pressed || Devices.gamepad.buttonSouth.pressed;
    }
}

system Jump(PlayerInput input, mut Velocity velocity)
{
    if (input.jump.down)
        velocity.value.y = 5;
}
```

## Sampling

The engine calls the input's `Sample` method on the player's own machine, once per tick. Its fields start at their defaults, and `Sample` assigns them by name, without `mut`.

`Sample` runs outside the match. It reads this machine's `Devices`, and the local singletons it takes as parameters, like `Sample(Settings settings)`, but never the match. It's the place to work out what the match needs from things only this machine knows: an aim direction from the mouse, which is in this machine's window, or anything that depends on its settings.

## Who gets which input

An input parameter gives a system the input of the player who **owns** the entity it's running for. `Owner` is a built-in component that ties an entity to a player:

```csharp
Spawn(Body, Owner { player = joined.player });
```

It's a normal component: it can be read, written, added and removed, and writing it hands control over.

Entities without an `Owner`, or whose owner isn't a known player, get the **server's input**, and so do systems that run once per tick. That's how the server controls what no player owns. An input parameter doesn't pick entities: add `with Owner` to only run on owned ones.

Players are `PlayerID`s, not ints or connections. Like `Entity`, a `PlayerID` is opaque, compared with `==`, and has a null value. A player who reconnects gets their `PlayerID` back, and keeps everything they owned. `PlayerID(0)` names a player by number, for local play and tests.

## Held, not pressed

Input carries whether buttons are held, not whether they just went down: when a player's input is late, the other machines guess it by repeating their last one, and a repeated "just went down" would press twice. In the match, `bool` fields of the input get `.down` and `.up`, worked out against the previous tick, inside structs too: `input.aim.fire.down`.

## Devices

`Devices` has a keyboard, a mouse and a gamepad for now. Every button has `.pressed` (held), `.down` (went down since the last sample) and `.up` (went up). On a device, `.pressed` means held at any point since the last sample, so a quick tap between ticks is never lost.

- **Keyboard:** every key by its position, named after the US layout: `keys.w`, `keys.space`, `keys.leftShift`, `keys.digit1`, `keys.upArrow`, `keys.f1`. `WASD` is in the same place on AZERTY.
- **Mouse:** `position`, `delta` and `scroll` (`float2`), and the buttons `left`, `right` and `middle`. `position` is in window pixels from the bottom left.
- **Gamepad:** `connected`, `leftStick` and `rightStick` (`float2`), `leftTrigger` and `rightTrigger` (0 to 1), the face buttons by position (`buttonSouth`, `buttonEast`, `buttonWest`, `buttonNorth`), `dpad.up` and the other directions, `leftShoulder`, `rightShoulder`, `start` and `select`.

Axes follow Unity: `y` is positive up, for sticks and the mouse, and `scroll.y` is positive scrolling away from you.

### Devices in the match

Match code can read devices too, with a `Devices` parameter: the devices of the player who owns the entity, like an input parameter. The input then sends what match code reads of them, and nothing else. A game can do without an `input` declaration this way:

```csharp
system Hop(Devices devices, mut Velocity velocity)
{
    if (devices.keyboard.space.down || devices.gamepad.buttonSouth.down)
        velocity.value.y = 5;
}
```

Match code can't read this machine's `Devices` directly; the error says to take the parameter.

## Input is an attack point

Inputs come from other players' machines, so the engine doesn't trust them:

- NaN and infinite floats become the field's default.
- Fields can declare bounds, `[Clamp(lo, hi)]`, `[Min(x)]` and `[Max(x)]`, which the engine applies to every input. A number bounds every component of a vector.
- Then the input's `Sanitize` method runs, if it has one, for what bounds can't say.

```csharp
input PlayerInput
{
    [Clamp(-1, 1)] float2 move;
    [Min(0), Max(3)] int gear = 1;
    bool boost;

    Sample() { move = Devices.gamepad.leftStick; }

    // After the bounds: what attributes can't say.
    Sanitize()
    {
        if (gear == 0) boost = false;
    }
}
```

Every input goes through these steps before the match reads it, including this machine's, so systems can rely on them without checking again. Devices are repaired the same way, since the engine knows their ranges.

An input holds numbers, bools, enums, vectors and structs of those; not text or lists. Up to 16 players for now.
