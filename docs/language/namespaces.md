# Namespaces and files

## Files

A game is every `.tide` file in its folder and its subfolders. Every declaration is visible from every file, and there are no imports between files. How you split a game into files is up to you: the editors can move a declaration to a file of its own.

Files compile in order of their paths, and that's the default order of systems and views: by file, then as they're written in each file (see [Systems](./systems.md#order)).

## Namespaces

Large games keep names apart with namespaces. `namespace` at the top of a file puts everything in it in that namespace:

```csharp
// combat.tide
namespace Combat;

component Health { int value = 100; }
```

Code outside the namespace names its declarations with it, `Combat.Health`, or imports the namespace with `using`:

```csharp
// main.tide
using Combat;

scene Main { }

event(Spawned) Setup(with Main)
{
    Spawn(Health, Items.Health { value = 3 });
}
```

- A file has at most one namespace, and files without one are in the global namespace. Namespaces can be dotted: `namespace Game.Combat;`.
- `namespace` and `using` come before any declaration.
- A plain name is looked up in the file's namespace, then the namespaces around it, then the `using` ones, then the global namespace. If two `using` namespaces both have it, it's ambiguous: write the namespace.
- Qualified names work wherever a type is named: parameters (`mut Combat.Health health`), `with` and `without`, component values, `Spawn`, `Add` and `Remove`, and `[Before]` and `[After]` (`[After(Physics.Gravity)]`).
- The same name can be declared in different namespaces.
- There's exactly one `Main` in a game, in any file or namespace.
