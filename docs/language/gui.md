# GUI

The GUI is immediate mode, like drawing: views call widgets every frame, and they're drawn over the world. It follows Unity's IMGUI.

```csharp
local singleton Settings
{
    bool open;
    bool fullscreen;
    float volume = 1;
}

view Options(mut Settings settings)
{
    if (GUI.Button(Rect(Screen.width - 210, 10, 200, 40), "Options")) settings.open = true;
    if (Devices.keyboard.escape.down || Devices.gamepad.start.down) settings.open = true;

    GUILayout.Modal(Anchor.MiddleCenter, settings.open)
    {
        GUILayout.Toggle("Fullscreen", settings.fullscreen);
        GUILayout.Slider("Volume", settings.volume, 0, 1);
    }
}
```

The GUI is local: widgets change this machine's state, never the match. To act on the match, put it in the [input](./input.md).

## Widgets change what you pass them

Widgets edit values through `mut` parameters, and return whether they changed them: `GUILayout.Toggle("Fullscreen", settings.fullscreen)` flips `settings.fullscreen` when it's clicked. A button returns whether it was pressed.

The `mut` argument is the variable itself, so its type matches exactly: `Slider` takes a `float` variable, not an `int`.

Widgets have no IDs to write. They're told apart by where they're called from and the entity the view runs for.

## GUILayout and GUI

`GUILayout` lays widgets out for you: they stack top to bottom, and containers arrange them. `GUI` has the same widgets, each at a `Rect` you give it first, as in Unity: `GUI.Button(rect, "Quit")`.

**Widgets**, with `GUI`'s taking a `Rect` first:

| Widget | What it is |
|---|---|
| `Label(text)` | Text |
| `Button(text)` | A button; returns whether it was pressed |
| `Toggle(text, mut bool value)` | A checkbox |
| `Slider(label, mut float value, min, max)` | A slider |
| `IntSlider(label, mut int value, min, max)` | A slider for ints |
| `TextField(label, mut string text)` | A field the player types into |
| `IntField`, `FloatField`, `Float2Field`, `Float3Field`, `Float4Field` | Number fields: a label and a `mut` value |
| `ColorField(label, mut Color value)` | A swatch, and fields for r, g, b and a |
| `GUILayout.Space(size)` | Empty space |

**Containers** take a block (see [Blocks](./functions.md#blocks)):

| Container | What it does |
|---|---|
| `GUILayout.Vertical() { ... }` | Stacks its widgets top to bottom |
| `GUILayout.Horizontal() { ... }` | Puts its widgets side by side |
| `GUILayout.Area(anchor) { ... }` | A panel sized to its content, at one of nine anchors |
| `GUILayout.Area(rect) { ... }` | A panel at a rect |
| `GUILayout.Modal(anchor, mut bool open) { ... }` | A panel over the whole screen while `open` is true, like a pause menu |

```csharp
view Hud(mut Look look)
{
    GUILayout.Area(Anchor.UpperLeft)
    {
        GUILayout.Horizontal()
        {
            if (GUILayout.Button("Options")) look.open = true;
            GUILayout.Label("v1.0");
        }
    }
}
```

The engine builds nothing a game couldn't build itself: containers are functions with a `Block`, and you can write your own the same way.

## Screen units

Positions and sizes are in units of a screen 1080 tall, whose width follows the window's shape, so a GUI laid out once fits every window. `Screen.width` and `Screen.height` are in those units, and `Screen.scale` is pixels per unit.

`Rect(x, y, width, height)` is measured from the top left corner, with `y` down, as in Unity's GUI. (World drawing and the mouse have `y` up.)

`Anchor` names the nine places an area can go, as Unity's `TextAnchor` does: `UpperLeft`, `UpperCenter`, `UpperRight`, `MiddleLeft`, `MiddleCenter`, `MiddleRight`, `LowerLeft`, `LowerCenter` and `LowerRight`.

## Modals

While a modal is up, it has the focus, the widgets outside it don't work, and the game and views get nothing from the devices. Back (Escape or the gamepad's east button) closes it. The modal drawn last is on top.

## Keyboard and gamepad

Navigation is built in. Tab and Shift+Tab move the focus between widgets, in the order they were drawn, and so do the arrows, the d-pad and the left stick once a widget has the focus. Enter, Space and the south button press, and Escape and the east button go back. Left and right step a focused slider or number field.

Typing into a field uses the characters the player types, which follow their keyboard layout, not keys by position. Clicking a number field, or pressing Enter on it, starts typing into it; Enter or leaving the field keeps a valid number, and Escape keeps the old one.

## The GUI and input

Whatever the GUI is using, such as a click on a button or typing in a field, is hidden from the input's `Sample` and from views' `Devices`. So clicking a button never fires a weapon, and typing a name never moves the player.
