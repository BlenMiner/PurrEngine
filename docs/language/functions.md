# Structs and functions

## Structs

A struct is a value type for fields and locals: plain data, copied when it's assigned, with no references.

```csharp
struct Range
{
    float lo;
    float hi = 1;
}

struct Stats
{
    float health = 100;
    Range damage;                   // lo = 0, hi = 1: Range's defaults
    Range armor = Range { hi = 5 };
}

component Unit
{
    Stats stats;
}
```

- A value is written like a component's: `Stats { health = 50 }`. Fields left out take their defaults.
- Components, singletons, inputs and other structs can hold structs, and so can locals. System parameters stay components, singletons, the input and `Devices`.
- A struct can't contain itself, even through other structs.

A struct's fields change through whatever holds it, so the signature still says what a system writes: `unit.stats.health -= 5` needs `mut Unit unit`. A local copy changes if it's `mut var`.

## Methods

Structs and components can have methods. Their fields are in scope by name, and so are their other methods. A method only reads the fields, unless it's `mut`:

```csharp
struct Stats
{
    float health = 100;

    bool IsDead() { return health <= 0; }

    mut void Hurt(float amount)
    {
        health -= amount;
        if (IsDead()) health = 0;
    }
}

system Burn(mut Unit unit)
{
    unit.stats.Hurt(1);
}
```

Calling a `mut` method changes what it's called on, so it needs write access, like an assignment: `unit.stats.Hurt(1)` needs `mut Unit unit`. A read-only method can't call a `mut` one.

Singletons and inputs have no methods of their own: keep the data and its methods in a struct, and the struct in them.

## Functions

A function is code that other code calls. It has no keyword, just a return type:

```csharp
float Heal(mut Stats stats, float amount)
{
    stats.health = Math.Min(stats.health + amount, 100);
    return stats.health;
}

system Regenerate(mut Unit unit)
{
    Heal(unit.stats, 0.5);
}
```

- A parameter is a read-only copy. A `mut` parameter is the caller's variable itself, which the function changes. The caller must be able to write it, so `Heal(unit.stats, 0.5)` needs `mut Unit unit`.
- Parameters and return values are built-in types, structs and components, and an argument's type matches exactly.
- A function returns a value on every path, unless it returns `void`.
- A function or method can fail, with `fails` after its parameters, and its callers handle the error where they call (see [Errors](./errors.md)).
- A function shares its name with nothing else in its namespace. Methods can't share a name either, even with different parameters, or share one with a field.

Functions see their parameters and `Math`, and methods their fields too, but nothing else. Neither can spawn or change entities: systems do. A function can draw and use the GUI, and then only views, and other functions like it, can call it.

A function can also be written in C, declared with `extern` and no body: see [Calling C](c-functions.md).

## Actions

A function's last parameter can be an `Action`: code the caller writes in braces after the call. The function runs it by calling it, as many times as it likes, including none.

```csharp
// A container of your own: shows its content only while open.
void Foldout(string title, mut bool open, Action content)
{
    GUILayout.Toggle(title, open);
    if (open) content();
}

view Options(mut Menu menu, mut Settings settings)
{
    Foldout("Audio", menu.audioOpen)
    {
        GUILayout.Slider("Volume", settings.volume, 0, 1);
    }
}
```

The block runs as if it were written at the call. It sees the caller's locals and parameters, and what it reads and writes counts toward the caller's signature. `return` in it ends the caller, and `break` and `continue` act on the caller's loop.

A function that takes a block is inlined where it's called, so blocks cost nothing and need no closures. That also means it can't call itself, and a block can only be run, never stored.

This is how the GUI's containers work: `GUILayout.Horizontal()`, `GUILayout.Area()` and the others are functions with a block, like `Foldout`.

## Operators

A struct can declare operators, in C#'s form. They compile to plain function calls.

```csharp
struct Money
{
    int cents;

    Money operator +(Money a, Money b) { return Money { cents = a.cents + b.cents }; }
    Money operator *(Money a, int times) { return Money { cents = a.cents * times }; }
    bool operator ==(Money a, Money b) { return a.cents == b.cents; }
    bool operator !=(Money a, Money b) { return !(a == b); }
}
```

- The operators are `+ - * / % & | ^ << >>`, the comparisons `== != < <= > >=`, and `-`, `!` and `~` with one parameter.
- At least one parameter is the struct itself. An operator sees only its parameters, not fields.
- As in C#, `==` and `!=` come in pairs, and so do `<` and `>`, and `<=` and `>=`.
- A compound assignment uses its operator: `total += price` uses `+`.
- Several operators can share a symbol with different types, like `Money * int` and `int * Money`.

Structs are only compared with `==` if they declare it.
