# Text and lists

Text and lists are values, like everything else: assigning one copies it, and changing a copy never changes the original. Nothing about them fails, either: past the end, positions are clamped.

## Text

`string` is text, in UTF-8. Literals are written in double quotes, with the escapes `\"`, `\\` and `\n`.

```csharp
component Nameplate
{
    string name = "Player";
}

system Greet(mut Nameplate plate, Stats stats)
{
    plate.name = $"{plate.name.Trim()} ({stats.wins} wins)";
}
```

### Joining and formatting

- `+` joins text with anything it can show: `"score " + score`, `1 + "st"`.
- `$"score {score}"` puts values in text. After a value, a colon and a format, as in C#: `{x:F2}` for two decimals, `{n:D3}` for at least three digits (`007`), `{n:X}` for hex. `{{` and `}}` are braces, and `?:` in a value goes in parentheses: `{(won ? 1 : 0)}`.

Text can show numbers, bools, enums (their member's name), vectors and quaternions (`(1, 0.5)`), `Color`, `Rect`, entities (`Entity(3:1)`, or `Entity(new)` for one a system splitting its entities across threads just spawned, whose ID comes once it's done) and players (`PlayerID(0)`). Floats are written with the fewest digits that read back as the same float, exactly the same on every platform.

It shows whole values too, which is handy for debugging. Structs, components, singletons, events and inputs show every field, the way C# shows records, and lists show their elements. Text inside them is in quotes, so empty text still shows:

```csharp
GUILayout.Label($"{session}");
// Session { state = Connected, player = PlayerID(0), ping = 0, server = true, open = false, room = "" }
GUILayout.Label($"{scores}");
// [3, 1, 2]
```

Matrices can't be shown yet, so neither can a value that holds one.

### Members

- `length` counts characters (Unicode code points), not bytes.
- `Contains`, `StartsWith`, `EndsWith` and `IndexOf`, which gives -1 when it's not there.
- `Substring(start)` and `Substring(start, length)`.
- `ToUpper` and `ToLower`, for ASCII letters for now.
- `Trim` and `Replace(from, to)`.
- `==` and `!=` compare text byte by byte. There's no `<` for text.

## Lists

`List<T>` is a list of values. It starts empty, and `[a, b, c]` makes one where the type is known from where it goes:

```csharp
component Inventory
{
    List<int> items = [1, 2, 3];
}

system Collect(mut Inventory inventory, Pickup pickup)
{
    if (!inventory.items.Contains(pickup.item)) inventory.items.Add(pickup.item);
}
```

`var x = [1, 2]` is an error, since it doesn't say the type.

### Members

- `count`, `items[i]` to read, and `items[i] = x` or `items[i] += x` to write.
- `Add(item)`, `Insert(index, item)`, `RemoveAt(index)` and `Clear()`.
- For elements that `==` compares (numbers, bools, enums, text, entities and players): `Contains(item)`, `IndexOf(item)` (-1 if it's not there) and `Remove(item)`, which returns whether it found one.

Past the end, a read gives the element type's zero and a write does nothing.

### Elements are copies

Like C#'s lists of structs, an element is a copy. `items[i].count = 1` is an error: take the element out, change it, and put it back.

```csharp
mut var slot = inventory.slots[i];
slot.count += 1;
inventory.slots[i] = slot;
```

Changing a list needs something that can change: a `mut` component or singleton, or a `mut` local.

Elements are built-in types, text, enums and structs. Lists of lists, and structs holding lists, aren't supported yet. ECS data doesn't go in lists either: keep an `Entity` instead.

### Going through every element at once

A `parallel` loop goes through a list's elements at once, on threads, as it goes through a grid's cells (see [Grids](./grids.md#going-through-every-cell)). Its places are the elements' indices:

```csharp
struct Body
{
    float3 position;
    float3 velocity;
    float mass;
}

singleton Space
{
    List<Body> bodies;
}

// Every body is pulled by every other, from where they all were
system Gravity(mut Space space, Time time)
{
    var n = space.bodies.count;
    parallel (var i in space.bodies)
    {
        mut var body = space.bodies[i];
        mut var pull = float3(0);
        for (var j = 0; j < n; j++)
        {
            var other = space.bodies[j];
            var d = other.position - body.position;
            var r2 = Math.Dot(d, d) + 0.01;
            pull += d * (other.mass / (r2 * Math.Sqrt(r2)));
        }
        body.velocity += pull * time.dt;
        space.bodies[i] = body;
    }
}
```

Each step reads the list as the loop found it and changes only its own element, `space.bodies[i]`, so the result is exactly the same on one thread as on many. `by 2 offset o` gives each step a block of elements, from `i` up to `i + 1`, which it changes as `items[i]` and `items[i + 1]`: a block that would go past the end is left out. The rules are a grid's parallel loop's: a step changes nothing else, not the list's count either, and tidec says what to write instead.

The list is one the world keeps, a component's or a singleton's, and its elements hold no text: the steps run on threads, where text can't be made.

## Where they live

A world keeps the text and lists in its components in its **heap**, which is part of the world. So a snapshot has them with everything else. The heap grows as it needs, with no limit but memory.

Text and lists that code makes along the way, joining text, say, live in a scratch area that's cleared once the system, view or handler is done, and are only copied into the world when they're stored in it. The scratch area grows as it needs too, so what code makes can be of any size; only running out of memory ends the program.

An input can't hold text or lists: what players send every tick is numbers, bools and enums.

Systems that change the match's text or lists wait for each other, since they share one heap.
